// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "server_internal.h"

#define PROXY_RESPONSE_HEADER_BYTES 16384U

void proxy_error_response(laghu_socket client, SSL *tls, unsigned int status, const char *reason) {
  char response[512];
  int length = snprintf(response, sizeof(response),
                        "HTTP/1.1 %u %s\r\nContent-Length: 0\r\nConnection: "
                        "close\r\nX-Laghu: bypass-error\r\n\r\n",
                        status, reason);
  if (length > 0) (void)proxy_client_send_all(client, tls, response, (size_t)length);
}

void proxy_reject_connection(proxy_queue *queue, laghu_socket client, const char *failure) {
  unsigned char discarded[4096U];
  proxy_access_log access;
  const laghu_proxy_options *options = proxy_options_acquire(queue);
  proxy_access_init(&access, queue);
  access.status = 503U;
  access.failure = failure;
  if (!options->downstream_tls) proxy_error_response(client, NULL, 503U, "Service Unavailable");
  (void)shutdown(client, LAGHU_SHUT_WRITE);
  while (recv(client, discarded, sizeof(discarded), MSG_DONTWAIT) > 0) {
  }
  laghu_close(client);
  proxy_access_write(queue, options, &access);
  proxy_options_release(queue);
}

bool proxy_read_body(laghu_socket socket, SSL *tls, const unsigned char *initial, size_t initial_length, size_t expected, bool to_close,
                     unsigned char **body, size_t *length) {
  size_t capacity = expected != 0U ? expected : 65536U, used = 0U;
  unsigned char *data;
  if (capacity > LAGHU_PROXY_MAX_BODY) return false;
  data = malloc(capacity == 0U ? 1U : capacity);
  if (data == NULL) return false;
  if (initial_length > capacity) {
    free(data);
    return false;
  }
  memcpy(data, initial, initial_length);
  used = initial_length;
  while ((expected != 0U && used < expected) || (expected == 0U && to_close)) {
    int got;
    if (used == capacity) {
      size_t grown = capacity * 2U;
      unsigned char *replacement;
      if (grown > LAGHU_PROXY_MAX_BODY) grown = LAGHU_PROXY_MAX_BODY;
      if (grown == capacity) {
        free(data);
        return false;
      }
      replacement = realloc(data, grown);
      if (replacement == NULL) {
        free(data);
        return false;
      }
      data = replacement;
      capacity = grown;
    }
    got = proxy_origin_recv(socket, tls, data + used, capacity - used);
    if (got == 0 && to_close) break;
    if (got <= 0) {
      free(data);
      return false;
    }
    used += (size_t)got;
  }
  if (expected != 0U && used != expected) {
    free(data);
    return false;
  }
  *body = data;
  *length = used;
  return true;
}

bool proxy_read_client_body(laghu_socket socket, SSL *tls, const unsigned char *initial, size_t initial_length, size_t expected,
                            unsigned int idle_timeout, unsigned int total_timeout, unsigned char **body, size_t *length) {
  uint64_t deadline = proxy_monotonic_ms() + (uint64_t)total_timeout * 1000U;
  size_t capacity = expected;
  size_t used = 0U;
  unsigned char *data;
  bool complete = false;
  if (capacity == 0U || capacity > LAGHU_PROXY_MAX_BODY || initial_length > capacity) return false;
  data = malloc(capacity);
  if (data == NULL) return false;
  memcpy(data, initial, initial_length);
  used = initial_length;
  while (used < expected) {
    int got = proxy_client_recv_until(socket, tls, data + used, expected - used, idle_timeout, deadline);
    if (got <= 0) goto done;
    used += (size_t)got;
  }
  *body = data;
  *length = used;
  data = NULL;
  complete = true;
done:
  proxy_timeout(socket, idle_timeout);
  free(data);
  return complete;
}

typedef enum {
  PROXY_CHUNK_SIZE = 0,
  PROXY_CHUNK_SIZE_LF,
  PROXY_CHUNK_DATA,
  PROXY_CHUNK_DATA_CR,
  PROXY_CHUNK_DATA_LF,
  PROXY_CHUNK_TRAILER,
  PROXY_CHUNK_TRAILER_LF,
  PROXY_CHUNK_COMPLETE
} proxy_chunked_state;

