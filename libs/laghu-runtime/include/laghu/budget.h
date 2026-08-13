// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_BUDGET_H
#define LAGHU_BUDGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"
#include "laghu/image.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LAGHU_BUDGET_REJECTION_NONE = 0,
  LAGHU_BUDGET_REJECTION_CONTENT,
  LAGHU_BUDGET_REJECTION_MEMORY,
  LAGHU_BUDGET_REJECTION_DEADLINE,
  LAGHU_BUDGET_REJECTION_CACHE,
  LAGHU_BUDGET_REJECTION_VARIANTS,
  LAGHU_BUDGET_REJECTION_COUNT
} laghu_budget_rejection;

typedef struct {
  uint64_t deadline_ms;
  size_t memory_limit;
  size_t current_memory;
  size_t peak_memory;
  size_t generated_bytes;
  uint64_t work_units;
  unsigned int dependencies;
  unsigned int variants;
  unsigned int variant_limit;
  laghu_budget_rejection rejection;
} laghu_transform_budget;

uint64_t laghu_runtime_monotonic_ms(void);
void laghu_transform_budget_init(laghu_transform_budget *budget, size_t memory_limit, unsigned int deadline_ms, unsigned int variant_limit);
bool laghu_transform_budget_reserve(laghu_transform_budget *budget, size_t bytes);
void laghu_transform_budget_release(laghu_transform_budget *budget, size_t bytes);
bool laghu_transform_budget_checkpoint(laghu_transform_budget *budget, uint64_t work_units);
bool laghu_transform_budget_generate(laghu_transform_budget *budget, size_t bytes);
bool laghu_transform_budget_variant(laghu_transform_budget *budget);

#ifdef __cplusplus
}
#endif

#endif
