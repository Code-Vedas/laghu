// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/base.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value)                                                              \
  do {                                                                            \
    if (!(value)) {                                                               \
      fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #value); \
      return 1;                                                                   \
    }                                                                             \
  } while (0)

int main(void) {
  unsigned char storage[8] = {0U};
  laghu_builder builder = {storage, 0U, sizeof(storage)};
  char copied[8];
  char digest[LAGHU_SHA256_HEX_SIZE];
  uint64_t parsed = 0U;
  char path[64];

  CHECK(laghu_base_ascii_equal((laghu_buffer){(const unsigned char *)"HeAd", 4U}, "head"));
  CHECK(laghu_base_string_copy(copied, sizeof(copied), "laghu"));
  CHECK(!laghu_base_string_copy(copied, 3U, "laghu") && copied[0] == '\0');
  CHECK(laghu_base_builder_append(&builder, "abc", 3U));
  CHECK(!laghu_base_builder_append(&builder, "123456", 6U));
  CHECK(builder.length == 3U && memcmp(builder.data, "abc", 3U) == 0);
  CHECK(laghu_base_parse_u64("42", 1U, 100U, &parsed) && parsed == 42U);
  CHECK(!laghu_base_parse_u64("101", 1U, 100U, &parsed));
  CHECK(laghu_base_url_resolve_same_origin("/pages/index.html", "https://example.test", (laghu_buffer){(const unsigned char *)"../hero.webp", 12U},
                                           0U, path, sizeof(path)) &&
        strcmp(path, "/pages/../hero.webp") == 0);
  CHECK(!laghu_base_url_resolve_same_origin("/pages/index.html", "https://example.test", (laghu_buffer){(const unsigned char *)"../hero.webp", 12U},
                                            LAGHU_URL_REJECT_TRAVERSAL, path, sizeof(path)));
  CHECK(laghu_base_hex_value('f') == 15 && laghu_base_hex_value('G') < 0);
  CHECK(laghu_sha256_hex((laghu_buffer){(const unsigned char *)"abc", 3U}, digest));
  CHECK(strcmp(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
  return 0;
}
