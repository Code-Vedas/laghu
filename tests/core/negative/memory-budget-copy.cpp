// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/memory_budget.hpp>

int main() {
  laghu::core::MemoryBudget original{*laghu::core::WorkerId::from_uint64(1), 1};
  laghu::core::MemoryBudget copy{original};
  return copy.limit() == 1 ? 0 : 1;
}
