// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <errno.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "laghu/cache.h"
#include "server_internal.h"

bool proxy_peer_trusted(const laghu_service_config *service, const proxy_connection *connection) {
  const unsigned char *peer;
  size_t index;
  unsigned int family;
  if (connection->peer.ss_family == AF_INET) {
    peer = (const unsigned char *)&((const struct sockaddr_in *)&connection->peer)->sin_addr;
    family = LAGHU_SERVICE_CIDR_FAMILY_IPV4;
  } else if (connection->peer.ss_family == AF_INET6) {
    peer = (const unsigned char *)&((const struct sockaddr_in6 *)&connection->peer)->sin6_addr;
    family = LAGHU_SERVICE_CIDR_FAMILY_IPV6;
  } else {
    return false;
  }
  for (index = 0U; index < service->trusted_proxy_count; ++index)
    if (laghu_service_cidr_matches(&service->trusted_proxies[index], peer, family)) return true;
  return false;
}

const char *proxy_effective_scheme(const laghu_config *core, const laghu_service_config *service, const proxy_connection *connection,
                                   const proxy_request *request) {
  const proxy_header *header = NULL;
  size_t index;
  if (connection->tls != NULL) return "https";
  if (core->respect_x_forwarded_proto != LAGHU_MODE_ON || !proxy_peer_trusted(service, connection)) return "http";
  for (index = 0U; index < request->header_count; ++index)
    if (proxy_name_equal(request->headers[index].name, "X-Forwarded-Proto")) {
      if (header != NULL) return "http";
      header = &request->headers[index];
    }
  if (header == NULL || strchr(header->value, ',') != NULL) return "http";
  if (proxy_name_equal(header->value, "https")) return "https";
  return "http";
}

bool proxy_peer_in_cidrs(const proxy_connection *connection, const laghu_service_cidr *cidrs, size_t count) {
  const unsigned char *peer;
  unsigned int family;
  size_t index;
  if (connection->peer.ss_family == AF_INET) {
    peer = (const unsigned char *)&((const struct sockaddr_in *)&connection->peer)->sin_addr;
    family = LAGHU_SERVICE_CIDR_FAMILY_IPV4;
  } else if (connection->peer.ss_family == AF_INET6) {
    peer = (const unsigned char *)&((const struct sockaddr_in6 *)&connection->peer)->sin6_addr;
    family = LAGHU_SERVICE_CIDR_FAMILY_IPV6;
  } else {
    return false;
  }
  for (index = 0U; index < count; ++index)
    if (laghu_service_cidr_matches(&cidrs[index], peer, family)) return true;
  return false;
}

static bool proxy_basic_base64(const char *input, unsigned char *output, size_t capacity, size_t *length) {
  size_t input_length;
  size_t index;
  size_t used = 0U;
  if (input == NULL || output == NULL || length == NULL) return false;
  input_length = strlen(input);
  if (input_length == 0U || input_length % 4U != 0U) return false;
  for (index = 0U; index < input_length; index += 4U) {
    unsigned int values[4U];
    size_t part;
    size_t padding = 0U;
    for (part = 0U; part < 4U; ++part) {
      unsigned char value = (unsigned char)input[index + part];
      if (value >= 'A' && value <= 'Z')
        values[part] = value - 'A';
      else if (value >= 'a' && value <= 'z')
        values[part] = value - 'a' + 26U;
      else if (value >= '0' && value <= '9')
        values[part] = value - '0' + 52U;
      else if (value == '+')
        values[part] = 62U;
      else if (value == '/')
        values[part] = 63U;
      else if (value == '=' && index + 4U == input_length && part >= 2U) {
        values[part] = 0U;
        ++padding;
      } else
        return false;
    }
    if (padding != 0U && (padding == 2U ? input[index + 2U] != '=' : input[index + 3U] != '=')) return false;
    if (padding > 2U || (padding != 0U && ((values[1U] & (padding == 2U ? 15U : 3U)) != 0U))) return false;
    if (used > capacity - (3U - padding)) return false;
    output[used++] = (unsigned char)((values[0U] << 2U) | (values[1U] >> 4U));
    if (padding < 2U) output[used++] = (unsigned char)((values[1U] << 4U) | (values[2U] >> 2U));
    if (padding == 0U) output[used++] = (unsigned char)((values[2U] << 6U) | values[3U]);
  }
  *length = used;
  return true;
}

