// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/rum.h"

#include <assert.h>

int main(void) {
  char error[128];
  laghu_rum_options options;
  laghu_rum_options_init(&options);
  assert(laghu_rum_store_validate("local:", error, sizeof(error)));
  assert(
      !laghu_rum_store_validate("http://secret.example", error, sizeof(error)));
  return 0;
}
