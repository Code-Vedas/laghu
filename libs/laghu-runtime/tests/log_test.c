// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/log.h"

#include <assert.h>
#include <string.h>

int main(void) {
  char output[LAGHU_LOG_LINE_SIZE];
  laghu_log_transaction record = {.common = {0, "standalone", "standalone"},
                                  .method = "GET",
                                  .path = "/safe?token=secret\n",
                                  .status = 200U,
                                  .decision = "pass",
                                  .cache = "cold",
                                  .failure = "none"};
  assert(laghu_log_render_transaction(&record, output, sizeof(output)));
  assert(strstr(output, "laghu-log-v1") != NULL);
  assert(strstr(output, "token=secret") == NULL);
  assert(strstr(output, "\"path\":\"/safe\"") != NULL);
  assert(!laghu_log_render_transaction(&record, output, 32U));
  return 0;
}
