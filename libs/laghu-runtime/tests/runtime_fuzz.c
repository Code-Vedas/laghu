// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stddef.h>
#include <stdint.h>

#include "laghu/runtime.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  laghu_runtime_html_result result;
  if (laghu_runtime_rewrite_css_markup(
          "/laghu-fuzz-cache-does-not-exist", (laghu_buffer){data, size},
          "/fuzz/page.html", "https://example.test",
          "0000000000000000000000000000000000000000000000000000000000000000",
          0U, 1U, 1U, true, true, true, 2048U, 8192U, &result)) {
    laghu_runtime_html_result_release(&result);
  }
  return 0;
}
