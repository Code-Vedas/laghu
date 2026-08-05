// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>

#include "ngx_http_laghu_internal.h"

static time_t ngx_http_laghu_last_defer_recommendation;

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
