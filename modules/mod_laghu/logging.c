// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/log.h"
#include "mod_laghu_internal.h"

static apr_time_t laghu_apache_last_defer_recommendation;

void laghu_apache_log_transaction(request_rec *request,
                                  laghu_apache_context *context,
                                  const laghu_http_transaction_result *result,
                                  const char *failure) {
  char line[LAGHU_LOG_LINE_SIZE];
  const char *cache = "none";
  const char *decision = "bypass-error";
  uint64_t original = 0U, output = 0U;
  bool job_published = false;
  const char *trace_id = NULL;
  const char *span_id = NULL;
  if (request == NULL || context == NULL || context->log_written) return;
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
        .common =
            {(time_t)apr_time_sec(apr_time_now()), "apache", "apache", trace_id,
             span_id},
        .method = request->method == NULL ? "unknown" : request->method,
        .path = request->uri == NULL ? "/" : request->uri,
        .status = (unsigned int)request->status,
        .decision = decision,
        .request_bytes = request->read_length > 0 ? request->read_length : 0U,
        .original_bytes = original,
        .output_bytes = output,
        .duration_ms =
            context->log_started == 0
                ? 0U
                : (uint64_t)((apr_time_now() - context->log_started) / 1000),
        .cache = cache,
        .job_published = job_published,
        .failure = failure == NULL ? "none" : failure};
    if (laghu_log_render_transaction(&record, line, sizeof(line))) {
      ap_log_rerror(APLOG_MARK, APLOG_NOTICE, 0, request, "laghu_json=%s",
                    line);
      context->log_written = true;
    }
  }
}

void laghu_apache_log_defer_recommendation(
    request_rec *request, const laghu_http_transaction_result *result) {
  apr_time_t now;
  if (!result->javascript_defer_recommended &&
      !result->javascript_defer_rollback_recommended)
    return;
  now = apr_time_now();
  if (laghu_apache_last_defer_recommendation != 0 &&
      now - laghu_apache_last_defer_recommendation < apr_time_from_sec(3600))
    return;
  laghu_apache_last_defer_recommendation = now;
  ap_log_rerror(APLOG_MARK, APLOG_NOTICE, 0, request,
                "Laghu event=%s path=\"%s\" "
                "template=%s bucket=%u observations=%llu approval=\"defer %s\"",
                result->javascript_defer_rollback_recommended
                    ? "javascript_defer_rollback_recommendation"
                    : "javascript_defer_recommendation",
                result->javascript_defer_path,
                result->javascript_defer_template,
                result->javascript_defer_bucket,
                (unsigned long long)result->javascript_defer_observations,
                result->javascript_defer_path);
}
