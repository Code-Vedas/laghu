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

static void proxy_admin_fail(laghu_socket client, SSL *tls, proxy_access_log *access, unsigned int status, const char *reason,
                             const char *failure) {
  proxy_error_response(client, tls, status, reason);
  access->status = status;
  access->failure = failure;
}

bool proxy_handle_administrative_routes(const proxy_connection *connection, proxy_worker *worker, proxy_request *request_value,
                                        const laghu_service_config *service, proxy_access_log *access_value) {
  laghu_socket client = connection->socket;
  SSL *tls = connection->tls;
  laghu_http_administrative_options admin_options = {service->metrics, service->readiness, service->statistics, service->purge_method,
                                                     service->purge_query, true};
  laghu_http_administrative_plan plan;
  laghu_buffer method = {(const unsigned char *)request_value->method, strlen(request_value->method)};
  laghu_buffer target = {(const unsigned char *)request_value->target, strlen(request_value->target)};
  if (!laghu_http_administrative_plan_build(&plan, method, target, &admin_options)) {
    return false;
  }
  if (!plan.recognized) return false;
  if (plan.status == 404U) {
    proxy_admin_fail(client, tls, access_value, 404U, "Not Found", "request_limit");
    return true;
  }
  bool head = strcmp(request_value->method, "HEAD") == 0;
  bool authorized =
      proxy_peer_in_cidrs(connection, service->purge_allow, service->purge_allow_count) && proxy_admin_token(service, request_value);
  if (!authorized) {
    proxy_send_admin_json(client, tls, 403U, "Forbidden", "{\"status\":\"forbidden\"}", head);
    access_value->status = 403U;
    access_value->failure = "admin_auth";
  } else if (plan.status == 400U) {
    proxy_send_admin_json(client, tls, 400U, "Bad Request", "{\"status\":\"malformed\"}", false);
    access_value->status = 400U;
    access_value->failure = "admin_malformed";
  } else if (plan.status == 405U) {
    proxy_send_admin_json(client, tls, 405U, "Method Not Allowed", "{\"status\":\"method_not_allowed\"}", head);
    access_value->status = 405U;
    access_value->failure = "admin_method";
  } else if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_METRICS || plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_READINESS) {
    (void)laghu_operational_registry_heartbeat(&worker->queue->operational, (uint64_t)time(NULL), true, 0U, 0U);
    proxy_operational_response *operational = calloc(1U, sizeof(*operational));
    laghu_cache_stats stats = {0};
    laghu_http_administrative_response response;
    if (operational == NULL) {
      proxy_send_admin_json(client, tls, 503U, "Service Unavailable", "{\"status\":\"unavailable\"}", head);
      access_value->status = 503U;
      access_value->failure = "runtime";
    } else if (!laghu_operational_registry_snapshot(&worker->queue->operational, &operational->snapshot)) {
      proxy_send_admin_json(client, tls, 503U, "Service Unavailable", "{\"status\":\"unavailable\"}", head);
      access_value->status = 503U;
      access_value->failure = "runtime";
    } else {
      bool cache_ready = plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_METRICS ||
                         (proxy_cache_probe(service->image_cache) && laghu_cache_backend_health_path(service->image_cache, &stats));
      if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_READINESS) laghu_operational_registry_cache(&worker->queue->operational, &stats);
      if (!laghu_http_administrative_render_operational(&plan, &operational->snapshot, (uint64_t)time(NULL),
                                                        proxy_state(worker->queue) == PROXY_RUNNING, cache_ready, service->readiness_strict,
                                                        operational->output, sizeof(operational->output), &response)) {
        proxy_send_admin_json(client, tls, 503U, "Service Unavailable", "{\"status\":\"unavailable\"}", head);
        access_value->status = 503U;
        access_value->failure = "runtime";
      } else if (response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_PROMETHEUS) {
        proxy_send_metrics(client, tls, operational->output, response.length, head);
        access_value->status = response.status;
        access_value->output_bytes = head ? 0U : response.length;
      } else {
        proxy_send_admin_json(client, tls, response.status, response.status == 200U ? "OK" : "Service Unavailable", operational->output, head);
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
    if (!laghu_cache_backend_health_path(service->image_cache, &stats)) {
      proxy_send_admin_json(client, tls, 503U, "Service Unavailable", "{\"status\":\"unavailable\"}", head);
      access_value->status = 503U;
      access_value->failure = "cache";
    } else if (!laghu_http_administrative_render_stats(&service->cache_limits, &stats, json, sizeof(json), &response)) {
      proxy_send_admin_json(client, tls, 503U, "Service Unavailable", "{\"status\":\"unavailable\"}", head);
      access_value->status = 503U;
      access_value->failure = "runtime";
    } else {
      proxy_send_admin_json(client, tls, response.status, "OK", json, head);
      access_value->status = response.status;
      access_value->output_bytes = head ? 0U : response.length;
    }
  } else if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_CONSOLE || plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_HISTORY ||
             plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_EXPLAIN) {
    laghu_operational_snapshot snapshot;
    laghu_operational_readiness readiness;
    laghu_http_administrative_console_page_model page;
    laghu_http_administrative_response response;
    laghu_cache_stats stats = {0};
    bool cache_ready = false;
    char *html = calloc(1U, LAGHU_OPERATIONAL_RENDER_SIZE);
    if (html == NULL) {
      proxy_admin_fail(client, tls, access_value, 503U, "Service Unavailable", "runtime");
      return true;
    }
    if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_CONSOLE || plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_EXPLAIN) {
      if (!proxy_cache_probe(service->image_cache) || !laghu_cache_backend_health_path(service->image_cache, &stats)) {
        proxy_admin_fail(client, tls, access_value, 503U, "Service Unavailable", "runtime");
        free(html);
        return true;
      }
      cache_ready = true;
    } else {
      if (proxy_cache_probe(service->image_cache) && laghu_cache_backend_health_path(service->image_cache, &stats)) {
        cache_ready = true;
      } else {
        cache_ready = false;
      }
    }
    if (!laghu_operational_registry_snapshot(&worker->queue->operational, &snapshot) ||
        !laghu_operational_readiness_evaluate(&snapshot, (uint64_t)time(NULL), proxy_state(worker->queue) == PROXY_RUNNING, cache_ready,
                                              service->readiness_strict, &readiness) ||
        !laghu_http_administrative_build_console_page_model(&plan, &stats, &readiness, &snapshot, &page) ||
        !laghu_http_administrative_render_console_page(&plan, &page, html, LAGHU_OPERATIONAL_RENDER_SIZE, &response)) {
      proxy_admin_fail(client, tls, access_value, 503U, "Service Unavailable", "runtime");
      free(html);
      return true;
    }
    if (response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_JSON) {
      const char *reason = response.status == 200U ? "OK" : "Service Unavailable";
      proxy_send_admin_json(client, tls, response.status, reason, html, head);
    } else {
      proxy_send_admin_html(client, tls, response.status, "OK", html, head);
      if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_EXPLAIN) {
        access_value->failure = response.status == 200U ? "none" : "readiness";
      }
    }
    access_value->status = response.status;
    access_value->output_bytes = head ? 0U : response.length;
    free(html);
  } else {
    uint64_t matched = 0U;
    const char *purge_target = plan.purge_target[0] != '\0' ? plan.purge_target : plan.normalized_path;
    laghu_cache_purge_result purged = laghu_cache_backend_purge_url_path(service->image_cache, purge_target, (uint64_t)time(NULL), &matched);
    laghu_http_administrative_response response;
    char json[192];
    if (!laghu_http_administrative_render_purge(purged, matched, json, sizeof(json), &response)) {
      proxy_admin_fail(client, tls, access_value, 503U, "Service Unavailable", "runtime");
    } else {
      const char *reason = response.status == 202U   ? "Accepted"
                           : response.status == 429U ? "Too Many Requests"
                           : response.status == 400U ? "Bad Request"
                                                     : "Service Unavailable";
      proxy_send_admin_json(client, tls, response.status, reason, json, false);
      access_value->status = response.status;
      access_value->failure = response.status == 202U ? "none" : "cache";
      access_value->output_bytes = response.length;
    }
  }
  return true;
}
