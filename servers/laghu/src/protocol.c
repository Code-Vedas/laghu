// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "server_internal.h"

static int laghu_proxy_hex(unsigned char byte) {
  if (byte >= '0' && byte <= '9') return byte - '0';
  if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
  if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
  return -1;
}

bool laghu_proxy_decode_chunked(laghu_buffer encoded, unsigned char *decoded, size_t capacity, size_t *decoded_length) {
  size_t input = 0U, output = 0U;
  if (decoded == NULL || decoded_length == NULL || (encoded.data == NULL && encoded.length != 0U)) return false;
  while (input < encoded.length) {
    size_t size = 0U;
    bool digit = false;
    while (input < encoded.length && encoded.data[input] != '\r') {
      int value;
      if (encoded.data[input] == ';') {
        while (input < encoded.length && encoded.data[input] != '\r') ++input;
        break;
      }
      value = laghu_proxy_hex(encoded.data[input++]);
      if (value < 0 || size > (SIZE_MAX - (size_t)value) / 16U) return false;
      digit = true;
      size = size * 16U + (size_t)value;
    }
    if (!digit || input + 1U >= encoded.length || encoded.data[input] != '\r' || encoded.data[input + 1U] != '\n') return false;
    input += 2U;
    if (size == 0U) {
      if (input + 1U == encoded.length && encoded.data[input] == '\n') {
        *decoded_length = output;
        return true;
      }
      if (input + 1U >= encoded.length || encoded.data[input] != '\r' || encoded.data[input + 1U] != '\n') return false;
      *decoded_length = output;
      return input + 2U == encoded.length;
    }
    if (size > capacity - output || size > encoded.length - input) return false;
    memcpy(decoded + output, encoded.data + input, size);
    output += size;
    input += size;
    if (input + 1U >= encoded.length || encoded.data[input] != '\r' || encoded.data[input + 1U] != '\n') return false;
    input += 2U;
  }
  return false;
}

bool proxy_name_equal(const char *left, const char *right) {
  while (*left != '\0' && *right != '\0') {
    if (tolower((unsigned char)*left++) != tolower((unsigned char)*right++)) return false;
  }
  return *left == '\0' && *right == '\0';
}

proxy_header *proxy_find(proxy_header *headers, size_t count, const char *name) {
  size_t index;
  for (index = 0U; index < count; ++index)
    if (proxy_name_equal(headers[index].name, name)) return &headers[index];
  return NULL;
}

size_t proxy_header_count(proxy_header *headers, size_t count, const char *name) {
  size_t index, found = 0U;
  for (index = 0U; index < count; ++index)
    if (proxy_name_equal(headers[index].name, name)) ++found;
  return found;
}

bool proxy_parse_headers(char *storage, size_t length, char **first_line, proxy_header *headers, size_t *count, size_t maximum) {
  char *cursor, *end;
  size_t found = 0U;
  if (length < 4U) return false;
  cursor = storage;
  end = strstr(cursor, "\r\n");
  if (end == NULL || (size_t)(end - cursor) > LAGHU_PROXY_LINE_BYTES) return false;
  *end = '\0';
  *first_line = cursor;
  cursor = end + 2U;
  while (*cursor != '\0') {
    char *colon;
    end = strstr(cursor, "\r\n");
    if (end == NULL) return false;
    if (end == cursor) {
      *count = found;
      return true;
    }
    if (*cursor == ' ' || *cursor == '\t' || found == maximum) return false;
    *end = '\0';
    colon = strchr(cursor, ':');
    if (colon == NULL || colon == cursor || (size_t)(colon - cursor) > LAGHU_HTTP_MAX_HEADER_NAME) return false;
    {
      char *byte;
      for (byte = cursor; byte < colon; ++byte)
        if (!isalnum((unsigned char)*byte) && strchr("!#$%&'*+-.^_`|~", *byte) == NULL) return false;
    }
    *colon++ = '\0';
    while (*colon == ' ' || *colon == '\t') ++colon;
    if (strlen(colon) > LAGHU_HTTP_MAX_HEADER_VALUE) return false;
    {
      const unsigned char *byte = (const unsigned char *)colon;
      while (*byte != '\0') {
        if ((*byte < 32U && *byte != '\t') || *byte == 127U) return false;
        ++byte;
      }
    }
    headers[found++] = (proxy_header){cursor, colon};
    cursor = end + 2U;
  }
  return false;
}

bool proxy_content_length(proxy_header *headers, size_t count, size_t *value, bool *present) {
  size_t index;
  unsigned long parsed = 0U;
  bool seen = false;
  for (index = 0U; index < count; ++index)
    if (proxy_name_equal(headers[index].name, "Content-Length")) {
      char *end = NULL;
      unsigned long current;
      errno = 0;
      current = strtoul(headers[index].value, &end, 10);
      if (errno || end == headers[index].value || *end || (seen && parsed != current)) return false;
      parsed = current;
      seen = true;
    }
  *value = (size_t)parsed;
  *present = seen;
  return true;
}

static bool proxy_request_method_valid(const char *method) {
  const unsigned char *byte = (const unsigned char *)method;
  if (*byte == '\0') return false;
  while (*byte != '\0') {
    if (!isalnum(*byte) && strchr("!#$%&'*+-.^_`|~", *byte) == NULL) return false;
    ++byte;
  }
  return true;
}

/* Standalone is origin-form only. Keep percent escapes opaque: routing and
 * upstream forwarding already preserve their current encoded-path contract. */
