// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/log.h"

#include <assert.h>
#include <string.h>

int main(void) {
  char output[LAGHU_LOG_LINE_SIZE];
  laghu_log_transaction record = {.common = {0, "standalone", "standalone", NULL, NULL},
                                  .method = "GET",
                                  .path = "/safe?token=secret\n",
                                  .status = 200U,
                                  .decision = "pass",
                                  .cache = "cold",
                                  .failure = "none",
                                  .route = "static",
                                  .upstream = "127.0.0.1:9000",
                                  .upstream_protocol = "fastcgi",
                                  .access = "basic_auth",
                                  .failovers = 1U,
                                  .static_response = true,
                                  .spa_fallback = true,
                                  .compressed = true,
                                  .rate_limited = true};
  assert(laghu_log_render_transaction(&record, output, sizeof(output)));
  assert(strstr(output, "laghu-log") != NULL);
  assert(strstr(output, "token=secret") == NULL);
  assert(strstr(output, "\"path\":\"/safe\"") != NULL);
  assert(strstr(output, "\"route\":\"static\"") != NULL);
  assert(strstr(output, "\"upstream\":\"127.0.0.1:9000\"") != NULL);
  assert(strstr(output, "\"upstream_protocol\":\"fastcgi\"") != NULL);
  assert(strstr(output, "\"access\":\"basic_auth\"") != NULL);
  assert(strstr(output, "\"failovers\":1") != NULL);
  assert(strstr(output, "\"static\":true") != NULL && strstr(output, "\"spa_fallback\":true") != NULL &&
         strstr(output, "\"compressed\":true") != NULL && strstr(output, "\"rate_limited\":true") != NULL);

  record.common.trace_id = "0123456789abcdef0123456789abcdef";
  record.common.span_id = "0123456789abcdef";
  assert(laghu_log_render_transaction(&record, output, sizeof(output)));
  assert(strstr(output, "\"trace_id\":\"0123456789abcdef0123456789abcdef\"") != NULL);
  assert(strstr(output, "\"span_id\":\"0123456789abcdef\"") != NULL);

  record.common.trace_id = NULL;
  record.common.span_id = NULL;
  assert(laghu_log_render_transaction(&record, output, sizeof(output)));
  assert(strstr(output, "\"trace_id\"") == NULL);
  assert(strstr(output, "\"span_id\"") == NULL);
  assert(!laghu_log_render_transaction(&record, output, 32U));
  {
    laghu_log_job job = {.common = {0, "worker", "libvips", NULL, NULL},
                         .job_kind = "image",
                         .outcome = "success",
                         .filters = 42U,
                         .accept_webp = true,
                         .accept_avif = false,
                         .failure = "none"};
    assert(laghu_log_render_job(&job, output, sizeof(output)));
    assert(strstr(output, "\"filters\":42") != NULL);
    assert(strstr(output, "\"accept_webp\":true") != NULL);
    assert(strstr(output, "\"accept_avif\":false") != NULL);
  }
  return 0;
}
