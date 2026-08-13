// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <string.h>

#include "server_internal.h"

proxy_lifecycle_state proxy_state(proxy_queue *queue) {
  proxy_lifecycle_state state;
  proxy_queue_lock(queue);
  state = queue->state;
  proxy_queue_unlock(queue);
  return state;
}

const char *proxy_state_name(proxy_lifecycle_state state) {
  switch (state) {
    case PROXY_STARTING:
      return "starting";
    case PROXY_RUNNING:
      return "running";
    case PROXY_DRAINING:
      return "draining";
    case PROXY_FORCING:
      return "forcing";
    case PROXY_STOPPED:
      return "stopped";
  }
  return "unknown";
}

void proxy_send_json(laghu_socket client, unsigned int status, const char *reason, const char *json, bool head) {
  char headers[512];
  size_t length = strlen(json);
  int count = snprintf(headers, sizeof(headers),
                       "HTTP/1.1 %u %s\r\nContent-Type: application/json\r\n"
                       "Content-Length: %zu\r\nCache-Control: no-store\r\n"
                       "Connection: close\r\n\r\n",
                       status, reason, length);
  if (count > 0 && (size_t)count < sizeof(headers) && proxy_send_all(client, headers, (size_t)count) && !head)
    (void)proxy_send_all(client, json, length);
}

void proxy_send_admin_json(laghu_socket client, unsigned int status, const char *reason, const char *json, bool head) {
  char headers[512];
  size_t length = strlen(json);
  int count = snprintf(headers, sizeof(headers),
                       "HTTP/1.1 %u %s\r\nContent-Type: application/json\r\n"
                       "Cache-Control: no-store\r\nContent-Length: %zu\r\n"
                       "Connection: close\r\n\r\n",
                       status, reason, length);
  if (count > 0 && (size_t)count < sizeof(headers)) {
    (void)proxy_send_all(client, headers, (size_t)count);
    if (!head) (void)proxy_send_all(client, json, length);
  }
}

void proxy_send_admin_html(laghu_socket client, unsigned int status, const char *reason, const char *html, bool head) {
  char headers[512];
  size_t length = strlen(html);
  int count = snprintf(headers, sizeof(headers),
                       "HTTP/1.1 %u %s\r\nContent-Type: text/html; "
                       "charset=utf-8\r\nCache-Control: no-store\r\n"
                       "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                       status, reason, length);
  if (count > 0 && (size_t)count < sizeof(headers)) {
    (void)proxy_send_all(client, headers, (size_t)count);
    if (!head) (void)proxy_send_all(client, html, length);
  }
}

void proxy_send_metrics(laghu_socket client, const char *body, size_t length, bool head) {
  char headers[512];
  int count = snprintf(headers, sizeof(headers),
                       "HTTP/1.1 200 OK\r\nContent-Type: text/plain; "
                       "version=0.0.4; charset=utf-8\r\n"
                       "Cache-Control: no-store\r\nContent-Length: %zu\r\n"
                       "Connection: close\r\n\r\n",
                       length);
  if (count > 0 && (size_t)count < sizeof(headers)) {
    (void)proxy_send_all(client, headers, (size_t)count);
    if (!head) (void)proxy_send_all(client, body, length);
  }
}
