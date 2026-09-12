// SPDX-License-Identifier: AGPL-3.0-only
#include <cstdint>

#include <laghu/core/checked_arithmetic.hpp>

#include <laghu/benchmark/internal/workload.hpp>

std::uint64_t laghu::benchmark::internal::run_core_foundation(
    std::uint64_t seed, WorkloadCounters& counters) noexcept {
  std::uint64_t state = seed;
  for (std::uint64_t iteration = 0U; iteration < core_foundation_operations_per_interval;
       ++iteration) {
    const auto incremented = laghu::core::checked_add(state, std::uint64_t{0x9e3779b9U});
    if (!incremented.has_value()) {
      return state;
    }
    state = *incremented ^ (state >> 13U);
  }
  // The foundation workload deliberately performs no allocation or Laghu OS
  // operation. The counters remain explicit, rather than inferred from host metrics.
  static_cast<void>(counters);
  return state;
}
