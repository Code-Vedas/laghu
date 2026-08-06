// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/catalog.h"

#include <assert.h>
#include <string.h>

int main(void) {
  static const char hash[] =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  char first[LAGHU_RUNTIME_KEY_SIZE], second[LAGHU_RUNTIME_KEY_SIZE];
  assert(strstr(laghu_runtime_image_beacon_script(), "/.laghu/beacon/images") !=
         NULL);
  assert(laghu_catalog_key("/hero.png", hash, hash, 1U, first));
  assert(laghu_catalog_key("/hero.png", hash, hash, 1U, second));
  assert(strcmp(first, second) == 0);
  assert(!laghu_catalog_key("", hash, hash, 1U, second));
  return 0;
}
