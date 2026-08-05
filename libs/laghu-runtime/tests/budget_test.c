// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/budget.h"

#include <assert.h>

int main(void) {
  laghu_transform_budget budget;
  laghu_transform_budget_init(&budget, 4096U, 1000U, 2U);
  assert(laghu_transform_budget_reserve(&budget, 1024U));
  assert(laghu_transform_budget_reserve(&budget, 2048U));
  assert(budget.current_memory == 3072U && budget.peak_memory == 3072U);
  laghu_transform_budget_release(&budget, 2048U);
  assert(budget.current_memory == 1024U);
  assert(!laghu_transform_budget_reserve(&budget, 4096U));
  assert(budget.rejection == LAGHU_BUDGET_REJECTION_MEMORY);
  budget.rejection = LAGHU_BUDGET_REJECTION_NONE;
  assert(laghu_transform_budget_checkpoint(&budget, 128U));
  assert(laghu_transform_budget_generate(&budget, 3072U));
  assert(!laghu_transform_budget_generate(&budget, 1U));
  assert(laghu_transform_budget_variant(&budget));
  assert(laghu_transform_budget_variant(&budget));
  assert(!laghu_transform_budget_variant(&budget));
  assert(budget.rejection == LAGHU_BUDGET_REJECTION_VARIANTS);
  return 0;
}
