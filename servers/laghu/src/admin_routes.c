// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "laghu/cache.h"
#include "server_internal.h"

static void proxy_admin_fail(laghu_socket client, proxy_access_log *access,
                             unsigned int status, const char *reason,
                             const char *failure) {
  proxy_error_response(client, status, reason);
  access->status = status;
  access->failure = failure;
}

bool proxy_handle_administrative_routes(const proxy_connection *connection,
                                        proxy_worker *worker,
                                        proxy_request *request_value,
                                        proxy_access_log *access_value) {
  const laghu_proxy_options *options = worker->queue->options;
  laghu_socket client = connection->socket;
  laghu_http_administrative_options admin_options = {
      options->service.metrics,     options->service.readiness,
      options->service.statistics,  options->service.purge_method,
      options->service.purge_query, true};
  laghu_http_administrative_plan plan;
  laghu_buffer method = {(const unsigned char *)request_value->method,
                         strlen(request_value->method)};
  laghu_buffer target = {(const unsigned char *)request_value->target,
                         strlen(request_value->target)};
  if (!laghu_http_administrative_plan_build(&plan, method, target,
                                            &admin_options)) {
    return false;
  }
  if (!plan.recognized) return false;
  if (plan.status == 404U) {
    proxy_admin_fail(client, access_value, 404U, "Not Found", "request_limit");
    return true;
  }
  bool head = strcmp(request_value->method, "HEAD") == 0;
  bool authorized =
      proxy_peer_in_cidrs(connection, options->service.purge_allow,
                          options->service.purge_allow_count) &&
      proxy_admin_token(options, request_value);
  if (!authorized) {
    proxy_send_admin_json(client, 403U, "Forbidden",
                          "{\"status\":\"forbidden\"}", head);
    access_value->status = 403U;
    access_value->failure = "admin_auth";
  } else if (plan.status == 400U) {
    proxy_send_admin_json(client, 400U, "Bad Request",
                          "{\"status\":\"malformed\"}", false);
    access_value->status = 400U;
    access_value->failure = "admin_malformed";
  } else if (plan.status == 405U) {
    proxy_send_admin_json(client, 405U, "Method Not Allowed",
                          "{\"status\":\"method_not_allowed\"}", head);
    access_value->status = 405U;
    access_value->failure = "admin_method";
  } else if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_METRICS ||
             plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_READINESS) {
    (void)laghu_operational_registry_heartbeat(
        &worker->queue->operational, (uint64_t)time(NULL), true, 0U, 0U);
    proxy_operational_response *operational = calloc(1U, sizeof(*operational));
    laghu_cache_stats stats = {0};
    laghu_http_administrative_response response;
    if (operational == NULL) {
      proxy_send_admin_json(client, 503U, "Service Unavailable",
                            "{\"status\":\"unavailable\"}", head);
      access_value->status = 503U;
      access_value->failure = "runtime";
    } else if (!laghu_operational_registry_snapshot(&worker->queue->operational,
                                                    &operational->snapshot)) {
      proxy_send_admin_json(client, 503U, "Service Unavailable",
                            "{\"status\":\"unavailable\"}", head);
      access_value->status = 503U;
      access_value->failure = "runtime";
    } else {
      bool cache_ready =
          plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_METRICS ||
          (proxy_cache_probe(options->service.image_cache) &&
           laghu_cache_backend_health_path(options->service.image_cache,
                                           &stats));
      if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_READINESS)
        laghu_operational_registry_cache(&worker->queue->operational, &stats);
      if (!laghu_http_administrative_render_operational(
              &plan, &operational->snapshot, (uint64_t)time(NULL),
              proxy_state(worker->queue) == PROXY_RUNNING, cache_ready,
              options->service.readiness_strict, operational->output,
              sizeof(operational->output), &response)) {
        proxy_send_admin_json(client, 503U, "Service Unavailable",
                              "{\"status\":\"unavailable\"}", head);
        access_value->status = 503U;
        access_value->failure = "runtime";
      } else if (response.content ==
                 LAGHU_HTTP_ADMINISTRATIVE_CONTENT_PROMETHEUS) {
        proxy_send_metrics(client, operational->output, response.length, head);
        access_value->status = response.status;
        access_value->output_bytes = head ? 0U : response.length;
      } else {
        proxy_send_admin_json(
            client, response.status,
            response.status == 200U ? "OK" : "Service Unavailable",
            operational->output, head);
        access_value->status = response.status;
        access_value->output_bytes = head ? 0U : response.length;
        access_value->failure = response.status == 200U ? "none" : "readiness";
      }
    }
    free(operational);
  } else if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_STATS) {
    laghu_cache_stats stats = {0};
    laghu_http_administrative_response response;
    char json[1536];
    if (!laghu_cache_backend_health_path(options->service.image_cache,
                                         &stats)) {
      proxy_send_admin_json(client, 503U, "Service Unavailable",
                            "{\"status\":\"unavailable\"}", head);
      access_value->status = 503U;
      access_value->failure = "cache";
    } else if (!laghu_http_administrative_render_stats(
                   &options->service.cache_limits, &stats, json, sizeof(json),
                   &response)) {
      proxy_send_admin_json(client, 503U, "Service Unavailable",
                            "{\"status\":\"unavailable\"}", head);
      access_value->status = 503U;
      access_value->failure = "runtime";
    } else {
      proxy_send_admin_json(client, response.status, "OK", json, head);
      access_value->status = response.status;
      access_value->output_bytes = head ? 0U : response.length;
    }
  } else {
    uint64_t matched = 0U;
    laghu_cache_purge_result purged = laghu_cache_backend_purge_url_path(
        options->service.image_cache, plan.normalized_path,
        (uint64_t)time(NULL), &matched);
    laghu_http_administrative_response response;
    char json[192];
    if (!laghu_http_administrative_render_purge(purged, matched, json,
                                                sizeof(json), &response)) {
      proxy_admin_fail(client, access_value, 503U, "Service Unavailable",
                       "runtime");
    } else {
      const char *reason = response.status == 202U   ? "Accepted"
                           : response.status == 429U ? "Too Many Requests"
                           : response.status == 400U ? "Bad Request"
                                                     : "Service Unavailable";
      proxy_send_admin_json(client, response.status, reason, json, false);
      access_value->status = response.status;
      access_value->failure = response.status == 202U ? "none" : "cache";
      access_value->output_bytes = response.length;
    }
  }
  return true;
}
