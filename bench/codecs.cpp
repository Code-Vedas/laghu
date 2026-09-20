// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <laghu/adapters/codecs.hpp>
#include <laghu/benchmark/internal/workload.hpp>
#include <laghu/core/bounded_arena.hpp>
#include <laghu/core/memory_budget.hpp>

namespace laghu::benchmark::internal {
namespace {

constexpr std::size_t arena_capacity = 8U * 1024U * 1024U;
alignas(std::max_align_t) std::array<std::byte, arena_capacity> arena_storage{};
constexpr std::array<std::byte, 4096> input{};

struct Source final {
  WorkloadCounters* counters{};
};

[[nodiscard]] core::Result<core::MutableByteView> acquire(
    void* context, std::size_t minimum) noexcept {
  auto& source = *static_cast<Source*>(context);
  if (minimum > arena_storage.size()) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::exhaustion, 0,
                                       "codec benchmark arena exhausted"}};
  }
  if (!source.counters->record_allocation_events()) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::overflow, 0,
                                       "codec benchmark allocation counter overflow"}};
  }
  return core::MutableByteView::from(arena_storage);
}

[[nodiscard]] core::Result<void> reset(void*) noexcept { return {}; }

}  // namespace

const std::string_view workload_name{LAGHU_CODEC_BENCHMARK_NAME};
const std::uint64_t operations_per_interval = 256U;

std::uint64_t run_workload(std::uint64_t seed, WorkloadCounters& counters) noexcept {
  constexpr core::WorkerId worker = *core::WorkerId::from_uint64(1U);
  Source source{&counters};
  core::MemoryBudget budget{worker, arena_capacity};
  core::BoundedArena arena{worker, budget,
      core::ArenaBlockSource{&source, acquire, reset}, 4096U, arena_capacity};
  adapters::NativeMemoryPool memory{worker, arena};
  std::uint64_t checksum = seed;
  for (std::uint64_t iteration = 0U; iteration < operations_per_interval; ++iteration) {
    std::array<std::max_align_t, adapters::codec_state_storage_words> state{};
    std::array<std::byte, 8192> output{};
    auto stream = LAGHU_CODEC_FACTORY(
        adapters::CodecDirection::encode, state, memory,
        adapters::CodecLimits{input.size(), output.size(), 8U});
    if (!stream.has_value()) return checksum;
    const auto progress = stream->process(
        *core::ByteView::from(input), *core::MutableByteView::from(output));
    if (!progress.has_value()) return checksum;
    std::size_t produced = progress->output_produced;
    bool finished = progress->finished;
    while (!finished && produced < output.size()) {
      const auto remaining = *core::MutableByteView::from(
          std::span<std::byte>{output}.subspan(produced));
      const auto completion = stream->finish(remaining);
      if (!completion.has_value()) return checksum;
      produced += completion->output_produced;
      finished = completion->finished;
      if (!finished && completion->output_produced == 0U) return checksum;
    }
    checksum ^= static_cast<std::uint64_t>(produced) + iteration;
  }
  const auto boundary = arena.quiescent_boundary(worker);
  if (!boundary.has_value() || !arena.reset(worker, *boundary).has_value()) return checksum;
  static_cast<void>(counters);
  return checksum;
}

}  // namespace laghu::benchmark::internal
