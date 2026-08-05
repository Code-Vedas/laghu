// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "mod_laghu_internal.h"

static apr_time_t laghu_apache_last_defer_recommendation;

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
