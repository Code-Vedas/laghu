// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "laghu/cache.h"
#include "server_internal.h"

#ifdef O_NOFOLLOW
#define LAGHU_PROXY_ADMIN_TOKEN_NOFOLLOW O_NOFOLLOW
#else
/* POSIX implementations without O_NOFOLLOW still validate that the opened
 * descriptor is the lstat inode before any token byte is consumed. */
#define LAGHU_PROXY_ADMIN_TOKEN_NOFOLLOW 0
#endif

#ifdef O_CLOEXEC
#define LAGHU_PROXY_ADMIN_TOKEN_CLOEXEC O_CLOEXEC
#else
#define LAGHU_PROXY_ADMIN_TOKEN_CLOEXEC 0
#endif

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

static bool proxy_rate_peer(const proxy_connection *connection, const unsigned char **bytes, size_t *length);
static uint64_t proxy_rate_identity(const unsigned char *bytes, size_t length);

static bool proxy_auth_refill(uint64_t *tokens, uint64_t *updated, uint64_t now, unsigned int rate, unsigned int burst) {
  uint64_t elapsed;
  uint64_t refill;
  uint64_t ceiling;
  if (tokens == NULL || updated == NULL || rate == 0U || burst == 0U) return false;
  ceiling = (uint64_t)burst * 1000U;
  if (*updated == 0U || now < *updated) {
    *updated = now;
    *tokens = ceiling;
    return true;
  }
  elapsed = now - *updated;
  refill = elapsed > UINT64_MAX / rate ? UINT64_MAX : elapsed * rate;
  if (refill != 0U) {
    uint64_t added = refill > ceiling ? ceiling : refill;
    *tokens = *tokens > ceiling - added ? ceiling : *tokens + added;
    *updated = now;
  }
  return true;
}

static bool proxy_auth_preauth_allowed(proxy_queue *queue, const laghu_proxy_options *options, const proxy_connection *connection,
                                       const laghu_proxy_rules *rules) {
  const unsigned char *address;
  size_t address_length;
  uint64_t identity;
  uint64_t now;
  size_t index;
  size_t candidate = LAGHU_PROXY_AUTH_BUCKETS;
  size_t oldest = LAGHU_PROXY_AUTH_BUCKETS;
  proxy_rate_bucket *bucket;
  if (queue == NULL || options == NULL || rules == NULL || !proxy_rate_peer(connection, &address, &address_length) ||
      (identity = proxy_rate_identity(address, address_length)) == 0U)
    return false;
  now = proxy_monotonic_ms();
  proxy_queue_lock(queue);
  if (queue->stopping || queue->state == PROXY_FORCING) goto rejected;
  for (index = 0U; index < LAGHU_PROXY_AUTH_BUCKETS; ++index) {
    size_t slot = (identity + rules->scope_id + index) % LAGHU_PROXY_AUTH_BUCKETS;
    proxy_rate_bucket *current = &queue->auth_buckets[slot];
    if (current->identity == identity && current->scope_id == rules->scope_id && current->address_length == address_length &&
        memcmp(current->address, address, address_length) == 0) {
      candidate = slot;
      break;
    }
    if (current->identity == 0U && candidate == LAGHU_PROXY_AUTH_BUCKETS) candidate = slot;
    if (current->identity != 0U && (oldest == LAGHU_PROXY_AUTH_BUCKETS || current->updated_ms < queue->auth_buckets[oldest].updated_ms))
      oldest = slot;
  }
  if (candidate == LAGHU_PROXY_AUTH_BUCKETS) candidate = oldest;
  if (candidate == LAGHU_PROXY_AUTH_BUCKETS) goto rejected;
  bucket = &queue->auth_buckets[candidate];
  if (bucket->identity != identity || bucket->scope_id != rules->scope_id || bucket->address_length != address_length ||
      memcmp(bucket->address, address, address_length) != 0) {
    memset(bucket, 0, sizeof(*bucket));
    bucket->identity = identity;
    bucket->scope_id = rules->scope_id;
    memcpy(bucket->address, address, address_length);
    bucket->address_length = (unsigned char)address_length;
  }
  if (!proxy_auth_refill(&bucket->tokens_milli, &bucket->updated_ms, now, options->auth_pre_rate, options->auth_pre_burst) ||
      bucket->tokens_milli < 1000U)
    goto rejected;
  bucket->tokens_milli -= 1000U;
  proxy_queue_unlock(queue);
  return true;
rejected:
  ++queue->auth_preauth_rejected;
  proxy_queue_unlock(queue);
  return false;
}

