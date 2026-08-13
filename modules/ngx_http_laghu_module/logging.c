// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>

#include "laghu/log.h"
#include "ngx_http_laghu_internal.h"

static time_t ngx_http_laghu_last_defer_recommendation;

void ngx_http_laghu_log_transaction(ngx_http_request_t *request,
                                    ngx_http_laghu_request_ctx_t *context,
                                    const laghu_http_transaction_result *result,
                                    const char *failure) {
  char method[17U], path[LAGHU_RUNTIME_PATH_SIZE + 1U];
  char line[LAGHU_LOG_LINE_SIZE];
  size_t method_length, path_length;
  const char *cache = "none";
  const char *decision = "bypass-error";
  uint64_t original = 0U, output = 0U;
  bool job_published = false;
  const char *trace_id = NULL;
  const char *span_id = NULL;
  if (request == NULL || context == NULL || context->log_written) return;
  method_length = request->method_name.len < sizeof(method) - 1U
                      ? request->method_name.len
                      : sizeof(method) - 1U;
  path_length = request->uri.len < sizeof(path) - 1U ? request->uri.len
                                                     : sizeof(path) - 1U;
  ngx_memcpy(method, request->method_name.data, method_length);
  method[method_length] = '\0';
  ngx_memcpy(path, request->uri.data, path_length);
  path[path_length] = '\0';
  if (result != NULL) {
    decision = laghu_decision_name(result->decision);
    original = result->original.length;
    output = result->selected.length;
    job_published = result->job_published;
    if (result->action == LAGHU_HTTP_ACTION_SERVE_CACHED)
      cache = "warm";
    else if (result->action != LAGHU_HTTP_ACTION_BYPASS)
      cache = "cold";
  }
  trace_id = context->trace_id[0] == '\0' ? NULL : context->trace_id;
  span_id = context->span_id[0] == '\0' ? NULL : context->span_id;
  {
    laghu_log_transaction record = {
        .common = {(time_t)ngx_time(), "nginx", "nginx", trace_id, span_id},
        .method = method_length == 0U ? "unknown" : method,
        .path = path_length == 0U ? "/" : path,
        .status = request->headers_out.status,
        .decision = decision,
        .request_bytes = request->request_length,
        .original_bytes = original,
        .output_bytes = output,
        .duration_ms = (ngx_current_msec - context->log_started_ms),
        .cache = cache,
        .job_published = job_published,
        .failure = failure == NULL ? "none" : failure};
    if (laghu_log_render_transaction(&record, line, sizeof(line))) {
      ngx_log_error(NGX_LOG_NOTICE, request->connection->log, 0,
                    "laghu_json=%s", line);
      context->log_written = true;
    }
  }
}

void ngx_http_laghu_log_defer_recommendation(
    ngx_http_request_t *request, const laghu_http_transaction_result *result) {
  time_t now;
  if (!result->javascript_defer_recommended &&
      !result->javascript_defer_rollback_recommended)
    return;
  now = ngx_time();
  if (ngx_http_laghu_last_defer_recommendation != 0 &&
      now - ngx_http_laghu_last_defer_recommendation < 3600)
    return;
  ngx_http_laghu_last_defer_recommendation = now;
  ngx_log_error(NGX_LOG_NOTICE, request->connection->log, 0,
                "laghu event=%s path=\"%s\" "
                "template=%s bucket=%ui observations=%uL approval=\"defer %s\"",
                result->javascript_defer_rollback_recommended
                    ? "javascript_defer_rollback_recommendation"
                    : "javascript_defer_recommendation",
                result->javascript_defer_path,
                result->javascript_defer_template,
                (ngx_uint_t)result->javascript_defer_bucket,
                (uint64_t)result->javascript_defer_observations,
                result->javascript_defer_path);
}
