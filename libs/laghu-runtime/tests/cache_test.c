// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/cache.h"

#include <assert.h>

int main(void) {
  uint64_t bytes = 0U;
  unsigned int seconds = 0U;
  assert(laghu_cache_size_parse("32m", 4U, UINT64_MAX, &bytes));
  assert(bytes == 32U * 1024U * 1024U);
  assert(!laghu_cache_size_parse("32mbogus", 4U, UINT64_MAX, &bytes));
  assert(laghu_cache_duration_parse("45s", 1U, 60U, &seconds));
  assert(seconds == 45U);
  return 0;
}
