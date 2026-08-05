// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/css.h"

#include <assert.h>

int main(void) {
  static const unsigned char html[] = "<p>unchanged</p>";
  laghu_runtime_html_result result;
  assert(laghu_runtime_rewrite_css_markup(
      "/nonexistent", (laghu_buffer){html, sizeof(html) - 1U}, "/",
      "https://example.test", "policy", 0U, 1U, 60U, true, true, true, 0U, true,
      true, 1024U, 1024U, &result));
  assert(!result.rewritten);
  laghu_runtime_html_result_release(&result);
  return 0;
}
