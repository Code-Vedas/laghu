// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstdint>

#include <laghu/benchmark/internal/metrics.hpp>

#include "laghu_test_support.hpp"

namespace {

[[nodiscard]] bool check_nearest_rank_percentiles() noexcept {
  std::array<std::uint64_t, 10> samples{10U, 2U, 8U, 4U, 6U, 1U, 9U, 3U, 7U, 5U};
  laghu::benchmark::internal::Percentiles summary{};
  return laghu::benchmark::internal::summarize_percentiles(samples, samples.size(), summary) &&
      summary.p50 == 5U && summary.p95 == 10U && summary.p99 == 10U && summary.p999 == 10U;
}

[[nodiscard]] bool check_single_sample_percentiles() noexcept {
  std::array<std::uint64_t, 2> samples{42U, 0U};
  laghu::benchmark::internal::Percentiles summary{};
  return laghu::benchmark::internal::summarize_percentiles(samples, 1U, summary) &&
      summary.p50 == 42U && summary.p95 == 42U && summary.p99 == 42U && summary.p999 == 42U;
}

[[nodiscard]] bool check_invalid_samples_are_rejected() noexcept {
  std::array<std::uint64_t, 1> samples{1U};
  laghu::benchmark::internal::Percentiles summary{};
  return !laghu::benchmark::internal::summarize_percentiles(samples, 0U, summary) &&
      !laghu::benchmark::internal::summarize_percentiles(samples, 2U, summary);
}

}  // namespace

int main() {
  static constexpr std::array tests{
      laghu::test::TestCase{"benchmark.nearest-rank", check_nearest_rank_percentiles},
      laghu::test::TestCase{"benchmark.single-sample", check_single_sample_percentiles},
      laghu::test::TestCase{"benchmark.invalid-samples", check_invalid_samples_are_rejected},
  };
  return laghu::test::run_tests(tests);
}