static bool proxy_request_target_valid(const char *target) {
  const unsigned char *byte = (const unsigned char *)target;
  if (*byte++ != '/') return false;
  while (*byte != '\0') {
    if (*byte <= 32U || *byte == 127U || *byte == '#') return false;
    ++byte;
  }
  return true;
}

bool proxy_parse_request(proxy_request *request, size_t length) {
  char *line, *space1, *space2;
  proxy_header *host, *transfer, *expect, *upgrade;
  if (!proxy_parse_headers(request->storage, length, &line, request->headers, &request->header_count, LAGHU_HTTP_MAX_REQUEST_HEADERS)) return false;
  space1 = strchr(line, ' ');
  if (space1 == NULL) return false;
  *space1++ = '\0';
  space2 = strchr(space1, ' ');
  if (space2 == NULL || strchr(space2 + 1, ' ') != NULL) return false;
  *space2++ = '\0';
  if (!proxy_request_method_valid(line) || !proxy_request_target_valid(space1) || *space2 == '\0' ||
      !laghu_base_string_copy(request->method, sizeof(request->method), line) ||
      !laghu_base_string_copy(request->target, sizeof(request->target), space1) ||
      !laghu_base_string_copy(request->version, sizeof(request->version), space2) ||
      (strcmp(request->version, "HTTP/1.1") && strcmp(request->version, "HTTP/1.0")))
    return false;
  host = proxy_find(request->headers, request->header_count, "Host");
  if ((!strcmp(request->version, "HTTP/1.1") && host == NULL) || proxy_header_count(request->headers, request->header_count, "Host") > 1U)
    return false;
  transfer = proxy_find(request->headers, request->header_count, "Transfer-Encoding");
  expect = proxy_find(request->headers, request->header_count, "Expect");
  upgrade = proxy_find(request->headers, request->header_count, "Upgrade");
  if (!proxy_content_length(request->headers, request->header_count, &request->content_length, &request->has_content_length)) return false;
  request->expect = expect != NULL;
  request->upgrade = upgrade != NULL;
  request->chunked = transfer != NULL;
  return !(request->chunked && request->has_content_length);
}

bool proxy_parse_response(proxy_response *response, size_t length) {
  char *line, *space1, *space2;
  proxy_header *transfer;
  uint64_t parsed_status;
  if (!proxy_parse_headers(response->storage, length, &line, response->headers, &response->header_count, LAGHU_HTTP_MAX_RESPONSE_HEADERS))
    return false;
  space1 = strchr(line, ' ');
  if (space1 == NULL) return false;
  *space1++ = '\0';
  space2 = strchr(space1, ' ');
  if (space2 == NULL) return false;
  *space2++ = '\0';
  if (strcmp(line, "HTTP/1.1") && strcmp(line, "HTTP/1.0")) return false;
  if (!laghu_base_string_copy(response->version, sizeof(response->version), line)) return false;
  if (strlen(space1) != 3U || !laghu_base_parse_u64(space1, 100U, 599U, &parsed_status)) return false;
  response->status = (unsigned int)parsed_status;
  if (*space2 == '\0' || !laghu_base_string_copy(response->reason, sizeof(response->reason), space2) ||
      !proxy_content_length(response->headers, response->header_count, &response->content_length, &response->has_content_length))
    return false;
  transfer = proxy_find(response->headers, response->header_count, "Transfer-Encoding");
  response->chunked = transfer != NULL && proxy_name_equal(transfer->value, "chunked");
  return (transfer == NULL || response->chunked) && !(response->chunked && response->has_content_length) && response->status >= 200U;
}

bool proxy_hop(const char *name) {
  return proxy_name_equal(name, "Connection") || proxy_name_equal(name, "Keep-Alive") || proxy_name_equal(name, "Proxy-Authenticate") ||
         proxy_name_equal(name, "Proxy-Authorization") || proxy_name_equal(name, "TE") || proxy_name_equal(name, "Trailer") ||
         proxy_name_equal(name, "Transfer-Encoding") || proxy_name_equal(name, "Upgrade");
}

bool proxy_connection_nominates(const proxy_header *headers, size_t count, const char *name) {
  size_t index;
  for (index = 0U; index < count; ++index) {
    const char *cursor;
    if (!proxy_name_equal(headers[index].name, "Connection")) continue;
    cursor = headers[index].value;
    while (*cursor != '\0') {
      const char *comma = strchr(cursor, ',');
      const char *end = comma;
      const char *first = cursor;
      size_t length;
      while (*first == ' ' || *first == '\t') ++first;
      if (end == NULL) end = cursor + strlen(cursor);
      while (end > first && (end[-1] == ' ' || end[-1] == '\t')) --end;
      length = (size_t)(end - first);
      if (strlen(name) == length) {
        size_t offset;
        bool equal = true;
        for (offset = 0U; offset < length; ++offset)
          if (tolower((unsigned char)first[offset]) != tolower((unsigned char)name[offset])) {
            equal = false;
            break;
          }
        if (equal) return true;
      }
      cursor = comma != NULL ? comma + 1 : end;
    }
  }
  return false;
}

bool proxy_forwarding_name(const char *name) {
  return proxy_name_equal(name, "Forwarded") || proxy_name_equal(name, "X-Forwarded-For") || proxy_name_equal(name, "X-Forwarded-Proto") ||
         proxy_name_equal(name, "X-Forwarded-Host");
}
