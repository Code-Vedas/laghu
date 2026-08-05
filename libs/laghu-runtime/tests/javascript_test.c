// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/javascript.h"

#include <assert.h>
#include <string.h>

int main(void) {
  char target[LAGHU_JAVASCRIPT_TARGET_SIZE];
  assert(laghu_javascript_target_normalize("es2022", target));
  assert(strcmp(target, "es2022") == 0);
  assert(!laghu_javascript_target_normalize("extends unsafe", target));
  return 0;
}