static bool proxy_basic_authorized(const proxy_request *request, const laghu_proxy_rules *rules) {
  const char *authorization;
  unsigned char decoded[512U];
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_length = 0U;
  char *separator;
  size_t decoded_length;
  size_t index;
  unsigned int matched = 0U;
  if ((rules->present & LAGHU_PROXY_RULE_BASIC_AUTH) == 0U) return true;
  authorization = proxy_single_header(request, "Authorization");
  if (authorization == NULL || strncmp(authorization, "Basic ", 6U) != 0 ||
      !proxy_basic_base64(authorization + 6U, decoded, sizeof(decoded) - 1U, &decoded_length))
    return false;
  decoded[decoded_length] = '\0';
  separator = strchr((char *)decoded, ':');
  if (separator == NULL || separator == (char *)decoded || separator[1] == '\0' || strchr(separator + 1U, ':') != NULL) return false;
  *separator++ = '\0';
  if (EVP_Digest(separator, strlen(separator), digest, &digest_length, EVP_sha256(), NULL) != 1 || digest_length != 32U) return false;
  for (index = 0U; index < rules->basic_auth_user_count; ++index) {
    unsigned int username = (unsigned int)(strcmp(rules->basic_auth_users[index].username, (const char *)decoded) == 0);
    unsigned int password = (unsigned int)(CRYPTO_memcmp(rules->basic_auth_users[index].password_hash, digest, 32U) == 0);
    matched |= username & password;
  }
  return matched != 0U;
}

static bool proxy_rate_peer(const proxy_connection *connection, const unsigned char **bytes, size_t *length) {
  if (connection == NULL || bytes == NULL || length == NULL) return false;
  if (connection->peer.ss_family == AF_INET) {
    *bytes = (const unsigned char *)&((const struct sockaddr_in *)&connection->peer)->sin_addr;
    *length = 4U;
    return true;
  }
  if (connection->peer.ss_family == AF_INET6) {
    *bytes = (const unsigned char *)&((const struct sockaddr_in6 *)&connection->peer)->sin6_addr;
    *length = 16U;
    return true;
  }
  return false;
}

static uint64_t proxy_rate_identity(const unsigned char *bytes, size_t length) {
  uint64_t hash = UINT64_C(1469598103934665603);
  size_t index;
  if (bytes == NULL || (length != 4U && length != 16U)) return 0U;
  for (index = 0U; index < length; ++index) {
    hash ^= bytes[index];
    hash *= UINT64_C(1099511628211);
  }
  return hash == 0U ? UINT64_C(1) : hash;
}

static bool proxy_rate_allowed(proxy_queue *queue, const proxy_connection *connection, const laghu_proxy_rules *rules) {
  proxy_rate_bucket *bucket;
  uint64_t identity;
  uint64_t now;
  uint64_t elapsed;
  uint64_t refill;
  const unsigned char *address;
  size_t address_length;
  size_t index;
  size_t candidate = LAGHU_PROXY_RATE_BUCKETS;
  size_t oldest = 0U;
  if ((rules->present & LAGHU_PROXY_RULE_RATE) == 0U) return true;
  if (!proxy_rate_peer(connection, &address, &address_length) || (identity = proxy_rate_identity(address, address_length)) == 0U) return false;
  now = proxy_monotonic_ms();
  proxy_queue_lock(queue);
  for (index = 0U; index < LAGHU_PROXY_RATE_BUCKETS; ++index) {
    proxy_rate_bucket *current = &queue->rate_buckets[(identity + rules->scope_id + index) % LAGHU_PROXY_RATE_BUCKETS];
    if (current->identity == identity && current->scope_id == rules->scope_id && current->address_length == address_length &&
        memcmp(current->address, address, address_length) == 0) {
      candidate = (identity + rules->scope_id + index) % LAGHU_PROXY_RATE_BUCKETS;
      break;
    }
    if (current->identity == 0U && candidate == LAGHU_PROXY_RATE_BUCKETS) candidate = (identity + rules->scope_id + index) % LAGHU_PROXY_RATE_BUCKETS;
    if (queue->rate_buckets[oldest].identity == 0U || (current->identity != 0U && current->updated_ms < queue->rate_buckets[oldest].updated_ms))
      oldest = (identity + rules->scope_id + index) % LAGHU_PROXY_RATE_BUCKETS;
  }
  if (candidate == LAGHU_PROXY_RATE_BUCKETS) candidate = oldest;
  bucket = &queue->rate_buckets[candidate];
  if (bucket->identity != identity || bucket->scope_id != rules->scope_id || bucket->address_length != address_length ||
      memcmp(bucket->address, address, address_length) != 0 || now < bucket->updated_ms) {
    bucket->identity = identity;
    bucket->scope_id = rules->scope_id;
    memcpy(bucket->address, address, address_length);
    bucket->address_length = (unsigned char)address_length;
    bucket->updated_ms = now;
    bucket->tokens_milli = (uint64_t)rules->rate_burst * 1000U;
  }
  elapsed = now - bucket->updated_ms;
  refill = elapsed > UINT64_MAX / rules->rate_per_second ? UINT64_MAX : elapsed * rules->rate_per_second;
  if (refill != 0U) {
    uint64_t ceiling = (uint64_t)rules->rate_burst * 1000U;
    bucket->tokens_milli = bucket->tokens_milli > ceiling - (refill > ceiling ? ceiling : refill) ? ceiling : bucket->tokens_milli + refill;
    bucket->updated_ms = now;
  }
  if (bucket->tokens_milli < 1000U) {
    proxy_queue_unlock(queue);
    return false;
  }
  bucket->tokens_milli -= 1000U;
  proxy_queue_unlock(queue);
  return true;
}

