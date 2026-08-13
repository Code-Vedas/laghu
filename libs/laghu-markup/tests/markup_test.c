// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/markup.h"

#include <stdio.h>

#define CHECK(value)                                 \
  do {                                               \
    if (!(value)) {                                  \
      fprintf(stderr, "check failed: %s\n", #value); \
      return 1;                                      \
    }                                                \
  } while (0)

int main(void) {
  static const unsigned char html[] = "<!-- ignored --><IMG src='hero.webp' hidden data-x=\"a>b\">";
  static const unsigned char srcset[] = "a.webp 1x, b.webp 2x";
  laghu_html_tag tag;
  laghu_html_attribute attribute;
  laghu_srcset_candidate candidate;
  size_t cursor = 0U;
  CHECK(laghu_html_next_tag((laghu_buffer){html, sizeof(html) - 1U}, &cursor, &tag));
  CHECK(laghu_base_ascii_equal(tag.name, "img"));
  CHECK(laghu_html_tag_attribute(&tag, "src", &attribute));
  CHECK(laghu_base_ascii_equal(attribute.value, "hero.webp"));
  CHECK(laghu_html_tag_has_attribute(&tag, "hidden"));
  cursor = 0U;
  CHECK(laghu_srcset_next((laghu_buffer){srcset, sizeof(srcset) - 1U}, &cursor, &candidate));
  CHECK(laghu_base_ascii_equal(candidate.url, "a.webp"));
  CHECK(laghu_base_ascii_equal(candidate.descriptor, "1x"));
  CHECK(laghu_srcset_next((laghu_buffer){srcset, sizeof(srcset) - 1U}, &cursor, &candidate));
  CHECK(laghu_base_ascii_equal(candidate.url, "b.webp"));
  return 0;
}
