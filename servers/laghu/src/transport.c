// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "server_internal.h"

void proxy_error_response(laghu_socket client, unsigned int status,
                          const char *reason) {
  char response[512];
  int length = snprintf(response, sizeof(response),
                        "HTTP/1.1 %u %s\r\nContent-Length: 0\r\nConnection: "
                        "close\r\nX-Laghu: bypass-error\r\n\r\n",
                        status, reason);
  if (length > 0) (void)proxy_send_all(client, response, (size_t)length);
}

void proxy_reject_connection(proxy_queue *queue, laghu_socket client,
                             const char *failure) {
  unsigned char discarded[4096U];
  proxy_access_log access;
  proxy_access_init(&access, queue);
  access.status = 503U;
  access.failure = failure;
  proxy_error_response(client, 503U, "Service Unavailable");
  (void)shutdown(client, LAGHU_SHUT_WRITE);
  while (recv(client, discarded, sizeof(discarded), MSG_DONTWAIT) > 0) {
  }
  laghu_close(client);
  proxy_access_write(queue, &access);
}

bool proxy_read_body(laghu_socket socket, SSL *tls,
                     const unsigned char *initial, size_t initial_length,
                     size_t expected, bool to_close, unsigned char **body,
                     size_t *length) {
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

static bool proxy_operation_removes(const laghu_http_transaction_result *result,
                                    const char *name) {
  size_t index;
  for (index = 0U; index < result->header_operation_count; ++index)
    if (proxy_name_equal(result->header_operations[index].name, name) &&
        (result->header_operations[index].kind == LAGHU_HTTP_HEADER_REMOVE ||
         result->header_operations[index].kind == LAGHU_HTTP_HEADER_SET))
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

bool proxy_send_headers(laghu_socket client, const proxy_response *origin,
                        const laghu_http_transaction_result *result,
                        size_t content_length, bool has_content_length) {
  char line[16384];
  size_t index;
  int count = snprintf(line, sizeof(line), "HTTP/1.1 %u %s\r\n", origin->status,
                       origin->reason[0] ? origin->reason : "OK");
  if (count <= 0 || !proxy_send_all(client, line, (size_t)count)) return false;
  for (index = 0U; index < origin->header_count; ++index) {
    if (proxy_hop(origin->headers[index].name) ||
        proxy_connection_nominates(origin->headers, origin->header_count,
                                   origin->headers[index].name) ||
        proxy_name_equal(origin->headers[index].name, "Content-Length") ||
        proxy_operation_removes(result, origin->headers[index].name))
      continue;
    count = snprintf(line, sizeof(line), "%s: %s\r\n",
                     origin->headers[index].name, origin->headers[index].value);
    if (count <= 0 || (size_t)count >= sizeof(line) ||
        !proxy_send_all(client, line, (size_t)count))
      return false;
  }
  for (index = 0U; index < result->header_operation_count; ++index) {
    const laghu_http_header_operation *operation =
        &result->header_operations[index];
    if (operation->kind == LAGHU_HTTP_HEADER_REMOVE ||
        proxy_name_equal(operation->name, "Content-Length"))
      continue;
    count = snprintf(line, sizeof(line), "%s: %s\r\n", operation->name,
                     operation->value == NULL ? "" : operation->value);
    if (count <= 0 || (size_t)count >= sizeof(line) ||
        !proxy_send_all(client, line, (size_t)count))
      return false;
  }
  count = has_content_length
              ? snprintf(line, sizeof(line),
                         "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                         content_length)
              : snprintf(line, sizeof(line), "Connection: close\r\n\r\n");
  return count > 0 && proxy_send_all(client, line, (size_t)count);
}

bool proxy_send_result(laghu_socket client, const proxy_response *origin,
                       const laghu_http_transaction_result *result,
                       laghu_buffer body) {
  return proxy_send_headers(client, origin, result, body.length, true) &&
         (body.length == 0U || proxy_send_all(client, body.data, body.length));
}

bool proxy_stream_body(laghu_socket origin, SSL *tls, laghu_socket client,
                       const unsigned char *initial, size_t initial_length,
                       size_t expected, bool until_close) {
  unsigned char buffer[65536U];
  size_t sent = 0U;
  if (expected != 0U && initial_length > expected) return false;
  if (initial_length != 0U && !proxy_send_all(client, initial, initial_length))
    return false;
  sent = initial_length;
  while ((expected != 0U && sent < expected) || until_close) {
    size_t wanted = sizeof(buffer);
    int got;
    if (expected != 0U && wanted > expected - sent) wanted = expected - sent;
    got = proxy_origin_recv(origin, tls, buffer, wanted);
    if (got == 0 && until_close) return true;
    if (got <= 0) return false;
    if (!proxy_send_all(client, buffer, (size_t)got)) return false;
    sent += (size_t)got;
  }
  return expected == 0U || sent == expected;
}
