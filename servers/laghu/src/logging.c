// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "server_internal.h"

static bool proxy_json_escape(const char *input, char *output,
                              size_t capacity) {
  size_t used = 0U;
  const unsigned char *cursor = (const unsigned char *)input;
  if (capacity == 0U) return false;
  while (*cursor != '\0') {
    const char *escape = NULL;
    char unicode[7];
    size_t length;
    if (*cursor == '"')
      escape = "\\\"";
    else if (*cursor == '\\')
      escape = "\\\\";
    else if (*cursor == '\n')
      escape = "\\n";
    else if (*cursor == '\r')
      escape = "\\r";
    else if (*cursor == '\t')
      escape = "\\t";
    else if (*cursor < 32U) {
      (void)snprintf(unicode, sizeof(unicode), "\\u%04x", *cursor);
      escape = unicode;
    }
    length = escape == NULL ? 1U : strlen(escape);
    if (used + length >= capacity) return false;
    if (escape == NULL)
      output[used++] = (char)*cursor;
    else {
      memcpy(output + used, escape, length);
      used += length;
    }
    ++cursor;
  }
  output[used] = '\0';
  return true;
}

static void proxy_timestamp(char output[32]) {
  time_t now = time(NULL);
  struct tm value;
#ifdef _WIN32
  (void)gmtime_s(&value, &now);
#else
  (void)gmtime_r(&now, &value);
#endif
  if (strftime(output, 32U, "%Y-%m-%dT%H:%M:%SZ", &value) == 0U)
    memcpy(output, "1970-01-01T00:00:00Z", 21U);
}

static void proxy_log_line(proxy_queue *queue, const char *line) {
  if (queue != NULL) proxy_queue_lock(queue);
  (void)fwrite(line, 1U, strlen(line), stderr);
  (void)fputc('\n', stderr);
  (void)fflush(stderr);
  if (queue != NULL) proxy_queue_unlock(queue);
}

void proxy_log_event(proxy_queue *queue, const char *event, const char *state) {
  char timestamp[32], line[512];
  proxy_timestamp(timestamp);
  (void)snprintf(line, sizeof(line),
                 "{\"timestamp\":\"%s\",\"event\":\"%s\","
                 "\"state\":\"%s\"}",
                 timestamp, event, state);
  proxy_log_line(queue, line);
}

void proxy_log_startup_failure(proxy_queue *queue, const char *failure) {
  char timestamp[32], line[512];
  proxy_timestamp(timestamp);
  (void)snprintf(line, sizeof(line),
                 "{\"timestamp\":\"%s\",\"event\":\"startup_failure\","
                 "\"state\":\"stopped\",\"failure\":\"%s\"}",
                 timestamp, failure);
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
}

void proxy_access_write(proxy_queue *queue, const proxy_access_log *access) {
  char timestamp[32], method[64], path[LAGHU_RUNTIME_PATH_SIZE * 2U];
  char defer_path[LAGHU_RUNTIME_PATH_SIZE * 2U];
  char line[LAGHU_RUNTIME_PATH_SIZE * 4U + 1400U];
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
  proxy_timestamp(timestamp);
  if (!proxy_json_escape(access->method[0] ? access->method : "unknown", method,
                         sizeof(method)) ||
      !proxy_json_escape(access->path[0] ? access->path : "/", path,
                         sizeof(path)) ||
      !proxy_json_escape(access->javascript_defer_path, defer_path,
                         sizeof(defer_path)))
    return;
  (void)snprintf(
      line, sizeof(line),
      "{\"timestamp\":\"%s\",\"event\":\"transaction\","
      "\"request_id\":\"%016llx\",\"method\":\"%s\","
      "\"path\":\"%s\",\"status\":%u,\"decision\":\"%s\","
      "\"input_bytes\":%zu,\"output_bytes\":%zu,"
      "\"duration_ms\":%llu,\"cache\":\"%s\","
      "\"job_published\":%s,\"failure\":\"%s\","
      "\"javascript_defer_recommended\":%s,"
      "\"javascript_defer_rollback_recommended\":%s,"
      "\"javascript_defer_path\":\"%s\","
      "\"javascript_defer_template\":\"%s\","
      "\"javascript_defer_bucket\":%u,"
      "\"javascript_defer_observations\":%llu}",
      timestamp, (unsigned long long)access->request_id, method, path,
      access->status, laghu_decision_name(access->decision),
      access->input_bytes, access->output_bytes, (unsigned long long)elapsed,
      access->cache_state, access->job_published ? "true" : "false",
      access->failure, access->javascript_defer_recommended ? "true" : "false",
      access->javascript_defer_rollback_recommended ? "true" : "false",
      defer_path, access->javascript_defer_template,
      access->javascript_defer_bucket,
      (unsigned long long)access->javascript_defer_observations);
  proxy_log_line(queue, line);
}