typedef struct {
  proxy_chunked_state state;
  unsigned char *decoded;
  size_t decoded_length;
  size_t decoded_capacity;
  size_t encoded_length;
  size_t chunk_size;
  size_t chunk_remaining;
  size_t line_length;
  bool size_digit;
  bool extension;
  bool trailer_colon;
} proxy_chunked_reader;

static int proxy_chunked_hex(unsigned char byte) {
  if (byte >= '0' && byte <= '9') return byte - '0';
  if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
  if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
  return -1;
}

static bool proxy_chunked_header_name(unsigned char byte) {
  return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || strchr("!#$%&'*+-.^_`|~", byte) != NULL;
}

static bool proxy_chunked_reserve(proxy_chunked_reader *reader, size_t needed) {
  size_t grown = reader->decoded_capacity == 0U ? 4096U : reader->decoded_capacity;
  unsigned char *replacement;
  if (needed > LAGHU_PROXY_MAX_BODY) return false;
  while (grown < needed) {
    if (grown > LAGHU_PROXY_MAX_BODY / 2U) {
      grown = LAGHU_PROXY_MAX_BODY;
      break;
    }
    grown *= 2U;
  }
  if (grown < needed) return false;
  replacement = realloc(reader->decoded, grown);
  if (replacement == NULL) return false;
  reader->decoded = replacement;
  reader->decoded_capacity = grown;
  return true;
}

static bool proxy_chunked_feed(proxy_chunked_reader *reader, const unsigned char *input, size_t length) {
  size_t index = 0U;
  if (input == NULL && length != 0U) return false;
  if (length > LAGHU_PROXY_MAX_BODY - reader->encoded_length) return false;
  reader->encoded_length += length;
  while (index < length) {
    unsigned char byte = input[index];
    switch (reader->state) {
      case PROXY_CHUNK_SIZE:
        if (++reader->line_length > LAGHU_PROXY_LINE_BYTES || byte == '\n') return false;
        if (byte == '\r') {
          if (!reader->size_digit) return false;
          reader->state = PROXY_CHUNK_SIZE_LF;
          ++index;
        } else if (byte == ';') {
          reader->extension = true;
          ++index;
        } else if (reader->extension) {
          if (byte < 32U || byte == 127U) return false;
          ++index;
        } else {
          int value = proxy_chunked_hex(byte);
          if (value < 0 || reader->chunk_size > (SIZE_MAX - (size_t)value) / 16U) return false;
          reader->size_digit = true;
          reader->chunk_size = reader->chunk_size * 16U + (size_t)value;
          if (reader->chunk_size > LAGHU_PROXY_MAX_BODY - reader->decoded_length) return false;
          ++index;
        }
        break;
      case PROXY_CHUNK_SIZE_LF:
        if (byte != '\n') return false;
        ++index;
        reader->line_length = 0U;
        reader->size_digit = false;
        reader->extension = false;
        if (reader->chunk_size == 0U) {
          reader->state = PROXY_CHUNK_TRAILER;
        } else {
          reader->chunk_remaining = reader->chunk_size;
          if (!proxy_chunked_reserve(reader, reader->decoded_length + reader->chunk_remaining)) return false;
          reader->state = PROXY_CHUNK_DATA;
        }
        reader->chunk_size = 0U;
        break;
      case PROXY_CHUNK_DATA: {
        size_t available = length - index;
        size_t take = reader->chunk_remaining < available ? reader->chunk_remaining : available;
        memcpy(reader->decoded + reader->decoded_length, input + index, take);
        reader->decoded_length += take;
        reader->chunk_remaining -= take;
        index += take;
        if (reader->chunk_remaining == 0U) reader->state = PROXY_CHUNK_DATA_CR;
        break;
      }
      case PROXY_CHUNK_DATA_CR:
        if (byte != '\r') return false;
        reader->state = PROXY_CHUNK_DATA_LF;
        ++index;
        break;
      case PROXY_CHUNK_DATA_LF:
        if (byte != '\n') return false;
        reader->state = PROXY_CHUNK_SIZE;
        ++index;
        break;
      case PROXY_CHUNK_TRAILER:
        if (byte == '\r') {
          if (reader->line_length != 0U && !reader->trailer_colon) return false;
          reader->state = PROXY_CHUNK_TRAILER_LF;
          ++index;
        } else {
          if (byte == '\n' || byte < 32U || byte == 127U || ++reader->line_length > LAGHU_PROXY_LINE_BYTES) return false;
          if (!reader->trailer_colon) {
            if (byte == ':') {
              if (reader->line_length == 1U) return false;
              reader->trailer_colon = true;
            } else if (!proxy_chunked_header_name(byte)) {
              return false;
            }
          }
          ++index;
        }
        break;
      case PROXY_CHUNK_TRAILER_LF:
        if (byte != '\n') return false;
        ++index;
        if (reader->line_length == 0U) {
          reader->state = PROXY_CHUNK_COMPLETE;
          if (index != length) return false;
        } else {
          reader->line_length = 0U;
          reader->trailer_colon = false;
          reader->state = PROXY_CHUNK_TRAILER;
        }
        break;
      case PROXY_CHUNK_COMPLETE:
        return false;
    }
  }
  return true;
}

