// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stddef.h>
#include <stdint.h>

#include "laghu/image.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  laghu_image_markup_options options = {0};
  laghu_image_markup_result result;
  laghu_buffer input = {data, size};

  (void)laghu_image_detect_format(input);
  if (laghu_image_rewrite_html(input, &options, &result)) {
    laghu_image_markup_result_release(&result);
  }
  if (laghu_image_rewrite_css(input, &options, &result)) {
    laghu_image_markup_result_release(&result);
  }
  return 0;
}
