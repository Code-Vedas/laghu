// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/budget.h"

#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>

#endif

uint64_t laghu_runtime_monotonic_ms(void) {
#ifdef _WIN32
  LARGE_INTEGER counter, frequency;
  if (!QueryPerformanceCounter(&counter) ||
      !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    return 0U;
  return (uint64_t)((counter.QuadPart * 1000U) / frequency.QuadPart);
#else
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0U;
  return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
#endif
}

void laghu_transform_budget_init(laghu_transform_budget *budget,
                                 size_t memory_limit, unsigned int deadline_ms,
                                 unsigned int variant_limit) {
  uint64_t now;
  if (budget == NULL) return;
  memset(budget, 0, sizeof(*budget));
  budget->memory_limit = memory_limit;
  budget->variant_limit = variant_limit;
  now = laghu_runtime_monotonic_ms();
  budget->deadline_ms =
      now > UINT64_MAX - deadline_ms ? UINT64_MAX : now + deadline_ms;
}

bool laghu_transform_budget_reserve(laghu_transform_budget *budget,
                                    size_t bytes) {
  if (budget == NULL || bytes > budget->memory_limit ||
      budget->current_memory > budget->memory_limit - bytes) {
    if (budget != NULL) budget->rejection = LAGHU_BUDGET_REJECTION_MEMORY;
    return false;
  }
  budget->current_memory += bytes;
  if (budget->current_memory > budget->peak_memory)
    budget->peak_memory = budget->current_memory;
  return true;
}

void laghu_transform_budget_release(laghu_transform_budget *budget,
                                    size_t bytes) {
  if (budget == NULL) return;
  budget->current_memory =
      bytes > budget->current_memory ? 0U : budget->current_memory - bytes;
}

bool laghu_transform_budget_checkpoint(laghu_transform_budget *budget,
                                       uint64_t work_units) {
  uint64_t now;
  if (budget == NULL || UINT64_MAX - budget->work_units < work_units) {
    if (budget != NULL) budget->rejection = LAGHU_BUDGET_REJECTION_CONTENT;
    return false;
  }
  budget->work_units += work_units;
  now = laghu_runtime_monotonic_ms();
  if (now == 0U || now > budget->deadline_ms) {
    budget->rejection = LAGHU_BUDGET_REJECTION_DEADLINE;
    return false;
  }
  return true;
}

bool laghu_transform_budget_generate(laghu_transform_budget *budget,
                                     size_t bytes) {
  if (budget == NULL || SIZE_MAX - budget->generated_bytes < bytes ||
      budget->generated_bytes + bytes > budget->memory_limit ||
      budget->current_memory >
          budget->memory_limit - (budget->generated_bytes + bytes)) {
    if (budget != NULL) budget->rejection = LAGHU_BUDGET_REJECTION_MEMORY;
    return false;
  }
  budget->generated_bytes += bytes;
  if (budget->current_memory + budget->generated_bytes > budget->peak_memory)
    budget->peak_memory = budget->current_memory + budget->generated_bytes;
  return true;
}

bool laghu_transform_budget_variant(laghu_transform_budget *budget) {
  if (budget == NULL || budget->variants >= budget->variant_limit) {
    if (budget != NULL) budget->rejection = LAGHU_BUDGET_REJECTION_VARIANTS;
    return false;
  }
  ++budget->variants;
  return true;
}