bool proxy_read_chunked_body(laghu_socket socket, SSL *tls, const unsigned char *initial, size_t initial_length, unsigned char **body,
                             size_t *length) {
  unsigned char buffer[65536U];
  proxy_chunked_reader reader = {.state = PROXY_CHUNK_SIZE};
  if (body == NULL || length == NULL || !proxy_chunked_feed(&reader, initial, initial_length)) goto fail;
  while (reader.state != PROXY_CHUNK_COMPLETE) {
    int got = proxy_origin_recv(socket, tls, buffer, sizeof(buffer));
    if (got <= 0 || !proxy_chunked_feed(&reader, buffer, (size_t)got)) goto fail;
  }
  if (reader.decoded == NULL) {
    reader.decoded = malloc(1U);
    if (reader.decoded == NULL) goto fail;
  }
  *body = reader.decoded;
  *length = reader.decoded_length;
  return true;
fail:
  free(reader.decoded);
  return false;
}

static bool proxy_operation_removes(const laghu_http_transaction_result *result, const char *name) {
  size_t index;
  for (index = 0U; index < result->header_operation_count; ++index)
    if (proxy_name_equal(result->header_operations[index].name, name) &&
        (result->header_operations[index].kind == LAGHU_HTTP_HEADER_REMOVE || result->header_operations[index].kind == LAGHU_HTTP_HEADER_SET))
      return true;
  return false;
}

bool proxy_beacon_allowed(proxy_queue *queue, uint64_t now) {
  bool allowed;
  pthread_mutex_lock(&queue->lock);
  if (queue->beacon_second != now) {
    queue->beacon_second = now;
    queue->beacon_count = 0U;
  }
  allowed = ++queue->beacon_count <= 32U;
  pthread_mutex_unlock(&queue->lock);
  return allowed;
}

static bool proxy_response_headers_flush(laghu_socket client, SSL *client_tls, char headers[PROXY_RESPONSE_HEADER_BYTES], size_t *used) {
  if (*used == 0U) return true;
  if (!proxy_client_send_all(client, client_tls, headers, *used)) return false;
  *used = 0U;
  return true;
}

static bool proxy_response_headers_append(laghu_socket client, SSL *client_tls, char headers[PROXY_RESPONSE_HEADER_BYTES], size_t *used,
                                          const char *format, ...) {
  va_list arguments;
  int count;
  va_start(arguments, format);
  count = vsnprintf(headers + *used, PROXY_RESPONSE_HEADER_BYTES - *used, format, arguments);
  va_end(arguments);
  if (count < 0) return false;
  if ((size_t)count >= PROXY_RESPONSE_HEADER_BYTES - *used) {
    if (!proxy_response_headers_flush(client, client_tls, headers, used)) return false;
    va_start(arguments, format);
    count = vsnprintf(headers, PROXY_RESPONSE_HEADER_BYTES, format, arguments);
    va_end(arguments);
    if (count < 0 || (size_t)count >= PROXY_RESPONSE_HEADER_BYTES) return false;
  }
  *used += (size_t)count;
  return true;
}

