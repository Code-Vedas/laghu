// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

namespace laghu::benchmark::internal {

class WorkloadCounters final {
 public:
  [[nodiscard]] bool record_allocation_events(std::uint64_t count = 1U) noexcept {
    return record(allocation_count_, allocation_instrumented_, count);
  }

  [[nodiscard]] bool record_laghu_syscall_events(std::uint64_t count = 1U) noexcept {
    return record(laghu_syscall_count_, laghu_syscall_instrumented_, count);
  }

  [[nodiscard]] bool allocation_instrumented() const noexcept { return allocation_instrumented_; }
  [[nodiscard]] bool laghu_syscall_instrumented() const noexcept {
    return laghu_syscall_instrumented_;
  }
  [[nodiscard]] std::uint64_t allocation_count() const noexcept { return allocation_count_; }
  [[nodiscard]] std::uint64_t laghu_syscall_count() const noexcept { return laghu_syscall_count_; }

 private:
  [[nodiscard]] static bool record(std::uint64_t& total, bool& instrumented,
                                   std::uint64_t count) noexcept {
    if (count == 0U) {
      return true;
    }
    if (count > std::numeric_limits<std::uint64_t>::max() - total) {
      return false;
    }
    total += count;
    instrumented = true;
    return true;
  }

  std::uint64_t allocation_count_{};
  std::uint64_t laghu_syscall_count_{};
  bool allocation_instrumented_{};
  bool laghu_syscall_instrumented_{};
};

extern const std::string_view workload_name;
extern const std::uint64_t operations_per_interval;

[[nodiscard]] std::uint64_t run_workload(std::uint64_t seed,
                                         WorkloadCounters& counters) noexcept;

}  // namespace laghu::benchmark::internal
