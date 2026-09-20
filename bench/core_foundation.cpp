// SPDX-License-Identifier: AGPL-3.0-only
#include <cstdint>

#include <laghu/core/checked_arithmetic.hpp>

#include <laghu/benchmark/internal/workload.hpp>

const std::string_view laghu::benchmark::internal::workload_name{"core-foundation"};
const std::uint64_t laghu::benchmark::internal::operations_per_interval = 4096U;

std::uint64_t laghu::benchmark::internal::run_workload(
    std::uint64_t seed, WorkloadCounters& counters) noexcept {
  std::uint64_t state = seed;
  for (std::uint64_t iteration = 0U; iteration < operations_per_interval;
       ++iteration) {
    const auto incremented = laghu::core::checked_add(state, std::uint64_t{0x9e3779b9U});
    if (!incremented.has_value()) {
      return state;
    }
    state = *incremented ^ (state >> 13U);
  }
  // The foundation workload deliberately performs no allocation or Laghu OS
  // operation. It records no events, so the caller reports these metrics as
  // uninstrumented rather than inferring them from host metrics.
  static_cast<void>(counters);
  return state;
}
