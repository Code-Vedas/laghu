// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <chrono>
#include <cstddef>

#include "laghu_test_support.hpp"

#include <laghu/runtime/event_batch.hpp>

namespace {

using laghu::core::ErrorCode;
using laghu::runtime::Event;
using laghu::runtime::EventBatch;
using laghu::runtime::EventBatchLimits;
using laghu::runtime::EventBatchYieldReason;
using laghu::runtime::EventNotification;
using laghu::runtime::EventNotifications;
using laghu::runtime::EventToken;
using laghu::runtime::EventWaitResult;
using laghu::runtime::ReadyRotation;

[[nodiscard]] bool check_limits_and_saturation() noexcept {
  const auto token = EventToken::from_uint64(1);
  const auto limits = EventBatchLimits::create(2, 5, std::chrono::milliseconds{2});
  if (!token || !limits) {
    return false;
  }
  std::array<Event, 2> storage{
      Event{*token, EventNotifications{EventNotification::readable}},
      Event{*token, EventNotifications{EventNotification::writable}},
  };
  auto batch = EventBatch::create(storage, *limits);
  if (!batch || !batch->commit_backend_result(EventWaitResult{2, true}) ||
      batch->event_count() != 2 || !batch->backend_saturated()) {
    return false;
  }

  ReadyRotation rotation;
  const auto first = batch->take_next(rotation, 3, std::chrono::nanoseconds{0});
  if (!first || batch->processed_events() != 1 || batch->consumed_work_units() != 3 ||
      batch->yield_reason(3, std::chrono::nanoseconds{1}) !=
          EventBatchYieldReason::work_ceiling) {
    return false;
  }
  const auto exhausted = batch->take_next(rotation, 3, std::chrono::nanoseconds{1});
  return !exhausted && exhausted.error().code() == ErrorCode::exhaustion &&
         batch->yield_reason(1, std::chrono::milliseconds{2}) ==
             EventBatchYieldReason::time_ceiling;
}

[[nodiscard]] bool check_ready_rotation_fairness() noexcept {
  const auto token1 = EventToken::from_uint64(1);
  const auto token2 = EventToken::from_uint64(2);
  const auto token3 = EventToken::from_uint64(3);
  const auto token4 = EventToken::from_uint64(4);
  const auto limits = EventBatchLimits::create(4, 2, std::chrono::seconds{1});
  if (!token1 || !token2 || !token3 || !token4 || !limits) {
    return false;
  }
  std::array<Event, 4> storage{
      Event{*token1, EventNotifications{EventNotification::readable}},
      Event{*token2, EventNotifications{EventNotification::readable}},
      Event{*token3, EventNotifications{EventNotification::readable}},
      Event{*token4, EventNotifications{EventNotification::readable}},
  };
  auto batch = EventBatch::create(storage, *limits);
  ReadyRotation rotation;
  if (!batch || !batch->commit_backend_result(EventWaitResult{4, false})) {
    return false;
  }
  const auto first = batch->take_next(rotation, 1, std::chrono::nanoseconds{0});
  const auto second = batch->take_next(rotation, 1, std::chrono::nanoseconds{0});
  batch->finish_rotation(rotation);
  if (!first || !second || first->token().value() != 1 || second->token().value() != 2) {
    return false;
  }

  storage = std::array<Event, 4>{
      Event{*token4, EventNotifications{EventNotification::readable}},
      Event{*token3, EventNotifications{EventNotification::readable}},
      Event{*token2, EventNotifications{EventNotification::readable}},
      Event{*token1, EventNotifications{EventNotification::readable}},
  };
  if (!batch->commit_backend_result(EventWaitResult{4, false})) {
    return false;
  }
  const auto third = batch->take_next(rotation, 1, std::chrono::nanoseconds{0});
  const auto fourth = batch->take_next(rotation, 1, std::chrono::nanoseconds{0});
  return third && fourth && third->token().value() == 3 && fourth->token().value() == 4;
}

[[nodiscard]] bool check_ready_rotation_fallbacks() noexcept {
  const auto token1 = EventToken::from_uint64(1);
  const auto token2 = EventToken::from_uint64(2);
  const auto token3 = EventToken::from_uint64(3);
  const auto token4 = EventToken::from_uint64(4);
  const auto limits = EventBatchLimits::create(3, 1, std::chrono::seconds{1});
  if (!token1 || !token2 || !token3 || !token4 || !limits) {
    return false;
  }
  std::array<Event, 3> storage{
      Event{*token1, EventNotifications{EventNotification::readable}},
      Event{*token2, EventNotifications{EventNotification::readable}},
      Event{*token3, EventNotifications{EventNotification::readable}},
  };
  auto batch = EventBatch::create(storage, *limits);
  ReadyRotation rotation;
  if (!batch || !batch->commit_backend_result(EventWaitResult{3, false}) ||
      !batch->take_next(rotation, 1, std::chrono::nanoseconds{0})) {
    return false;
  }
  batch->finish_rotation(rotation);

  if (!batch->commit_backend_result(EventWaitResult{0, false})) {
    return false;
  }
  batch->finish_rotation(rotation);
  storage = std::array<Event, 3>{
      Event{*token4, EventNotifications{EventNotification::readable}},
      Event{*token2, EventNotifications{EventNotification::readable}},
      Event{*token1, EventNotifications{EventNotification::readable}},
  };
  if (!batch->commit_backend_result(EventWaitResult{3, false})) {
    return false;
  }
  const auto preserved = batch->take_next(rotation, 1, std::chrono::nanoseconds{0});
  if (!preserved || preserved->token().value() != 2) {
    return false;
  }
  batch->finish_rotation(rotation);

  storage = std::array<Event, 3>{
      Event{*token3, EventNotifications{EventNotification::readable}},
      Event{*token2, EventNotifications{EventNotification::readable}},
      Event{*token1, EventNotifications{EventNotification::readable}},
  };
  if (!batch->commit_backend_result(EventWaitResult{3, false})) {
    return false;
  }
  const auto fallback = batch->take_next(rotation, 1, std::chrono::nanoseconds{0});
  return fallback && fallback->token().value() == 1;
}

[[nodiscard]] bool check_invalid_limits_and_capacity() noexcept {
  const auto token = EventToken::from_uint64(1);
  if (!token) {
    return false;
  }
  std::array<Event, 1> storage{
      Event{*token, EventNotifications{EventNotification::readable}},
  };
  const auto limits = EventBatchLimits::create(2, 1, std::chrono::nanoseconds{1});
  if (!limits) {
    return false;
  }
  const auto undersized = EventBatch::create(storage, *limits);
  return !EventBatchLimits::create(0, 1, std::chrono::nanoseconds{1}) &&
         !EventBatchLimits::create(1, 0, std::chrono::nanoseconds{1}) &&
         !EventBatchLimits::create(1, 1, std::chrono::nanoseconds{0}) &&
         !undersized && undersized.error().code() == ErrorCode::exhaustion;
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"runtime.event_batch.limits", check_limits_and_saturation},
      laghu::test::TestCase{"runtime.event_batch.rotation", check_ready_rotation_fairness},
      laghu::test::TestCase{"runtime.event_batch.rotation_fallbacks",
                            check_ready_rotation_fallbacks},
      laghu::test::TestCase{"runtime.event_batch.invalid", check_invalid_limits_and_capacity},
  };
  return laghu::test::run_tests(tests);
}
