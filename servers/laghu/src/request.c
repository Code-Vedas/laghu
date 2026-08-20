// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "laghu/cache.h"
#include "server_internal.h"

bool proxy_peer_trusted(const laghu_proxy_options *options, const proxy_connection *connection) {
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
  for (index = 0U; index < options->service.trusted_proxy_count; ++index)
    if (laghu_service_cidr_matches(&options->service.trusted_proxies[index], peer, family)) return true;
  return false;
}

const char *proxy_effective_scheme(const laghu_proxy_options *options, const proxy_connection *connection, const proxy_request *request) {
  const proxy_header *header = NULL;
  size_t index;
  if (connection->tls != NULL) return "https";
  if (options->config.respect_x_forwarded_proto != LAGHU_MODE_ON || !proxy_peer_trusted(options, connection)) return "http";
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

bool proxy_admin_token(const laghu_proxy_options *options, const proxy_request *request) {
  proxy_header *provided = proxy_find((proxy_header *)request->headers, request->header_count, "X-Laghu-Purge-Token");
  unsigned char expected[257U];
  size_t length, provided_length, index, maximum;
  unsigned char difference = 0U;
  FILE *file;
  struct stat status;
  if (lstat(options->service.purge_token_file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
      (status.st_mode & (S_IRWXG | S_IRWXO)) != 0U)
    return false;
  if (provided == NULL) return false;
  file = fopen(options->service.purge_token_file, "rb");
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

bool proxy_append_forwarding(const laghu_proxy_options *options, const proxy_connection *connection, const proxy_request *request, const char *host,
                             char *output, size_t capacity, size_t *length) {
  bool trusted = proxy_peer_trusted(options, connection);
  char peer[INET6_ADDRSTRLEN + 4U];
  const char *scheme = proxy_effective_scheme(options, connection, request);
  const char *existing;
  size_t host_index;
  if (options->forwarded_mode == LAGHU_PROXY_FORWARDED_OFF) return true;
  for (host_index = 0U; host[host_index] != '\0'; ++host_index)
    if (!isalnum((unsigned char)host[host_index]) && host[host_index] != '.' && host[host_index] != '-' && host[host_index] != ':' &&
        host[host_index] != '[' && host[host_index] != ']')
      return false;
  if (options->forwarded_mode == LAGHU_PROXY_FORWARDED_STANDARD || options->forwarded_mode == LAGHU_PROXY_FORWARDED_BOTH) {
    char standard[INET6_ADDRSTRLEN + 300U];
    if (!proxy_peer_text(connection, peer, sizeof(peer), true) ||
        snprintf(standard, sizeof(standard), "for=%s;proto=%s;host=\"%s\"", peer, scheme, host) <= 0)
      return false;
    existing = trusted ? proxy_single_header(request, "Forwarded") : NULL;
    if (existing != NULL && !proxy_forwarded_value_valid(existing)) existing = NULL;
    if (!proxy_append_line(output, capacity, length, "Forwarded", existing, standard)) return false;
  }
  if (options->forwarded_mode == LAGHU_PROXY_FORWARDED_X || options->forwarded_mode == LAGHU_PROXY_FORWARDED_BOTH) {
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