bool proxy_send_headers(laghu_socket client, SSL *client_tls, const proxy_response *origin, const laghu_http_transaction_result *result,
                        size_t content_length, bool has_content_length) {
  char headers[PROXY_RESPONSE_HEADER_BYTES];
  size_t used = 0U;
  size_t index;
  unsigned int status = result != NULL && result->not_modified ? 304U : origin->status;
  const char *reason = result != NULL && result->not_modified ? "Not Modified" : (origin->reason[0] ? origin->reason : "OK");
  if (result != NULL && result->not_modified) has_content_length = false;
  if (!proxy_response_headers_append(client, client_tls, headers, &used, "HTTP/1.1 %u %s\r\n", status, reason)) return false;
  for (index = 0U; index < origin->header_count; ++index) {
    if (proxy_hop(origin->headers[index].name) || proxy_connection_nominates(origin->headers, origin->header_count, origin->headers[index].name) ||
        proxy_name_equal(origin->headers[index].name, "Content-Length") ||
        (result != NULL && proxy_operation_removes(result, origin->headers[index].name)))
      continue;
    if (!proxy_response_headers_append(client, client_tls, headers, &used, "%s: %s\r\n", origin->headers[index].name, origin->headers[index].value))
      return false;
  }
  if (result != NULL)
    for (index = 0U; index < result->header_operation_count; ++index) {
      const laghu_http_header_operation *operation = &result->header_operations[index];
      if (operation->kind == LAGHU_HTTP_HEADER_REMOVE || proxy_name_equal(operation->name, "Content-Length")) continue;
      if (!proxy_response_headers_append(client, client_tls, headers, &used, "%s: %s\r\n", operation->name,
                                         operation->value == NULL ? "" : operation->value))
        return false;
    }
  if (has_content_length) {
    if (!proxy_response_headers_append(client, client_tls, headers, &used, "Content-Length: %zu\r\nConnection: close\r\n\r\n", content_length))
      return false;
  } else if (!proxy_response_headers_append(client, client_tls, headers, &used, "Connection: close\r\n\r\n")) {
    return false;
  }
  return proxy_response_headers_flush(client, client_tls, headers, &used);
}

bool proxy_send_early_hints(laghu_socket client, SSL *client_tls, const char *request_version, const laghu_http_transaction_result *result) {
  char line[16384];
  size_t index;
  bool emitted = false;
  if (result == NULL) return false;
  if (request_version == NULL || strcmp(request_version, "HTTP/1.1") != 0) return true;
  for (index = 0U; index < result->header_operation_count; ++index) {
    const laghu_http_header_operation *operation = &result->header_operations[index];
    int count;
    if (!operation->early_hint) continue;
    if (!emitted) {
      if (!proxy_client_send_all(client, client_tls, "HTTP/1.1 103 Early Hints\r\n", sizeof("HTTP/1.1 103 Early Hints\r\n") - 1U)) return false;
      emitted = true;
    }
    count = snprintf(line, sizeof(line), "Link: %s\r\n", operation->value);
    if (count <= 0 || (size_t)count >= sizeof(line) || !proxy_client_send_all(client, client_tls, line, (size_t)count)) return false;
  }
  return !emitted || proxy_client_send_all(client, client_tls, "\r\n", 2U);
}

bool proxy_send_result(laghu_socket client, SSL *client_tls, const proxy_response *origin, const laghu_http_transaction_result *result,
                       laghu_buffer body) {
  if (result != NULL && result->not_modified) body.length = 0U;
  return proxy_send_headers(client, client_tls, origin, result, body.length, true) &&
         (body.length == 0U || proxy_client_send_all(client, client_tls, body.data, body.length));
}

bool proxy_stream_body(laghu_socket origin, SSL *origin_tls, laghu_socket client, SSL *client_tls, const unsigned char *initial,
                       size_t initial_length, size_t expected, bool until_close) {
  unsigned char buffer[65536U];
  size_t sent = 0U;
  if (expected != 0U && initial_length > expected) return false;
  if (initial_length != 0U && !proxy_client_send_all(client, client_tls, initial, initial_length)) return false;
  sent = initial_length;
  while ((expected != 0U && sent < expected) || until_close) {
    size_t wanted = sizeof(buffer);
    int got;
    if (expected != 0U && wanted > expected - sent) wanted = expected - sent;
    got = proxy_origin_recv(origin, origin_tls, buffer, wanted);
    if (got == 0 && until_close) return true;
    if (got <= 0) return false;
    if (!proxy_client_send_all(client, client_tls, buffer, (size_t)got)) return false;
    sent += (size_t)got;
  }
  return expected == 0U || sent == expected;
}
