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

static void proxy_beacon_fail(laghu_socket client, proxy_access_log *access, unsigned int status, const char *reason, const char *failure) {
  proxy_error_response(client, status, reason);
  access->status = status;
  access->failure = failure;
}

static void proxy_beacon_send_script(laghu_socket client, const char *script, size_t length, proxy_access_log *access) {
  char header[256];
  int written = snprintf(header, sizeof(header),
                         "HTTP/1.1 200 OK\r\nContent-Type: "
                         "application/javascript\r\nContent-Length: "
                         "%zu\r\nConnection: close\r\n\r\n",
                         length);
  if (written > 0 && (size_t)written < sizeof(header)) {
    (void)proxy_send_all(client, header, (size_t)written);
    (void)proxy_send_all(client, script, length);
  }
  access->status = 200U;
  access->output_bytes = length;
}

static void proxy_beacon_send_no_content(laghu_socket client, proxy_access_log *access) {
  static const char response[] =
      "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n"
      "Connection: close\r\n\r\n";
  (void)proxy_send_all(client, response, sizeof(response) - 1U);
  access->status = 204U;
}

static bool proxy_beacon_request_valid(proxy_request *request, size_t request_body_length) {
  proxy_header *type = proxy_find(request->headers, request->header_count, "Content-Type");
  proxy_header *site = proxy_find(request->headers, request->header_count, "Sec-Fetch-Site");
  return type != NULL && strncmp(type->value, "application/json", 16U) == 0 && site != NULL && proxy_name_equal(site->value, "same-origin") &&
         request_body_length != 0U && request_body_length <= LAGHU_PROXY_BEACON_BODY;
}

bool proxy_handle_beacon_routes(const proxy_connection *connection, proxy_worker *worker, proxy_request *request_value,
                                const unsigned char *request_body, size_t request_body_length, proxy_access_log *access_value) {
  const laghu_proxy_options *options = worker->queue->options;
  laghu_socket client = connection->socket;
  if (!strcmp(request_value->target, "/.laghu/health")) {
    bool head = !strcmp(request_value->method, "HEAD");
    if (strcmp(request_value->method, "GET") != 0 && !head) {
      proxy_beacon_fail(client, access_value, 405U, "Method Not Allowed", "request_limit");
    } else {
      char json[128];
      const char *state = proxy_state_name(proxy_state(worker->queue));
      (void)snprintf(json, sizeof(json), "{\"status\":\"ok\",\"state\":\"%s\"}", state);
      proxy_send_json(client, 200U, "OK", json, head);
      access_value->status = 200U;
      access_value->output_bytes = head ? 0U : strlen(json);
    }
    return true;
  }
  {
    laghu_http_beacon_options beacon_options = {options->config.image_beacon == LAGHU_MODE_ON, options->config.critical_css_beacon == LAGHU_MODE_ON,
                                                options->config.instrumentation_beacon == LAGHU_MODE_ON};
    laghu_http_beacon_plan plan;
    laghu_buffer method = {(const unsigned char *)request_value->method, strlen(request_value->method)};
    laghu_buffer target = {(const unsigned char *)request_value->target, strlen(request_value->target)};
    if (!laghu_http_beacon_plan_build(&plan, method, target, &beacon_options) || !plan.recognized) {
      return false;
    }
    if (plan.status != 0U) {
      proxy_beacon_fail(client, access_value, plan.status, plan.status == 404U ? "Not Found" : "Method Not Allowed", "request_limit");
      return true;
    }
    if (plan.action == LAGHU_HTTP_BEACON_ACTION_SERVE_SCRIPT) {
      const char *script;
      size_t script_length;
      if (plan.route == LAGHU_HTTP_BEACON_ROUTE_IMAGE_SCRIPT) {
        script = laghu_runtime_image_beacon_script();
        script_length = strlen(script);
      } else if (plan.route == LAGHU_HTTP_BEACON_ROUTE_CRITICAL_CSS_SCRIPT) {
        script = laghu_runtime_critical_css_beacon_script();
        script_length = strlen(script);
      } else {
        script = laghu_runtime_instrumentation_script();
        script_length = strlen(script);
      }
      proxy_beacon_send_script(client, script, script_length, access_value);
      return true;
    }
    {
      bool applied = false;
      uint64_t now = (uint64_t)time(NULL);
      if (!proxy_beacon_request_valid(request_value, request_body_length)) {
        proxy_beacon_fail(client, access_value, 400U, "Bad Request", "client_parse");
        return true;
      } else if (!proxy_beacon_allowed(worker->queue, now)) {
        proxy_beacon_fail(client, access_value, 429U, "Too Many Requests", "request_limit");
        return true;
      }
      if (plan.route == LAGHU_HTTP_BEACON_ROUTE_IMAGE_REPORT) {
        laghu_image_beacon_record beacon;
        laghu_runtime_queue_snapshot queue_snapshot;
        laghu_runtime_queue *runtime_queue = proxy_runtime_queue(worker);
        laghu_policy policy;
        char policy_key[LAGHU_RUNTIME_KEY_SIZE];
        applied = laghu_runtime_parse_image_beacon((laghu_buffer){request_body, request_body_length}, &beacon) &&
                  laghu_resolve_config_policy(&options->config, &policy) && laghu_variant_key((laghu_buffer){NULL, 0U}, &policy, policy_key) &&
                  runtime_queue != NULL && laghu_runtime_queue_snapshot_get(runtime_queue, &queue_snapshot) &&
                  laghu_catalog_apply_beacon(worker->queue->rum, options->service.image_cache, policy_key, queue_snapshot.capabilities, now,
                                             options->config.image_metadata_ttl, &beacon);
      } else if (plan.route == LAGHU_HTTP_BEACON_ROUTE_CRITICAL_CSS_REPORT) {
        laghu_critical_css_beacon beacon;
        laghu_policy policy;
        char policy_key[LAGHU_RUNTIME_KEY_SIZE];
        applied = laghu_runtime_parse_critical_css_beacon((laghu_buffer){request_body, request_body_length}, &beacon) &&
                  laghu_resolve_config_policy(&options->config, &policy) && laghu_variant_key((laghu_buffer){NULL, 0U}, &policy, policy_key) &&
                  laghu_critical_css_apply_beacon(worker->queue->rum, options->service.image_cache, policy_key, now,
                                                  options->config.image_metadata_ttl, &beacon);
      } else if (plan.route == LAGHU_HTTP_BEACON_ROUTE_INSTRUMENTATION_REPORT) {
        laghu_instrumentation_beacon beacon;
        applied =
            laghu_runtime_parse_instrumentation_beacon((laghu_buffer){request_body, request_body_length}, &beacon) &&
            laghu_instrumentation_apply_beacon(worker->queue->rum, options->service.image_cache, now, options->config.image_metadata_ttl, &beacon);
      }
      if (!applied) {
        proxy_beacon_fail(client, access_value, 400U, "Bad Request", "worker");
      } else {
        proxy_beacon_send_no_content(client, access_value);
      }
      return true;
    }
  }
}