bool proxy_request_access_allowed(proxy_queue *queue, const proxy_connection *connection, const proxy_request *request,
                                  const laghu_proxy_rules *rules, const char **failure) {
  if (failure != NULL) *failure = "none";
  if (rules == NULL || queue == NULL || connection == NULL || request == NULL) return false;
  if (rules->deny_count != 0U && proxy_peer_in_cidrs(connection, rules->deny, rules->deny_count)) {
    if (failure != NULL) *failure = "cidr_deny";
    return false;
  }
  if (rules->allow_count != 0U && !proxy_peer_in_cidrs(connection, rules->allow, rules->allow_count)) {
    if (failure != NULL) *failure = "cidr_allow";
    return false;
  }
  if (!proxy_basic_authorized(request, rules)) {
    if (failure != NULL) *failure = "basic_auth";
    return false;
  }
  if (!proxy_rate_allowed(queue, connection, rules)) {
    if (failure != NULL) *failure = "rate_limit";
    return false;
  }
  return true;
}

bool proxy_admin_token(const laghu_service_config *service, const proxy_request *request) {
  proxy_header *provided = proxy_find((proxy_header *)request->headers, request->header_count, "X-Laghu-Purge-Token");
  unsigned char expected[257U];
  size_t length, provided_length, index, maximum;
  unsigned char difference = 0U;
  FILE *file;
  struct stat status;
  if (lstat(service->purge_token_file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
      (status.st_mode & (S_IRWXG | S_IRWXO)) != 0U)
    return false;
  if (provided == NULL) return false;
  file = fopen(service->purge_token_file, "rb");
  if (file == NULL) return false;
  length = fread(expected, 1U, sizeof(expected), file);
  if (fclose(file) != 0 || length == 0U || length == sizeof(expected)) return false;
  while (length != 0U && (expected[length - 1U] == '\n' || expected[length - 1U] == '\r')) --length;
  if (length < 16U) return false;
  for (index = 0U; index < length; ++index)
    if (expected[index] <= 0x20U || expected[index] == 0x7fU) return false;
  provided_length = strlen(provided->value);
  maximum = length > provided_length ? length : provided_length;
  difference = (unsigned char)(length ^ provided_length);
  for (index = 0U; index < maximum; ++index) {
    unsigned char left = index < length ? expected[index] : 0U;
    unsigned char right = index < provided_length ? (unsigned char)provided->value[index] : 0U;
    difference |= (unsigned char)(left ^ right);
  }
  return difference == 0U;
}

void proxy_poll_flush_file(const laghu_proxy_options *options) {
  if (options->service.cache_flush_file[0] != '\0')
    (void)laghu_cache_flush_file_poll(options->service.image_cache, options->service.cache_flush_file, (uint64_t)time(NULL), NULL);
}

bool proxy_peer_text(const proxy_connection *connection, char *output, size_t capacity, bool bracket_ipv6) {
  const void *address;
  char raw[INET6_ADDRSTRLEN];
  int family = connection->peer.ss_family;
  if (family == AF_INET)
    address = &((const struct sockaddr_in *)&connection->peer)->sin_addr;
  else if (family == AF_INET6)
    address = &((const struct sockaddr_in6 *)&connection->peer)->sin6_addr;
  else
    return false;
  if (inet_ntop(family, address, raw, sizeof(raw)) == NULL) return false;
  if (family == AF_INET6 && bracket_ipv6) {
    int count = snprintf(output, capacity, "\"[%s]\"", raw);
    return count > 0 && (size_t)count < capacity;
  }
  return *raw != '\0' && laghu_base_string_copy(output, capacity, raw);
}

const char *proxy_single_header(const proxy_request *request, const char *name) {
  const char *value = NULL;
  size_t index;
  for (index = 0U; index < request->header_count; ++index) {
    size_t offset;
    if (!proxy_name_equal(request->headers[index].name, name)) continue;
    if (value != NULL || request->headers[index].value[0] == '\0' || strlen(request->headers[index].value) > LAGHU_HTTP_MAX_HEADER_VALUE) return NULL;
    for (offset = 0U; request->headers[index].value[offset] != '\0'; ++offset)
      if ((unsigned char)request->headers[index].value[offset] < 32U || (unsigned char)request->headers[index].value[offset] == 127U) return NULL;
    value = request->headers[index].value;
  }
  return value;
}

bool proxy_forwarded_value_valid(const char *value) {
  bool quoted = false, escaped = false, content = false, equals = false;
  size_t index;
  for (index = 0U; value[index] != '\0'; ++index) {
    unsigned char byte = (unsigned char)value[index];
    if (escaped) {
      escaped = false;
      content = true;
    } else if (quoted && byte == '\\') {
      escaped = true;
    } else if (byte == '"') {
      quoted = !quoted;
      content = true;
    } else if (!quoted && byte == ',') {
      if (!content || !equals) return false;
      content = false;
      equals = false;
    } else if (!quoted && byte == '=') {
      equals = true;
      content = true;
    } else if (byte != ' ' && byte != '\t') {
      content = true;
    }
  }
  return content && equals && !quoted && !escaped;
}

bool proxy_xff_value_valid(const char *value) {
  const char *cursor = value;
  while (*cursor != '\0') {
    const char *comma = strchr(cursor, ',');
    const char *end = comma == NULL ? cursor + strlen(cursor) : comma;
    char address[INET6_ADDRSTRLEN];
    unsigned char binary[16];
    size_t length;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    while (end > cursor && (end[-1] == ' ' || end[-1] == '\t')) --end;
    length = (size_t)(end - cursor);
    if (length == 0U || length >= sizeof(address)) return false;
    memcpy(address, cursor, length);
    address[length] = '\0';
    if (inet_pton(AF_INET, address, binary) != 1 && inet_pton(AF_INET6, address, binary) != 1) return false;
    cursor = comma == NULL ? end : comma + 1;
  }
  return true;
}

bool proxy_append_line(char *output, size_t capacity, size_t *length, const char *name, const char *existing, const char *value) {
  int count =
      snprintf(output + *length, capacity - *length, "%s: %s%s%s\r\n", name, existing == NULL ? "" : existing, existing == NULL ? "" : ", ", value);
  if (count <= 0 || (size_t)count >= capacity - *length) return false;
  *length += (size_t)count;
  return true;
}

bool proxy_append_forwarding(laghu_proxy_forwarded_mode forwarded_mode, const laghu_config *core, const laghu_service_config *service,
                             const proxy_connection *connection, const proxy_request *request, const char *host, char *output, size_t capacity,
                             size_t *length) {
  bool trusted = proxy_peer_trusted(service, connection);
  char peer[INET6_ADDRSTRLEN + 4U];
  const char *scheme = proxy_effective_scheme(core, service, connection, request);
  const char *existing;
  size_t host_index;
  if (forwarded_mode == LAGHU_PROXY_FORWARDED_OFF) return true;
  for (host_index = 0U; host[host_index] != '\0'; ++host_index)
    if (!isalnum((unsigned char)host[host_index]) && host[host_index] != '.' && host[host_index] != '-' && host[host_index] != ':' &&
        host[host_index] != '[' && host[host_index] != ']')
      return false;
  if (forwarded_mode == LAGHU_PROXY_FORWARDED_STANDARD || forwarded_mode == LAGHU_PROXY_FORWARDED_BOTH) {
    char standard[INET6_ADDRSTRLEN + 300U];
    if (!proxy_peer_text(connection, peer, sizeof(peer), true) ||
        snprintf(standard, sizeof(standard), "for=%s;proto=%s;host=\"%s\"", peer, scheme, host) <= 0)
      return false;
    existing = trusted ? proxy_single_header(request, "Forwarded") : NULL;
    if (existing != NULL && !proxy_forwarded_value_valid(existing)) existing = NULL;
    if (!proxy_append_line(output, capacity, length, "Forwarded", existing, standard)) return false;
  }
  if (forwarded_mode == LAGHU_PROXY_FORWARDED_X || forwarded_mode == LAGHU_PROXY_FORWARDED_BOTH) {
    if (!proxy_peer_text(connection, peer, sizeof(peer), false)) return false;
    existing = trusted ? proxy_single_header(request, "X-Forwarded-For") : NULL;
    if (existing != NULL && !proxy_xff_value_valid(existing)) existing = NULL;
    if (!proxy_append_line(output, capacity, length, "X-Forwarded-For", existing, peer) ||
        !proxy_append_line(output, capacity, length, "X-Forwarded-Proto", NULL, scheme) ||
        !proxy_append_line(output, capacity, length, "X-Forwarded-Host", NULL, host))
      return false;
  }
  return true;
}
