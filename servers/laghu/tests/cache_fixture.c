// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <string.h>

#include "laghu/runtime.h"

int main(int argc, char **argv) {
  static const unsigned char body[] = "immutable-fixture";
  laghu_runtime_cache_entry entry;
  if (argc != 3 || strlen(argv[2]) != LAGHU_RUNTIME_KEY_SIZE - 1U) return 2;
  return laghu_runtime_cache_publish(
             argv[1], argv[2], argv[2], argv[2], "image/png", "fixture",
             (laghu_buffer){body, sizeof(body) - 1U}, &entry)
             ? 0
             : 1;
}
