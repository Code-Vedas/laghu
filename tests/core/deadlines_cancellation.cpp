// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <thread>
#include <type_traits>

#include <laghu/core/deadlines_cancellation.hpp>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define LAGHU_TEST_THREAD_SANITIZER 1
#endif
#endif
#ifndef LAGHU_TEST_THREAD_SANITIZER
#define LAGHU_TEST_THREAD_SANITIZER 0
#endif

std::atomic<std::size_t> allocation_attempts{};

#if !LAGHU_TEST_THREAD_SANITIZER
void* operator new(std::size_t size) {
  allocation_attempts.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size); memory != nullptr) {
    return memory;
  }
  std::abort();
}

void* operator new[](std::size_t size) {
  allocation_attempts.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size); memory != nullptr) {
    return memory;
  }
  std::abort();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
#endif

namespace {

class DeterministicClock final {
 public:
  constexpr explicit DeterministicClock(laghu::core::MonotonicInstant now) noexcept : now_(now) {}

  [[nodiscard]] constexpr laghu::core::MonotonicInstant now() const noexcept { return now_; }
  constexpr void advance(laghu::core::MonotonicInstant elapsed) noexcept { now_ += elapsed; }

 private:
  laghu::core::MonotonicInstant now_;
};

[[nodiscard]] constexpr bool check(bool condition) noexcept { return condition; }

[[nodiscard]] bool check_deadline_boundaries() noexcept {
  DeterministicClock clock{100};
  const auto after = laghu::core::Deadline::after(clock, 20);
  const auto overflow = laghu::core::Deadline::after(
      DeterministicClock{std::numeric_limits<laghu::core::MonotonicInstant>::max()}, 1);
  if (!check(after.has_value() && after->instant() == 120 && !after->expired(clock) &&
             after->require_not_expired(clock).has_value() && !overflow.has_value() &&
             overflow.error().code() == laghu::core::ErrorCode::overflow)) {
    return false;
  }

  clock.advance(20);
  if (!check(after->expired(clock) && !after->require_not_expired(clock).has_value() &&
             after->require_not_expired(clock).error().code() == laghu::core::ErrorCode::deadline)) {
    return false;
  }

  const auto parent = laghu::core::Deadline::at(150);
  const auto earlier = laghu::core::Deadline::at(125);
  const auto later = laghu::core::Deadline::at(175);
  return check(laghu::core::Deadline::child(parent, earlier).instant() == 125 &&
               laghu::core::Deadline::child(parent, later).instant() == 150);
}

[[nodiscard]] bool check_terminal_idempotence() noexcept {
  laghu::core::CancellationState state;
  laghu::core::CancellationSource source{state};
  const auto token = source.token();
  const auto first = source.cancel();
  const auto repeated = source.cancel();
  const auto completion = source.complete();
  const auto outcome = token.outcome();
  return check(first.is_cancelled() && first.cause == laghu::core::CancellationCause::explicit_request &&
               repeated.is_cancelled() && completion.is_cancelled() && outcome.is_cancelled() &&
               outcome.cause == laghu::core::CancellationCause::explicit_request &&
               !token.require_active().has_value() &&
               token.require_active().error().code() == laghu::core::ErrorCode::cancellation);
}

[[nodiscard]] bool check_completion_wins() noexcept {
  laghu::core::CancellationState state;
  laghu::core::CancellationSource source{state};
  const auto completion = source.complete();
  const auto attempted_cancellation = source.cancel();
  const auto token = source.token();
  const auto active = token.require_active();
  if (!check(completion.terminal == laghu::core::CancellationTerminal::completed &&
             completion.cause == laghu::core::CancellationCause::none &&
             attempted_cancellation.terminal == laghu::core::CancellationTerminal::completed &&
             attempted_cancellation.cause == laghu::core::CancellationCause::none &&
             token.outcome().terminal == laghu::core::CancellationTerminal::completed &&
             !active.has_value() && active.error().code() == laghu::core::ErrorCode::invalid_state)) {
    return false;
  }

  DeterministicClock clock{10};
  const auto deadline = laghu::core::Deadline::at(10);
  const auto attempted_deadline_cancellation = source.cancel_if_expired(deadline, clock);
  return check(attempted_deadline_cancellation.terminal ==
                   laghu::core::CancellationTerminal::completed &&
               attempted_deadline_cancellation.cause == laghu::core::CancellationCause::none &&
               !token.require_active().has_value() &&
               token.require_active().error().code() == laghu::core::ErrorCode::invalid_state);
}

[[nodiscard]] bool check_deadline_cancellation() noexcept {
  laghu::core::CancellationState state;
  laghu::core::CancellationSource source{state};
  DeterministicClock clock{49};
  const auto deadline = laghu::core::Deadline::at(50);
  if (!check(source.cancel_if_expired(deadline, clock).terminal ==
                 laghu::core::CancellationTerminal::pending &&
             source.token().require_active(deadline, clock).has_value())) {
    return false;
  }
  clock.advance(1);
  const auto expired = source.cancel_if_expired(deadline, clock);
  return check(expired.is_cancelled() &&
               expired.cause == laghu::core::CancellationCause::deadline_expired &&
               !source.token().require_active().has_value() &&
               source.token().require_active().error().code() == laghu::core::ErrorCode::deadline);
}

[[nodiscard]] bool check_parent_observation_and_bounds() noexcept {
  laghu::core::CancellationState root_state;
  laghu::core::CancellationSource root_source{root_state};
  const auto root = root_source.token();
  std::array<laghu::core::CancellationState, laghu::core::CancellationToken::maximum_parent_depth>
      children{};
  laghu::core::CancellationToken current = root;
  for (laghu::core::CancellationState& child_state : children) {
    const auto child = current.child(child_state);
    if (!child.has_value()) {
      return false;
    }
    current = *child;
  }
  laghu::core::CancellationState too_deep_state;
  const auto too_deep = current.child(too_deep_state);
  const auto cycle = current.child(root_state);
  if (!check(!too_deep.has_value() && too_deep.error().code() == laghu::core::ErrorCode::invalid_range &&
             !cycle.has_value() && cycle.error().code() == laghu::core::ErrorCode::invalid_state &&
             current.outcome().terminal == laghu::core::CancellationTerminal::pending)) {
    return false;
  }

  const auto cancelled = root_source.cancel();
  const auto observed = current.outcome();
  return check(cancelled.is_cancelled() && observed.is_cancelled() &&
               observed.cause == laghu::core::CancellationCause::explicit_request &&
               !current.require_active().has_value());
}

[[nodiscard]] bool check_races() noexcept {
  for (int iteration = 0; iteration < 64; ++iteration) {
    laghu::core::CancellationState state;
    std::atomic<bool> start{false};
    std::thread completing{[&]() noexcept {
      while (!start.load(std::memory_order_acquire)) {
      }
      laghu::core::CancellationSource source{state};
      static_cast<void>(source.complete());
    }};
    std::thread cancelling{[&]() noexcept {
      while (!start.load(std::memory_order_acquire)) {
      }
      laghu::core::CancellationSource source{state};
      static_cast<void>(source.cancel());
    }};
    start.store(true, std::memory_order_release);
    completing.join();
    cancelling.join();
    const auto outcome = laghu::core::CancellationSource{state}.token().outcome();
    if (!outcome.is_terminal() ||
        (outcome.terminal == laghu::core::CancellationTerminal::completed &&
         outcome.cause != laghu::core::CancellationCause::none) ||
        (outcome.is_cancelled() && outcome.cause != laghu::core::CancellationCause::explicit_request)) {
      return false;
    }
  }

  for (int iteration = 0; iteration < 64; ++iteration) {
    laghu::core::CancellationState state;
    std::atomic<bool> start{false};
    std::thread explicit_cancel{[&]() noexcept {
      while (!start.load(std::memory_order_acquire)) {
      }
      laghu::core::CancellationSource source{state};
      static_cast<void>(source.cancel());
    }};
    std::thread deadline_cancel{[&]() noexcept {
      while (!start.load(std::memory_order_acquire)) {
      }
      laghu::core::CancellationSource source{state};
      const DeterministicClock clock{100};
      static_cast<void>(source.cancel_if_expired(laghu::core::Deadline::at(100), clock));
    }};
    start.store(true, std::memory_order_release);
    explicit_cancel.join();
    deadline_cancel.join();
    const auto outcome = laghu::core::CancellationSource{state}.token().outcome();
    if (!outcome.is_cancelled() ||
        (outcome.cause != laghu::core::CancellationCause::explicit_request &&
         outcome.cause != laghu::core::CancellationCause::deadline_expired)) {
      return false;
    }
  }
  return true;
}

static_assert(!std::is_copy_constructible_v<laghu::core::CancellationState>);
static_assert(!std::is_copy_constructible_v<laghu::core::CancellationSource>);
static_assert(std::is_copy_constructible_v<laghu::core::CancellationToken>);
static_assert(noexcept(std::declval<const laghu::core::CancellationToken&>().outcome()));

}  // namespace

int main() {
  const std::size_t allocation_attempts_before = allocation_attempts.load(std::memory_order_relaxed);
  if (!check(check_deadline_boundaries())) {
    return 1;
  }
  if (!check(check_terminal_idempotence())) {
    return 2;
  }
  if (!check(check_completion_wins())) {
    return 3;
  }
  if (!check(check_deadline_cancellation())) {
    return 4;
  }
  if (!check(check_parent_observation_and_bounds())) {
    return 5;
  }
  if (!check(allocation_attempts.load(std::memory_order_relaxed) == allocation_attempts_before)) {
    return 6;
  }
  if (!check(check_races())) {
    return 7;
  }
  return 0;
}
