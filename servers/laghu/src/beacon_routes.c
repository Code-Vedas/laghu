// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "laghu/catalog.h"
#include "laghu/config.h"
#include "laghu/instrumentation.h"
#include "server_internal.h"

static const char laghu_beacon_script[] =
    "addEventListener('load',()=>{document.querySelectorAll('img[src]').forEach"
    "(i=>{const r=i.getBoundingClientRect();if(r.width<1||r.height<1)return;"
    "fetch('/.laghu/beacon/images',{method:'POST',headers:{'Content-Type':"
    "'application/json'},body:JSON.stringify({url:new URL(i.currentSrc||i.src,"
    "location.href).pathname,width:Math.round(r.width),height:Math.round(r."
    "height),viewport_width:innerWidth,dpr_hundredths:Math.min(400,Math.max("
    "100,Math.round(devicePixelRatio*100))),above_fold:r.top<innerHeight,"
    "mobile:innerWidth<768}),keepalive:true})})});";

#define PROXY_FAIL(status_value, reason_value, failure_value)     \
  do {                                                            \
    proxy_error_response(client, (status_value), (reason_value)); \
    access.status = (status_value);                               \
    access.failure = (failure_value);                             \
  } while (0)

bool proxy_handle_beacon_routes(const proxy_connection *connection,
                                proxy_worker *worker,
                                proxy_request *request_value,
                                const unsigned char *request_body,
                                size_t request_body_length,
                                proxy_access_log *access_value) {
  const laghu_proxy_options *options = worker->queue->options;
  laghu_socket client = connection->socket;
#define request (*request_value)
#define access (*access_value)
  if (!strcmp(request.target, "/.laghu/health")) {
    bool head = !strcmp(request.method, "HEAD");
    if (strcmp(request.method, "GET") != 0 && !head) {
      PROXY_FAIL(405U, "Method Not Allowed", "request_limit");
    } else {
      char json[128];
      const char *state = proxy_state_name(proxy_state(worker->queue));
      (void)snprintf(json, sizeof(json), "{\"status\":\"ok\",\"state\":\"%s\"}",
                     state);
      proxy_send_json(client, 200U, "OK", json, head);
      access.status = 200U;
      access.output_bytes = head ? 0U : strlen(json);
    }
    return true;
  }
  if (!strcmp(request.target, "/.laghu/beacon/images.js") &&
      options->config.image_beacon == LAGHU_MODE_ON &&
      !strcmp(request.method, "GET")) {
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\nContent-Type: "
                     "application/javascript\r\nContent-Length: "
                     "%zu\r\nConnection: close\r\n\r\n",
                     sizeof(laghu_beacon_script) - 1U);
    if (n > 0) {
      (void)proxy_send_all(client, head, (size_t)n);
      (void)proxy_send_all(client, laghu_beacon_script,
                           sizeof(laghu_beacon_script) - 1U);
    }
    access.status = 200U;
    access.output_bytes = sizeof(laghu_beacon_script) - 1U;
    return true;
  }
  if (!strcmp(request.target, "/.laghu/beacon/images") &&
      options->config.image_beacon == LAGHU_MODE_ON &&
      !strcmp(request.method, "POST")) {
    proxy_header *type =
        proxy_find(request.headers, request.header_count, "Content-Type");
    proxy_header *site =
        proxy_find(request.headers, request.header_count, "Sec-Fetch-Site");
    laghu_image_beacon_record beacon;
    laghu_policy policy;
    char policy_key[LAGHU_RUNTIME_KEY_SIZE];
    uint64_t now = (uint64_t)time(NULL);
    if (type == NULL || strncmp(type->value, "application/json", 16U) != 0 ||
        site == NULL || !proxy_name_equal(site->value, "same-origin") ||
        request_body_length == 0U ||
        request_body_length > LAGHU_PROXY_BEACON_BODY) {
      PROXY_FAIL(400U, "Bad Request", "client_parse");
    } else if (!proxy_beacon_allowed(worker->queue, now)) {
      PROXY_FAIL(429U, "Too Many Requests", "request_limit");
    } else if (!laghu_runtime_parse_image_beacon(
                   (laghu_buffer){request_body, request_body_length},
                   &beacon) ||
               !laghu_resolve_config_policy(&options->config, &policy) ||
               !laghu_variant_key((laghu_buffer){NULL, 0U}, &policy,
                                  policy_key) ||
               (worker->runtime_queue.mapping == NULL &&
                !laghu_runtime_queue_open(&worker->runtime_queue,
                                          options->worker_queue_path)) ||
               !laghu_runtime_queue_refresh(&worker->runtime_queue) ||
               !laghu_catalog_apply_beacon(
                   worker->queue->rum, options->cache_path, policy_key,
                   worker->runtime_queue.capabilities, now,
                   options->config.image_metadata_ttl, &beacon)) {
      PROXY_FAIL(400U, "Bad Request", "worker");
    } else {
      static const char response_204[] =
          "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n"
          "Connection: close\r\n\r\n";
      (void)proxy_send_all(client, response_204, sizeof(response_204) - 1U);
      access.status = 204U;
    }
    return true;
  }
  if (!strcmp(request.target, "/.laghu/beacon/critical-css.js") &&
      options->config.critical_css_beacon == LAGHU_MODE_ON &&
      !strcmp(request.method, "GET")) {
    const char *script = laghu_runtime_critical_css_beacon_script();
    char head[256];
    size_t script_length = strlen(script);
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\nContent-Type: "
                     "application/javascript\r\nContent-Length: "
                     "%zu\r\nConnection: close\r\n\r\n",
                     script_length);
    if (n > 0) {
      (void)proxy_send_all(client, head, (size_t)n);
      (void)proxy_send_all(client, script, script_length);
    }
    access.status = 200U;
    access.output_bytes = script_length;
    return true;
  }
  if (!strcmp(request.target, "/.laghu/beacon/critical-css") &&
      options->config.critical_css_beacon == LAGHU_MODE_ON &&
      !strcmp(request.method, "POST")) {
    proxy_header *type =
        proxy_find(request.headers, request.header_count, "Content-Type");
    proxy_header *site =
        proxy_find(request.headers, request.header_count, "Sec-Fetch-Site");
    laghu_critical_css_beacon beacon;
    laghu_policy policy;
    char policy_key[LAGHU_RUNTIME_KEY_SIZE];
    uint64_t now = (uint64_t)time(NULL);
    if (type == NULL || strncmp(type->value, "application/json", 16U) != 0 ||
        site == NULL || !proxy_name_equal(site->value, "same-origin") ||
        request_body_length == 0U ||
        request_body_length > LAGHU_PROXY_BEACON_BODY) {
      PROXY_FAIL(400U, "Bad Request", "client_parse");
    } else if (!proxy_beacon_allowed(worker->queue, now)) {
      PROXY_FAIL(429U, "Too Many Requests", "request_limit");
    } else if (!laghu_runtime_parse_critical_css_beacon(
                   (laghu_buffer){request_body, request_body_length},
                   &beacon) ||
               !laghu_resolve_config_policy(&options->config, &policy) ||
               !laghu_variant_key((laghu_buffer){NULL, 0U}, &policy,
                                  policy_key) ||
               !laghu_critical_css_apply_beacon(
                   worker->queue->rum, options->cache_path, policy_key, now,
                   options->config.image_metadata_ttl, &beacon)) {
      PROXY_FAIL(400U, "Bad Request", "worker");
    } else {
      static const char response_204[] =
          "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n"
          "Connection: close\r\n\r\n";
      (void)proxy_send_all(client, response_204, sizeof(response_204) - 1U);
      access.status = 204U;
    }
    return true;
  }
  if (!strcmp(request.target, "/.laghu/beacon/instrumentation.js") &&
      options->config.instrumentation_beacon == LAGHU_MODE_ON &&
      !strcmp(request.method, "GET")) {
    const char *script = laghu_runtime_instrumentation_script();
    char head[256];
    size_t script_length = strlen(script);
    int n =
        snprintf(head, sizeof(head),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/javascript\r\n"
                 "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                 script_length);
    if (n > 0) {
      (void)proxy_send_all(client, head, (size_t)n);
      (void)proxy_send_all(client, script, script_length);
    }
    access.status = 200U;
    access.output_bytes = script_length;
    return true;
  }
  if (!strcmp(request.target, "/.laghu/beacon/instrumentation") &&
      options->config.instrumentation_beacon == LAGHU_MODE_ON &&
      !strcmp(request.method, "POST")) {
    proxy_header *type =
        proxy_find(request.headers, request.header_count, "Content-Type");
    proxy_header *site =
        proxy_find(request.headers, request.header_count, "Sec-Fetch-Site");
    laghu_instrumentation_beacon beacon;
    uint64_t now = (uint64_t)time(NULL);
    if (type == NULL || strncmp(type->value, "application/json", 16U) != 0 ||
        site == NULL || !proxy_name_equal(site->value, "same-origin") ||
        request_body_length == 0U ||
        request_body_length > LAGHU_PROXY_BEACON_BODY) {
      PROXY_FAIL(400U, "Bad Request", "client_parse");
    } else if (!proxy_beacon_allowed(worker->queue, now)) {
      PROXY_FAIL(429U, "Too Many Requests", "request_limit");
    } else if (!laghu_runtime_parse_instrumentation_beacon(
                   (laghu_buffer){request_body, request_body_length},
                   &beacon) ||
               !laghu_instrumentation_apply_beacon(
                   worker->queue->rum, options->cache_path, now,
                   options->config.image_metadata_ttl, &beacon)) {
      PROXY_FAIL(400U, "Bad Request", "worker");
    } else {
      static const char response_204[] =
          "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n"
          "Connection: close\r\n\r\n";
      (void)proxy_send_all(client, response_204, sizeof(response_204) - 1U);
      access.status = 204U;
    }
    return true;
  }
  return false;
#undef access
#undef request
}

#undef PROXY_FAIL
