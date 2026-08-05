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

#define PROXY_FAIL(status_value, reason_value, failure_value)     \
  do {                                                            \
    proxy_error_response(client, (status_value), (reason_value)); \
    access.status = (status_value);                               \
    access.failure = (failure_value);                             \
  } while (0)

bool proxy_handle_administrative_routes(const proxy_connection *connection,
                                        proxy_worker *worker,
                                        proxy_request *request_value,
                                        proxy_access_log *access_value) {
  const laghu_proxy_options *options = worker->queue->options;
  laghu_socket client = connection->socket;
#define request (*request_value)
#define access (*access_value)
  {
    bool purge_control = false;
    char purge_target[LAGHU_RUNTIME_PATH_SIZE];
    bool normalized = laghu_cache_source_normalize(
        request.target, purge_target, sizeof(purge_target), &purge_control);
    bool purge_request = strcmp(request.method, "PURGE") == 0 ||
                         purge_control ||
                         strstr(request.target, "laghu=purge") != NULL;
    bool stats_request =
        normalized && strcmp(purge_target, "/.laghu/stats") == 0;
    bool metrics_request =
        normalized && strcmp(purge_target, "/.laghu/metrics") == 0;
    bool readiness_request =
        normalized && strcmp(purge_target, "/.laghu/ready") == 0;
    if ((metrics_request && !options->metrics) ||
        (readiness_request && !options->readiness)) {
      PROXY_FAIL(404U, "Not Found", "request_limit");
      return true;
    }
    if (purge_request || stats_request || metrics_request ||
        readiness_request) {
      bool head = strcmp(request.method, "HEAD") == 0;
      bool authorized = proxy_peer_in_cidrs(connection, options->purge_allow,
                                            options->purge_allow_count) &&
                        proxy_admin_token(options, &request);
      if (!normalized) {
        proxy_send_admin_json(client, 400U, "Bad Request",
                              "{\"status\":\"malformed\"}", false);
        access.status = 400U;
        access.failure = "admin_malformed";
      } else if (!authorized) {
        proxy_send_admin_json(client, 403U, "Forbidden",
                              "{\"status\":\"forbidden\"}", head);
        access.status = 403U;
        access.failure = "admin_auth";
      } else if (metrics_request || readiness_request) {
        (void)laghu_operational_registry_heartbeat(
            &worker->queue->operational, (uint64_t)time(NULL), true, 0U, 0U);
        proxy_operational_response *operational =
            calloc(1U, sizeof(*operational));
        laghu_cache_stats stats = {0};
        size_t output_length = 0U;
        bool enabled = metrics_request ? options->metrics : options->readiness;
        bool method = strcmp(request.method, "GET") == 0 || head;
        if (operational == NULL) {
          proxy_send_admin_json(client, 503U, "Service Unavailable",
                                "{\"status\":\"unavailable\"}", head);
          access.status = 503U;
          access.failure = "runtime";
        } else if (!enabled || !method) {
          proxy_send_admin_json(client, 405U, "Method Not Allowed",
                                "{\"status\":\"method_not_allowed\"}", head);
          access.status = 405U;
          access.failure = "admin_method";
        } else if (!laghu_operational_registry_snapshot(
                       &worker->queue->operational, &operational->snapshot)) {
          proxy_send_admin_json(client, 503U, "Service Unavailable",
                                "{\"status\":\"unavailable\"}", head);
          access.status = 503U;
          access.failure = "runtime";
        } else if (metrics_request &&
                   laghu_operational_render_prometheus(
                       &operational->snapshot, (uint64_t)time(NULL),
                       operational->output, sizeof(operational->output),
                       &output_length)) {
          proxy_send_metrics(client, operational->output, output_length, head);
          access.status = 200U;
          access.output_bytes = head ? 0U : output_length;
        } else if (readiness_request) {
          laghu_operational_readiness readiness;
          bool cache_ready =
              proxy_cache_probe(options->cache_path) &&
              laghu_cache_backend_health_path(options->cache_path, &stats);
          laghu_operational_registry_cache(&worker->queue->operational, &stats);
          if (!laghu_operational_readiness_evaluate(
                  &operational->snapshot, (uint64_t)time(NULL),
                  proxy_state(worker->queue) == PROXY_RUNNING, cache_ready,
                  options->readiness_strict, &readiness) ||
              !laghu_operational_render_readiness(
                  &readiness, options->readiness_strict, operational->output,
                  sizeof(operational->output), &output_length)) {
            proxy_send_admin_json(client, 503U, "Service Unavailable",
                                  "{\"status\":\"unavailable\"}", head);
            access.status = 503U;
            access.failure = "runtime";
          } else {
            unsigned int status = readiness.runtime_ready &&
                                          readiness.cache_ready &&
                                          readiness.workers_ready
                                      ? 200U
                                      : 503U;
            proxy_send_admin_json(client, status,
                                  status == 200U ? "OK" : "Service Unavailable",
                                  operational->output, head);
            access.status = status;
            access.output_bytes = head ? 0U : output_length;
            access.failure = status == 200U ? "none" : "readiness";
          }
        } else {
          proxy_send_admin_json(client, 503U, "Service Unavailable",
                                "{\"status\":\"unavailable\"}", head);
          access.status = 503U;
          access.failure = "runtime";
        }
        free(operational);
      } else if (stats_request) {
        laghu_cache_stats stats = {0};
        char json[1536];
        uint64_t requests;
        if (!options->statistics ||
            (strcmp(request.method, "GET") != 0 && !head)) {
          proxy_send_admin_json(client, 405U, "Method Not Allowed",
                                "{\"status\":\"method_not_allowed\"}", head);
          access.status = 405U;
          access.failure = "admin_method";
        } else if (!laghu_cache_backend_health_path(options->cache_path,
                                                    &stats)) {
          proxy_send_admin_json(client, 503U, "Service Unavailable",
                                "{\"status\":\"unavailable\"}", head);
          access.status = 503U;
          access.failure = "cache";
        } else {
          requests = stats.hits + stats.misses;
          (void)snprintf(
              json, sizeof(json),
              "{\"schema\":\"laghu-cache-stats-v1\",\"backend\":\"file\","
              "\"capacity\":{\"bytes\":%llu,\"files\":%llu},"
              "\"usage\":{\"bytes\":%llu,\"files\":%llu},"
              "\"requests\":{\"hits\":%llu,\"misses\":%llu,"
              "\"hit_ratio_ppm\":%llu},\"publications\":%llu,"
              "\"rejected_writes\":%llu,\"evictions\":%llu,"
              "\"purges\":{\"url\":%llu,\"full\":%llu,"
              "\"artifacts\":%llu,\"bytes\":%llu,\"generation\":%llu,"
              "\"last\":%llu},\"corrupt_removals\":%llu,"
              "\"cleaner_active\":%s,\"rebuilding\":%s,"
              "\"last_maintenance\":%llu}",
              (unsigned long long)options->cache_limits.size_limit,
              (unsigned long long)options->cache_limits.inode_limit,
              (unsigned long long)stats.bytes, (unsigned long long)stats.files,
              (unsigned long long)stats.hits, (unsigned long long)stats.misses,
              (unsigned long long)(requests == 0U
                                       ? 0U
                                       : stats.hits * UINT64_C(1000000) /
                                             requests),
              (unsigned long long)stats.publications,
              (unsigned long long)stats.rejected_publications,
              (unsigned long long)stats.evictions,
              (unsigned long long)stats.url_purges,
              (unsigned long long)stats.full_purges,
              (unsigned long long)stats.invalidated_artifacts,
              (unsigned long long)stats.invalidated_bytes,
              (unsigned long long)stats.cache_generation,
              (unsigned long long)stats.last_purge,
              (unsigned long long)stats.corrupt_removals,
              stats.cleaner_active ? "true" : "false",
              stats.rebuilding ? "true" : "false",
              (unsigned long long)stats.last_cleanup);
          proxy_send_admin_json(client, 200U, "OK", json, head);
          access.status = 200U;
          access.output_bytes = head ? 0U : strlen(json);
        }
      } else if ((strcmp(request.method, "PURGE") == 0 &&
                  !options->purge_method) ||
                 (purge_control && !options->purge_query) ||
                 (strcmp(request.method, "PURGE") != 0 &&
                  strcmp(request.method, "GET") != 0)) {
        proxy_send_admin_json(client, 405U, "Method Not Allowed",
                              "{\"status\":\"method_not_allowed\"}", false);
        access.status = 405U;
        access.failure = "admin_method";
      } else {
        uint64_t matched = 0U;
        laghu_cache_purge_result purged = laghu_cache_backend_purge_url_path(
            options->cache_path, purge_target, (uint64_t)time(NULL), &matched);
        unsigned int status =
            purged == LAGHU_CACHE_PURGE_ACCEPTED
                ? 202U
                : (purged == LAGHU_CACHE_PURGE_SATURATED
                       ? 429U
                       : (purged == LAGHU_CACHE_PURGE_INVALID ? 400U : 503U));
        char json[192];
        const char *reason = status == 202U   ? "Accepted"
                             : status == 429U ? "Too Many Requests"
                             : status == 400U ? "Bad Request"
                                              : "Service Unavailable";
        (void)snprintf(json, sizeof(json),
                       "{\"status\":\"%s\",\"matched_artifacts\":%llu}",
                       status == 202U ? "accepted" : "rejected",
                       (unsigned long long)matched);
        proxy_send_admin_json(client, status, reason, json, false);
        access.status = status;
        access.failure = status == 202U ? "none" : "cache";
        access.output_bytes = strlen(json);
      }
      return true;
    }
  }
  return false;
#undef access
#undef request
}

#undef PROXY_FAIL