static bool proxy_auth_kdf_begin(proxy_queue *queue, const laghu_proxy_options *options) {
  uint64_t now;
  if (queue == NULL || options == NULL) return false;
  now = proxy_monotonic_ms();
  proxy_queue_lock(queue);
  if (queue->stopping || queue->state == PROXY_FORCING || queue->auth_kdf_active >= options->auth_kdf_concurrency ||
      !proxy_auth_refill(&queue->auth_kdf_tokens_milli, &queue->auth_kdf_updated_ms, now, options->auth_kdf_rate, options->auth_kdf_burst) ||
      queue->auth_kdf_tokens_milli < 1000U) {
    ++queue->auth_kdf_saturated;
    proxy_queue_unlock(queue);
    return false;
  }
  queue->auth_kdf_tokens_milli -= 1000U;
  ++queue->auth_kdf_starts;
  ++queue->auth_kdf_active;
  if (queue->auth_kdf_active > queue->auth_kdf_highwater) queue->auth_kdf_highwater = queue->auth_kdf_active;
  proxy_queue_unlock(queue);
  return true;
}

static void proxy_auth_kdf_end(proxy_queue *queue) {
  if (queue == NULL) return;
  proxy_queue_lock(queue);
  if (queue->auth_kdf_active != 0U) --queue->auth_kdf_active;
  proxy_queue_unlock(queue);
}

static bool proxy_basic_password_matches(proxy_queue *queue, const laghu_proxy_options *options, const laghu_proxy_basic_auth_user *user,
                                         const char *password, bool *saturated) {
  unsigned char derived[32U];
  unsigned int derived_length = 0U;
  bool matched = false;
  bool kdf_active = false;
  if (saturated != NULL) *saturated = false;
  if (user == NULL || password == NULL) return false;
  if (user->password_kdf == LAGHU_PROXY_BASIC_AUTH_SHA256) {
    if (EVP_Digest(password, strlen(password), derived, &derived_length, EVP_sha256(), NULL) != 1 || derived_length != sizeof(derived)) goto done;
  } else if (user->password_kdf == LAGHU_PROXY_BASIC_AUTH_SCRYPT_V1) {
    if (!proxy_auth_kdf_begin(queue, options)) {
      if (saturated != NULL) *saturated = true;
      goto done;
    }
    kdf_active = true;
    if (EVP_PBE_scrypt(password, strlen(password), user->password_salt, sizeof(user->password_salt), LAGHU_PROXY_BASIC_AUTH_SCRYPT_N,
                       LAGHU_PROXY_BASIC_AUTH_SCRYPT_R, LAGHU_PROXY_BASIC_AUTH_SCRYPT_P, LAGHU_PROXY_BASIC_AUTH_SCRYPT_MAX_MEMORY, derived,
                       sizeof(derived)) != 1)
      goto done;
  } else {
    goto done;
  }
  matched = CRYPTO_memcmp(user->password_hash, derived, sizeof(derived)) == 0;
done:
  OPENSSL_cleanse(derived, sizeof(derived));
  if (kdf_active) proxy_auth_kdf_end(queue);
  return matched;
}

