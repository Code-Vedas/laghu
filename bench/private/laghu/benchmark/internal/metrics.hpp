// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace laghu::benchmark::internal {

struct Percentiles final {
  std::uint64_t p50{};
  std::uint64_t p95{};
  std::uint64_t p99{};
  std::uint64_t p999{};
};

[[nodiscard]] constexpr std::size_t nearest_rank_index(std::size_t sample_count,
                                                         std::uint64_t per_mille) noexcept {
  if (sample_count == 0U || per_mille == 0U || per_mille > 1000U) {
    return 0U;
  }
  const std::size_t numerator = sample_count * static_cast<std::size_t>(per_mille);
  return (numerator + 999U) / 1000U - 1U;
}

template <std::size_t Capacity>
[[nodiscard]] bool summarize_percentiles(std::array<std::uint64_t, Capacity>& samples,
                                         std::size_t count, Percentiles& output) noexcept {
  if (count == 0U || count > Capacity) {
    return false;
  }
  for (std::size_t left = 0U; left < count; ++left) {
    std::size_t minimum = left;
    for (std::size_t right = left + 1U; right < count; ++right) {
      if (samples[right] < samples[minimum]) {
        minimum = right;
      }
    }
    const std::uint64_t value = samples[left];
    samples[left] = samples[minimum];
    samples[minimum] = value;
  }
  output = Percentiles{
      samples[nearest_rank_index(count, 500U)],
      samples[nearest_rank_index(count, 950U)],
      samples[nearest_rank_index(count, 990U)],
      samples[nearest_rank_index(count, 999U)],
  };
  return true;
}

}  // namespace laghu::benchmark::internal
