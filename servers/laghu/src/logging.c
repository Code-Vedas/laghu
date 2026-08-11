// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>

#include "laghu/log.h"
#include "server_internal.h"

static void proxy_access_generate_trace_ids(proxy_access_log *access) {
  uint64_t trace_high = UINT64_C(1469598103934665603);
  uint64_t trace_low = UINT64_C(1099511628211);
  if (access == NULL) return;
  trace_high ^= access->request_id;
  trace_high *= UINT64_C(1099511628211);
  trace_low ^= access->started_ms;
  trace_low *= UINT64_C(1099511628211);
  if (trace_high == 0U && trace_low == 0U) trace_high = UINT64_C(1);
  (void)snprintf(access->trace_id, sizeof(access->trace_id),
                 "%016" PRIx64 "%016" PRIx64, trace_high, trace_low);
  (void)snprintf(access->span_id, sizeof(access->span_id),
                 "%016" PRIx64, trace_high ^ trace_low);
}

static void proxy_log_line(proxy_queue *queue, const char *line) {
  if (queue != NULL) proxy_queue_lock(queue);
  (void)fwrite(line, 1U, strlen(line), stderr);
  (void)fputc('\n', stderr);
  (void)fflush(stderr);
  if (queue != NULL) proxy_queue_unlock(queue);
}

void proxy_log_event(proxy_queue *queue, const char *event, const char *state) {
  char line[LAGHU_LOG_LINE_SIZE];
  laghu_log_lifecycle record = {
      .common = {(time_t)time(NULL), "standalone", "standalone", NULL, NULL},
      .state = state == NULL ? "failed" : state,
      .failure = event != NULL && strcmp(event, "startup_failure") == 0
                     ? "runtime"
                     : "none"};
  if (laghu_log_render_lifecycle(&record, line, sizeof(line)))
    proxy_log_line(queue, line);
}

void proxy_log_startup_failure(proxy_queue *queue, const char *failure) {
  char line[LAGHU_LOG_LINE_SIZE];
  laghu_log_lifecycle record = {
      .common = {(time_t)time(NULL), "standalone", "standalone", NULL, NULL},
      .state = "failed",
      .failure = failure == NULL ? "runtime" : failure};
  if (laghu_log_render_lifecycle(&record, line, sizeof(line)))
    proxy_log_line(queue, line);
}

void proxy_access_init(proxy_access_log *access, proxy_queue *queue) {
  memset(access, 0, sizeof(*access));
  access->started_ms = proxy_monotonic_ms();
  access->decision = LAGHU_DECISION_BYPASS_ERROR;
  access->cache_state = "none";
  access->failure = "none";
  proxy_queue_lock(queue);
  access->request_id = queue->request_prefix + ++queue->request_counter;
  proxy_queue_unlock(queue);
  proxy_access_generate_trace_ids(access);
}

void proxy_access_write(proxy_queue *queue, const proxy_access_log *access) {
  char line[LAGHU_LOG_LINE_SIZE];
  uint64_t elapsed = proxy_monotonic_ms() - access->started_ms;
  (void)laghu_operational_registry_heartbeat(
      &queue->operational, (uint64_t)time(NULL), true, 0U, 0U);
  laghu_operational_decision operational_decision =
      access->output_bytes < access->original_response_bytes
          ? LAGHU_OPERATIONAL_DECISION_OPTIMIZED
          : LAGHU_OPERATIONAL_DECISION_ORIGINAL;
  if (access->decision != LAGHU_DECISION_PASS &&
      access->decision != LAGHU_DECISION_IMAGE_HIT)
    operational_decision = LAGHU_OPERATIONAL_DECISION_BYPASS;
  if (strcmp(access->cache_state, "hit") == 0)
    operational_decision = LAGHU_OPERATIONAL_DECISION_CACHED;
  else if (access->job_published)
    operational_decision = LAGHU_OPERATIONAL_DECISION_QUEUED;
  laghu_operational_registry_record(&queue->operational, operational_decision,
                                    access->original_response_bytes,
                                    access->output_bytes,
                                    elapsed * UINT64_C(1000));
  if (strcmp(access->failure, "none") != 0)
    laghu_operational_registry_failure(&queue->operational,
                                       LAGHU_OPERATIONAL_FAILURE_TRANSPORT);
  {
    laghu_log_transaction record = {
        .common =
            {(time_t)time(NULL), "standalone", "standalone",
             access->trace_id[0] == '\0' ? NULL : access->trace_id,
             access->span_id[0] == '\0' ? NULL : access->span_id},
        .method = access->method[0] ? access->method : "unknown",
        .path = access->path[0] ? access->path : "/",
        .status = access->status,
        .decision = laghu_decision_name(access->decision),
        .request_bytes = access->input_bytes,
        .original_bytes = access->original_response_bytes,
        .output_bytes = access->output_bytes,
        .duration_ms = elapsed,
        .cache = access->cache_state,
        .job_published = access->job_published,
        .failure = access->failure};
    if (laghu_log_render_transaction(&record, line, sizeof(line)))
      proxy_log_line(queue, line);
  }
}