static bool proxy_basic_authorized(proxy_queue *queue, const laghu_proxy_options *options, const laghu_config *core,
                                   const laghu_service_config *service, const proxy_connection *connection, const proxy_request *request,
                                   const laghu_proxy_rules *rules, bool *saturated) {
  const char *authorization;
  unsigned char decoded[512U];
  char *separator;
  size_t decoded_length;
  size_t index;
  bool matched = false;
  if ((rules->present & LAGHU_PROXY_RULE_BASIC_AUTH) == 0U) return true;
  if (core == NULL || service == NULL || connection == NULL || strcmp(proxy_effective_scheme(core, service, connection, request), "https") != 0)
    return false;
  authorization = proxy_single_header(request, "Authorization");
  if (authorization == NULL || strncmp(authorization, "Basic ", 6U) != 0 ||
      !proxy_basic_base64(authorization + 6U, decoded, sizeof(decoded) - 1U, &decoded_length))
    goto done;
  decoded[decoded_length] = '\0';
  separator = strchr((char *)decoded, ':');
  if (separator == NULL || separator == (char *)decoded || separator[1] == '\0' || strchr(separator + 1U, ':') != NULL) goto done;
  *separator++ = '\0';
  for (index = 0U; index < rules->basic_auth_user_count; ++index) {
    if (strcmp(rules->basic_auth_users[index].username, (const char *)decoded) == 0) {
      matched = proxy_basic_password_matches(queue, options, &rules->basic_auth_users[index], separator, saturated);
      break;
    }
  }
done:
  OPENSSL_cleanse(decoded, sizeof(decoded));
  return matched;
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

bool proxy_request_access_allowed(proxy_queue *queue, const laghu_config *core, const laghu_service_config *service,
                                  const laghu_proxy_options *options, const proxy_connection *connection, const proxy_request *request,
                                  const laghu_proxy_rules *rules, const char **failure) {
  const char *authorization;
  bool saturated = false;
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
  if ((rules->present & LAGHU_PROXY_RULE_BASIC_AUTH) != 0U) {
    /* Only a syntactic Basic candidate spends admission. This remains before
     * decoding, so malformed Basic credentials cannot bypass the KDF gate. */
    if (core == NULL || service == NULL || strcmp(proxy_effective_scheme(core, service, connection, request), "https") != 0) {
      if (failure != NULL) *failure = "basic_auth";
      return false;
    }
    authorization = proxy_single_header(request, "Authorization");
    if (authorization != NULL && strncmp(authorization, "Basic ", 6U) == 0 && !proxy_auth_preauth_allowed(queue, options, connection, rules)) {
      if (failure != NULL) *failure = "preauth_rate";
      return false;
    }
  }
  if (!proxy_basic_authorized(queue, options, core, service, connection, request, rules, &saturated)) {
    proxy_queue_lock(queue);
    if (!saturated) ++queue->auth_credential_failed;
    proxy_queue_unlock(queue);
    if (saturated) {
      if (failure != NULL) *failure = "kdf_saturated";
      return false;
    }
    if (failure != NULL) *failure = "basic_auth";
    return false;
  }
  if (!proxy_rate_allowed(queue, connection, rules)) {
    if ((rules->present & LAGHU_PROXY_RULE_BASIC_AUTH) != 0U) {
      proxy_queue_lock(queue);
      ++queue->auth_postauth_rejected;
      proxy_queue_unlock(queue);
    }
    if (failure != NULL) *failure = "rate_limit";
    return false;
  }
  return true;
}

void proxy_auth_state_clear(proxy_queue *queue) {
  if (queue == NULL) return;
  proxy_queue_lock(queue);
  memset(queue->auth_buckets, 0, sizeof(queue->auth_buckets));
  queue->auth_kdf_updated_ms = 0U;
  queue->auth_kdf_tokens_milli = 0U;
  queue->auth_kdf_highwater = 0U;
  proxy_queue_unlock(queue);
}

bool proxy_auth_metrics_append(proxy_queue *queue, char *output, size_t capacity, size_t *length) {
  uint64_t starts, preauth, saturated, credentials, postauth;
  unsigned int active, highwater;
  int written;
  if (queue == NULL || output == NULL || length == NULL || *length >= capacity) return false;
  proxy_queue_lock(queue);
  starts = queue->auth_kdf_starts;
  preauth = queue->auth_preauth_rejected;
  saturated = queue->auth_kdf_saturated;
  credentials = queue->auth_credential_failed;
  postauth = queue->auth_postauth_rejected;
  active = queue->auth_kdf_active;
  highwater = queue->auth_kdf_highwater;
  proxy_queue_unlock(queue);
  written = snprintf(output + *length, capacity - *length,
                     "# TYPE laghu_auth_kdf_starts_total counter\n"
                     "laghu_auth_kdf_starts_total %llu\n"
                     "# TYPE laghu_auth_admissions_total counter\n"
                     "laghu_auth_admissions_total{result=\"preauth_rejected\"} %llu\n"
                     "laghu_auth_admissions_total{result=\"kdf_saturated\"} %llu\n"
                     "laghu_auth_admissions_total{result=\"credential_failed\"} %llu\n"
                     "laghu_auth_admissions_total{result=\"postauth_rate_rejected\"} %llu\n"
                     "# TYPE laghu_auth_kdf_active gauge\nlaghu_auth_kdf_active %u\n"
                     "# TYPE laghu_auth_kdf_highwater gauge\nlaghu_auth_kdf_highwater %u\n",
                     (unsigned long long)starts, (unsigned long long)preauth, (unsigned long long)saturated, (unsigned long long)credentials,
                     (unsigned long long)postauth, active, highwater);
  if (written < 0 || (size_t)written >= capacity - *length) return false;
  *length += (size_t)written;
  return true;
}

static int proxy_admin_token_open(const char *path) {
  int descriptor;
  int flags;
  if (path == NULL) return -1;
  flags = O_RDONLY | LAGHU_PROXY_ADMIN_TOKEN_NOFOLLOW | LAGHU_PROXY_ADMIN_TOKEN_CLOEXEC;
  descriptor = open(path, flags);
  if (descriptor < 0) return -1;
#ifndef O_CLOEXEC
  /* This fallback explicitly sets close-on-exec before metadata validation or
   * reading. O_NOFOLLOW is optional only because the lstat/fstat inode pairing
   * below rejects any replacement before consuming a byte. */
  flags = fcntl(descriptor, F_GETFD);
  if (flags < 0 || fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
    (void)close(descriptor);
    return -1;
  }
#endif
  return descriptor;
}

bool proxy_admin_token_file_read(const char *path, unsigned char output[257U], size_t *length, proxy_admin_token_file_hook before_open,
                                 proxy_admin_token_file_hook after_open, void *context) {
  struct stat listed;
  struct stat opened;
  int descriptor = -1;
  size_t used = 0U;
  bool valid = false;
  if (output == NULL || length == NULL) return false;
  *length = 0U;
  OPENSSL_cleanse(output, 257U);
  if (path == NULL || lstat(path, &listed) != 0 || !S_ISREG(listed.st_mode) || listed.st_uid != geteuid() ||
      (listed.st_mode & (S_IRWXG | S_IRWXO)) != 0U)
    return false;
  if (before_open != NULL) before_open(path, context);
  descriptor = proxy_admin_token_open(path);
  if (descriptor < 0 || fstat(descriptor, &opened) != 0 || opened.st_dev != listed.st_dev || opened.st_ino != listed.st_ino ||
      !S_ISREG(opened.st_mode) || opened.st_uid != geteuid() || (opened.st_mode & (S_IRWXG | S_IRWXO)) != 0U || opened.st_size <= 0 ||
      opened.st_size >= (off_t)257U)
    goto done;
  if (after_open != NULL) after_open(path, context);
  while (used < 257U) {
    ssize_t received = read(descriptor, output + used, 257U - used);
    if (received > 0) {
      used += (size_t)received;
    } else if (received == 0) {
      break;
    } else if (errno != EINTR) {
      goto done;
    }
  }
  if (used != 0U && used != 257U) {
    *length = used;
    valid = true;
  }
done:
  if (descriptor >= 0 && close(descriptor) != 0) valid = false;
  if (!valid) {
    OPENSSL_cleanse(output, 257U);
    *length = 0U;
  }
  return valid;
}

bool proxy_admin_token(const laghu_service_config *service, const proxy_request *request) {
  proxy_header *provided;
  unsigned char expected[257U];
  size_t length, provided_length, index, maximum;
  unsigned char difference = 0U;
  bool authorized = false;
  if (service == NULL || request == NULL) return false;
  provided = proxy_find((proxy_header *)request->headers, request->header_count, "X-Laghu-Purge-Token");
  if (provided == NULL || !proxy_admin_token_file_read(service->purge_token_file, expected, &length, NULL, NULL, NULL)) return false;
  while (length != 0U && (expected[length - 1U] == '\n' || expected[length - 1U] == '\r')) --length;
  if (length < 16U) goto done;
  for (index = 0U; index < length; ++index)
    if (expected[index] <= 0x20U || expected[index] == 0x7fU) goto done;
  provided_length = strlen(provided->value);
  maximum = length > provided_length ? length : provided_length;
  difference = (unsigned char)(length ^ provided_length);
  for (index = 0U; index < maximum; ++index) {
    unsigned char left = index < length ? expected[index] : 0U;
    unsigned char right = index < provided_length ? (unsigned char)provided->value[index] : 0U;
    difference |= (unsigned char)(left ^ right);
  }
  authorized = difference == 0U;
done:
  OPENSSL_cleanse(expected, sizeof(expected));
  return authorized;
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
