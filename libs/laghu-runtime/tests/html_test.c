// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/html.h"

#include <assert.h>
#include <string.h>

int main(void) {
  static const unsigned char html[] = "<title>x</title><p>y</p>";
  laghu_runtime_head_result result;
  assert(laghu_runtime_plan_html_document((laghu_buffer){html, sizeof(html) - 1U}, LAGHU_HTML_PLAN_ADD_COMBINE_HEAD, &result));
  assert((result.rewritten && result.length >= sizeof(html) - 1U) || (!result.rewritten && result.data == NULL));
  laghu_runtime_head_result_release(&result);
  return 0;
}
