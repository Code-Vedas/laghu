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
  laghu_css_parse_result parsed;
  laghu_buffer input = {data, size};

  (void)laghu_image_detect_format(input);
  if (laghu_image_rewrite_html(input, &options, &result)) {
    laghu_image_markup_result_release(&result);
  }
  if (laghu_image_rewrite_css(input, &options, &result)) {
    laghu_image_markup_result_release(&result);
  }
  (void)laghu_css_discover(input, "/fuzz.css", "https://example.test", &parsed);
  (void)laghu_css_discover_style_attributes(input, "/fuzz.html", "https://example.test", &parsed);
  if (laghu_css_fallback_rewrite_urls(input, "/fuzz.css", "https://example.test", &options, &result)) {
    laghu_image_markup_result_release(&result);
  }
  if (laghu_css_rewrite_style_attributes(input, "/fuzz.html", "https://example.test", &options, &result)) {
    laghu_image_markup_result_release(&result);
  }
  return 0;
}
