// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>

namespace laghu::benchmark::internal {

struct WorkloadCounters final {
  std::uint64_t allocation_count{};
  std::uint64_t laghu_syscall_count{};
};

inline constexpr std::uint64_t core_foundation_operations_per_interval = 4096U;

[[nodiscard]] std::uint64_t run_core_foundation(std::uint64_t seed,
                                                WorkloadCounters& counters) noexcept;

}  // namespace laghu::benchmark::internal
