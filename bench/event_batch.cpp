// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>

#include <laghu/runtime/event_batch.hpp>

#include <laghu/benchmark/internal/workload.hpp>

const std::string_view laghu::benchmark::internal::workload_name{"event-batch"};
const std::uint64_t laghu::benchmark::internal::operations_per_interval = 4096U;

laghu::core::Result<std::uint64_t> laghu::benchmark::internal::run_workload(
    std::uint64_t seed, WorkloadCounters& counters) noexcept {
  constexpr std::array<std::size_t, 3> batch_sizes{16, 64, 256};
  laghu::runtime::ReadyRotation rotation;
  std::uint64_t checksum = seed;
  for (std::uint64_t iteration = 0; iteration < operations_per_interval;
       ++iteration) {
    const std::size_t batch_size = batch_sizes[iteration % batch_sizes.size()];
    const std::size_t ordinal = static_cast<std::size_t>(iteration) % batch_size;
    checksum ^= static_cast<std::uint64_t>(rotation.index_for(ordinal, batch_size));
    rotation.advance(ordinal + 1, batch_size);
  }
  static_cast<void>(counters);
  return checksum;
}
