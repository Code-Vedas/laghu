// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stddef.h>
#include <stdint.h>

#include "laghu/runtime.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  laghu_runtime_html_result result;
  laghu_runtime_head_result head;
  laghu_html_planner_mask plan =
      size == 0U ? LAGHU_HTML_PLAN_LEXICAL
                 : (laghu_html_planner_mask)(data[0] & UINT8_C(0x7f));
  if (size > 1U && (data[1] & UINT8_C(1)) != 0U) {
    plan |= LAGHU_HTML_PLAN_CONVERT_META_TAGS | LAGHU_HTML_PLAN_RESOURCE_HINTS;
  }
  if (size > 1U && (data[1] & UINT8_C(2)) != 0U)
    plan |= LAGHU_HTML_PLAN_TRIM_URLS;
  if (laghu_runtime_plan_html_document_at(
          (laghu_buffer){data, size}, "/fuzz/page.html", "https://example.test",
          plan, &head)) {
    laghu_runtime_head_result_release(&head);
  }
  if (laghu_runtime_rewrite_css_markup(
          "/laghu-fuzz-cache-does-not-exist", (laghu_buffer){data, size},
          "/fuzz/page.html", "https://example.test",
          "0000000000000000000000000000000000000000000000000000000000000000",
          0U, 1U, 1U, true, true, true, plan, true, true, 2048U, 8192U,
          &result)) {
    laghu_runtime_html_result_release(&result);
  }
  if (laghu_runtime_finalize_html_headers(
          "/laghu-fuzz-cache-does-not-exist", (laghu_buffer){data, size},
          "/fuzz/page.html", "https://example.test",
          "0000000000000000000000000000000000000000000000000000000000000000",
          0U, 1U, 1U, plan, "en", "", 2048U, 8192U, true, &result)) {
    laghu_runtime_html_result_release(&result);
  }
  return 0;
}
