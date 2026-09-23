// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <laghu/runtime/event_batch.hpp>

#include <laghu/benchmark/internal/workload.hpp>

const std::string_view laghu::benchmark::internal::workload_name{"event-batch"};
const std::uint64_t laghu::benchmark::internal::operations_per_interval = 4032U;

namespace {

template <std::size_t... Index>
[[nodiscard]] constexpr auto make_events(std::index_sequence<Index...>) noexcept {
  using laghu::runtime::Event;
  using laghu::runtime::EventNotification;
  using laghu::runtime::EventNotifications;
  using laghu::runtime::EventToken;
  return std::array<Event, sizeof...(Index)>{
      Event{*EventToken::from_uint64(Index + 1U),
            EventNotifications{EventNotification::readable}}...};
}

template <std::size_t BatchSize>
[[nodiscard]] laghu::core::Result<std::uint64_t> run_batches(
    std::uint64_t checksum) noexcept {
  constexpr std::size_t work_limit = BatchSize / 2U;
  constexpr std::size_t repetitions = 24U;
  auto events = make_events(std::make_index_sequence<BatchSize>{});
  const auto limits = laghu::runtime::EventBatchLimits::create(
      BatchSize, work_limit, std::chrono::seconds{1});
  if (!limits) {
    return std::unexpected{limits.error()};
  }
  auto batch = laghu::runtime::EventBatch::create(events, *limits);
  if (!batch) {
    return std::unexpected{batch.error()};
  }
  laghu::runtime::ReadyRotation rotation;
  for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
    const auto committed = batch->commit_backend_result(
        laghu::runtime::EventWaitResult{BatchSize, true});
    if (!committed) {
      return std::unexpected{committed.error()};
    }
    for (std::size_t index = 0; index < work_limit; ++index) {
      const auto event =
          batch->take_next(rotation, 1U, std::chrono::nanoseconds{0});
      if (!event) {
        return std::unexpected{event.error()};
      }
      checksum ^= event->token().value();
    }
    if (!batch->backend_saturated() ||
        batch->yield_reason(1U, std::chrono::nanoseconds{0}) !=
            laghu::runtime::EventBatchYieldReason::work_ceiling) {
      return std::unexpected{laghu::core::Error{
          laghu::core::ErrorDomain::core, laghu::core::ErrorCode::invalid_state,
          0, "event batch benchmark invariant failed"}};
    }
    batch->finish_rotation(rotation);
  }
  return checksum;
}

}  // namespace

laghu::core::Result<std::uint64_t> laghu::benchmark::internal::run_workload(
    std::uint64_t seed, WorkloadCounters& counters) noexcept {
  std::uint64_t checksum = seed;
  for (const auto run : {run_batches<16U>, run_batches<64U>, run_batches<256U>}) {
    const auto result = run(checksum);
    if (!result) {
      return std::unexpected{result.error()};
    }
    checksum = *result;
  }
  static_cast<void>(counters);
  return checksum;
}
