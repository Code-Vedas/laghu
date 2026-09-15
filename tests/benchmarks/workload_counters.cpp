// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstdint>
#include <limits>

#include <laghu/benchmark/internal/workload.hpp>

#include "laghu_test_support.hpp"

namespace {

[[nodiscard]] bool run_instrumented_fixture(
    laghu::benchmark::internal::WorkloadCounters& counters) noexcept {
  return counters.record_allocation_events(2U) && counters.record_laghu_syscall_events(3U);
}

[[nodiscard]] bool run_fixture(std::uint64_t warmup_intervals, std::uint64_t measured_intervals,
                               laghu::benchmark::internal::WorkloadCounters& reported) noexcept {
  laghu::benchmark::internal::WorkloadCounters warmup{};
  for (std::uint64_t interval = 0U; interval < warmup_intervals; ++interval) {
    if (!run_instrumented_fixture(warmup)) {
      return false;
    }
  }
  for (std::uint64_t interval = 0U; interval < measured_intervals; ++interval) {
    if (!run_instrumented_fixture(reported)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool check_exact_instrumentation_accounting() noexcept {
  laghu::benchmark::internal::WorkloadCounters counters{};
  return !counters.allocation_instrumented() && !counters.laghu_syscall_instrumented() &&
      counters.allocation_count() == 0U && counters.laghu_syscall_count() == 0U &&
      counters.record_allocation_events(0U) && counters.record_laghu_syscall_events(0U) &&
      !counters.allocation_instrumented() && !counters.laghu_syscall_instrumented() &&
      run_instrumented_fixture(counters) && counters.allocation_instrumented() &&
      counters.laghu_syscall_instrumented() && counters.allocation_count() == 2U &&
      counters.laghu_syscall_count() == 3U;
}

[[nodiscard]] bool check_warmup_counters_are_not_reported() noexcept {
  laghu::benchmark::internal::WorkloadCounters no_warmup{};
  laghu::benchmark::internal::WorkloadCounters extended_warmup{};
  return run_fixture(0U, 4U, no_warmup) && run_fixture(7U, 4U, extended_warmup) &&
      no_warmup.allocation_instrumented() && no_warmup.laghu_syscall_instrumented() &&
      extended_warmup.allocation_instrumented() && extended_warmup.laghu_syscall_instrumented() &&
      no_warmup.allocation_count() == 8U && no_warmup.laghu_syscall_count() == 12U &&
      extended_warmup.allocation_count() == 8U && extended_warmup.laghu_syscall_count() == 12U;
}

[[nodiscard]] bool check_counter_overflow_is_rejected() noexcept {
  laghu::benchmark::internal::WorkloadCounters counters{};
  constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
  return counters.record_allocation_events(maximum) && !counters.record_allocation_events() &&
      counters.allocation_instrumented() && counters.allocation_count() == maximum &&
      counters.record_laghu_syscall_events(maximum - 1U) &&
      !counters.record_laghu_syscall_events(2U) && counters.laghu_syscall_instrumented() &&
      counters.laghu_syscall_count() == maximum - 1U;
}

}  // namespace

int main() {
  static constexpr std::array tests{
      laghu::test::TestCase{"benchmark.workload-counters.exact", check_exact_instrumentation_accounting},
      laghu::test::TestCase{"benchmark.workload-counters.warmup", check_warmup_counters_are_not_reported},
      laghu::test::TestCase{"benchmark.workload-counters.overflow", check_counter_overflow_is_rejected},
  };
  return laghu::test::run_tests(tests);
}
