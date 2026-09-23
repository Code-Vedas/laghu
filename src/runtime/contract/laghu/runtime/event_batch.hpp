// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <chrono>
#include <cstddef>
#include <span>

#include <laghu/core/contract.hpp>
#include <laghu/runtime/event_backend.hpp>

namespace laghu::runtime {

class EventBatchLimits final {
 public:
  [[nodiscard]] static constexpr core::Result<EventBatchLimits> create(
      std::size_t maximum_events, std::size_t maximum_work_units,
      std::chrono::nanoseconds maximum_duration) noexcept {
    if (maximum_events == 0 || maximum_work_units == 0 ||
        maximum_duration.count() <= 0) {
      return std::unexpected{core::Error{core::ErrorDomain::core,
                                         core::ErrorCode::invalid_range, 0,
                                         "event batch limits must be positive"}};
    }
    return EventBatchLimits{maximum_events, maximum_work_units,
                            maximum_duration};
  }

  [[nodiscard]] constexpr std::size_t maximum_events() const noexcept {
    return maximum_events_;
  }
  [[nodiscard]] constexpr std::size_t maximum_work_units() const noexcept {
    return maximum_work_units_;
  }
  [[nodiscard]] constexpr std::chrono::nanoseconds maximum_duration() const noexcept {
    return maximum_duration_;
  }

 private:
  constexpr EventBatchLimits(std::size_t maximum_events,
                             std::size_t maximum_work_units,
                             std::chrono::nanoseconds maximum_duration) noexcept
      : maximum_events_(maximum_events),
        maximum_work_units_(maximum_work_units),
        maximum_duration_(maximum_duration) {}

  std::size_t maximum_events_;
  std::size_t maximum_work_units_;
  std::chrono::nanoseconds maximum_duration_;
};

enum class EventBatchYieldReason : std::uint8_t {
  none,
  events_exhausted,
  work_ceiling,
  time_ceiling,
};

class ReadyRotation final {
 public:
  [[nodiscard]] constexpr std::size_t index_for(
      std::size_t ordinal, std::size_t ready_count) const noexcept {
    if (ready_count == 0) {
      return 0;
    }
    const std::size_t normalized_start = next_start_ % ready_count;
    const std::size_t normalized_ordinal = ordinal % ready_count;
    const std::size_t remaining = ready_count - normalized_start;
    return normalized_ordinal < remaining ? normalized_start + normalized_ordinal
                                          : normalized_ordinal - remaining;
  }

  constexpr void advance(std::size_t processed, std::size_t ready_count) noexcept {
    if (ready_count == 0) {
      next_start_ = 0;
      return;
    }
    const std::size_t normalized_start = next_start_ % ready_count;
    const std::size_t normalized_processed = processed % ready_count;
    next_start_ = normalized_processed < ready_count - normalized_start
                      ? normalized_start + normalized_processed
                      : normalized_processed - (ready_count - normalized_start);
  }

 private:
  std::size_t next_start_{0};
};

class EventBatch final {
 public:
  EventBatch(const EventBatch&) = delete;
  EventBatch& operator=(const EventBatch&) = delete;
  EventBatch(EventBatch&&) noexcept = default;
  EventBatch& operator=(EventBatch&&) noexcept = default;
  ~EventBatch() = default;

  [[nodiscard]] static constexpr core::Result<EventBatch> create(
      std::span<Event> storage, EventBatchLimits limits) noexcept {
    if (storage.size() < limits.maximum_events()) {
      return std::unexpected{core::Error{core::ErrorDomain::core,
                                         core::ErrorCode::exhaustion, 0,
                                         "event batch storage is smaller than its limit"}};
    }
    return EventBatch{storage.first(limits.maximum_events()), limits};
  }

  [[nodiscard]] constexpr std::span<Event> backend_output() noexcept {
    return storage_;
  }

  [[nodiscard]] constexpr core::Result<void> commit_backend_result(
      EventWaitResult result) noexcept {
    if (result.event_count > storage_.size()) {
      return std::unexpected{core::Error{core::ErrorDomain::core,
                                         core::ErrorCode::overflow, 0,
                                         "event backend exceeded batch capacity"}};
    }
    event_count_ = result.event_count;
    processed_events_ = 0;
    consumed_work_units_ = 0;
    backend_saturated_ = result.saturated;
    return {};
  }

  [[nodiscard]] constexpr EventBatchYieldReason yield_reason(
      std::size_t next_work_units,
      std::chrono::nanoseconds elapsed) const noexcept {
    if (processed_events_ >= event_count_) {
      return EventBatchYieldReason::events_exhausted;
    }
    if (elapsed >= limits_.maximum_duration()) {
      return EventBatchYieldReason::time_ceiling;
    }
    if (next_work_units >
        limits_.maximum_work_units() - consumed_work_units_) {
      return EventBatchYieldReason::work_ceiling;
    }
    return EventBatchYieldReason::none;
  }

  [[nodiscard]] constexpr core::Result<Event> take_next(
      ReadyRotation& rotation, std::size_t work_units,
      std::chrono::nanoseconds elapsed) noexcept {
    const EventBatchYieldReason reason = yield_reason(work_units, elapsed);
    if (reason != EventBatchYieldReason::none) {
      return std::unexpected{core::Error{core::ErrorDomain::core,
                                         core::ErrorCode::exhaustion, 0,
                                         "event batch processing ceiling reached"}};
    }
    const std::size_t index = rotation.index_for(processed_events_, event_count_);
    ++processed_events_;
    consumed_work_units_ += work_units;
    return storage_[index];
  }

  constexpr void finish_rotation(ReadyRotation& rotation) const noexcept {
    rotation.advance(processed_events_, event_count_);
  }

  [[nodiscard]] constexpr std::size_t event_count() const noexcept {
    return event_count_;
  }
  [[nodiscard]] constexpr std::size_t processed_events() const noexcept {
    return processed_events_;
  }
  [[nodiscard]] constexpr std::size_t consumed_work_units() const noexcept {
    return consumed_work_units_;
  }
  [[nodiscard]] constexpr bool backend_saturated() const noexcept {
    return backend_saturated_;
  }

 private:
  constexpr EventBatch(std::span<Event> storage, EventBatchLimits limits) noexcept
      : storage_(storage), limits_(limits) {}

  std::span<Event> storage_;
  EventBatchLimits limits_;
  std::size_t event_count_{0};
  std::size_t processed_events_{0};
  std::size_t consumed_work_units_{0};
  bool backend_saturated_{false};
};

}  // namespace laghu::runtime
