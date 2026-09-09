// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <type_traits>

#include <laghu/core/state_transitions.hpp>

std::size_t allocation_attempts{};

void* operator new(std::size_t size) {
  ++allocation_attempts;
  if (void* memory = std::malloc(size); memory != nullptr) {
    return memory;
  }
  std::abort();
}

void* operator new[](std::size_t size) {
  ++allocation_attempts;
  if (void* memory = std::malloc(size); memory != nullptr) {
    return memory;
  }
  std::abort();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

enum class State : std::uint8_t {
  idle,
  active,
  closed,
};

enum class Reason : std::uint8_t {
  start,
  finish,
  retry,
};

using laghu::core::ErrorCode;
using laghu::core::StateMachine;
using laghu::core::TransitionRecord;
using laghu::core::TransitionRule;
using laghu::core::TransitionTable;
using laghu::core::TransitionTrace;

constexpr std::array rules{
    TransitionRule<State, Reason>{State::idle, State::active, Reason::start},
    TransitionRule<State, Reason>{State::active, State::active, Reason::retry},
    TransitionRule<State, Reason>{State::active, State::closed, Reason::finish},
};
constexpr auto table_result = TransitionTable<State, Reason>::create(rules);
static_assert(table_result.has_value());
constexpr TransitionTable<State, Reason> transitions = *table_result;
static_assert(transitions.size() == 3);
static_assert(transitions.permits(State::idle, State::active, Reason::start));
static_assert(transitions.permits(State::active, State::active, Reason::retry));
static_assert(!transitions.permits(State::idle, State::closed, Reason::finish));

constexpr std::array duplicate_rules{
    TransitionRule<State, Reason>{State::idle, State::active, Reason::start},
    TransitionRule<State, Reason>{State::idle, State::active, Reason::start},
};
constexpr std::array shared_target_rules{
    TransitionRule<State, Reason>{State::idle, State::active, Reason::start},
    TransitionRule<State, Reason>{State::idle, State::active, Reason::retry},
};
constexpr std::array shared_reason_rules{
    TransitionRule<State, Reason>{State::idle, State::active, Reason::start},
    TransitionRule<State, Reason>{State::idle, State::closed, Reason::start},
};
constexpr std::array empty_rules = std::array<TransitionRule<State, Reason>, 0>{};
static_assert(!TransitionTable<State, Reason>::create(duplicate_rules).has_value());
static_assert(TransitionTable<State, Reason>::create(shared_target_rules).has_value());
static_assert(TransitionTable<State, Reason>::create(shared_reason_rules).has_value());
static_assert(!TransitionTable<State, Reason>::create(empty_rules).has_value());
static_assert(std::is_empty_v<TransitionTrace<State, Reason, false>>);
static_assert(sizeof(StateMachine<State, Reason, true>) > sizeof(StateMachine<State, Reason, false>));
static_assert(noexcept(StateMachine<State, Reason, false>{State::idle, transitions}.transition(
    State::active, Reason::start)));

[[nodiscard]] constexpr bool check(bool condition) noexcept { return condition; }

[[nodiscard]] bool check_disabled_trace_and_invalid_states() noexcept {
  StateMachine<State, Reason> machine{State::idle, transitions};
  const auto valid = machine.transition(State::active, Reason::start);
  const auto self = machine.transition(State::active, Reason::retry);
  const auto invalid = machine.transition(State::closed, Reason::start);
  const auto unknown_next = machine.transition(static_cast<State>(255), Reason::finish);
  if (!check(valid.has_value() && self.has_value() && !invalid.has_value() &&
             !unknown_next.has_value() && machine.current() == State::active &&
             invalid.error().code() == ErrorCode::invalid_state &&
             unknown_next.error().code() == ErrorCode::invalid_state)) {
    return false;
  }

  StateMachine<State, Reason> unknown_current{static_cast<State>(254), transitions};
  const auto unknown_current_result = unknown_current.transition(State::active, Reason::start);
  return check(!unknown_current_result.has_value() &&
               unknown_current_result.error().code() == ErrorCode::invalid_state &&
               unknown_current.current() == static_cast<State>(254));
}

[[nodiscard]] bool check_enabled_trace_records() noexcept {
  std::array<TransitionRecord<State, Reason>, 3> storage{};
  TransitionTrace<State, Reason, true> trace{storage};
  StateMachine<State, Reason, true> machine{State::idle, transitions, trace};
  const auto first = machine.transition(State::active, Reason::start);
  const auto second = machine.transition(State::active, Reason::retry);
  const auto third = machine.transition(State::closed, Reason::finish);
  const auto records = trace.records();
  return check(first.has_value() && second.has_value() && third.has_value() &&
               machine.current() == State::closed && trace.size() == 3 && trace.capacity() == 3 &&
               trace.sequence() == 3 && records[0].sequence == 1 &&
               records[0].current == State::idle && records[0].next == State::active &&
               records[0].reason == Reason::start && records[1].sequence == 2 &&
               records[1].current == State::active && records[1].next == State::active &&
               records[1].reason == Reason::retry && records[2].sequence == 3 &&
               records[2].current == State::active && records[2].next == State::closed &&
               records[2].reason == Reason::finish);
}

[[nodiscard]] bool check_distinct_full_tuple_rules() noexcept {
  const auto shared_target = TransitionTable<State, Reason>::create(shared_target_rules);
  const auto shared_reason = TransitionTable<State, Reason>::create(shared_reason_rules);
  if (!check(shared_target.has_value() && shared_reason.has_value())) {
    return false;
  }

  StateMachine<State, Reason> target_machine{State::idle, *shared_target};
  StateMachine<State, Reason> reason_machine{State::idle, *shared_reason};
  const auto target_transition = target_machine.transition(State::active, Reason::retry);
  const auto reason_transition = reason_machine.transition(State::closed, Reason::start);
  return check(target_transition.has_value() && reason_transition.has_value() &&
               target_machine.current() == State::active && reason_machine.current() == State::closed);
}

[[nodiscard]] bool check_trace_failure_is_atomic() noexcept {
  std::array<TransitionRecord<State, Reason>, 1> full_storage{};
  TransitionTrace<State, Reason, true> full_trace{full_storage};
  StateMachine<State, Reason, true> full_machine{State::idle, transitions, full_trace};
  const auto first = full_machine.transition(State::active, Reason::start);
  const auto full = full_machine.transition(State::active, Reason::retry);
  if (!check(first.has_value() && !full.has_value() && full.error().code() == ErrorCode::exhaustion &&
             full_machine.current() == State::active && full_trace.size() == 1 &&
             full_trace.sequence() == 1 && full_trace.records()[0].sequence == 1)) {
    return false;
  }

  std::array<TransitionRecord<State, Reason>, 1> zero_storage{};
  TransitionTrace<State, Reason, true> zero_trace{std::span<TransitionRecord<State, Reason>>{
      zero_storage}.first(0)};
  StateMachine<State, Reason, true> zero_machine{State::idle, transitions, zero_trace};
  const auto zero_capacity = zero_machine.transition(State::active, Reason::start);
  if (!check(!zero_capacity.has_value() && zero_capacity.error().code() == ErrorCode::exhaustion &&
             zero_machine.current() == State::idle && zero_trace.size() == 0 &&
             zero_trace.sequence() == 0)) {
    return false;
  }

  std::array<TransitionRecord<State, Reason>, 1> overflow_storage{};
  TransitionTrace<State, Reason, true> overflow_trace{overflow_storage,
                                                        std::numeric_limits<std::uint64_t>::max()};
  StateMachine<State, Reason, true> overflow_machine{State::idle, transitions, overflow_trace};
  const auto overflow = overflow_machine.transition(State::active, Reason::start);
  return check(!overflow.has_value() && overflow.error().code() == ErrorCode::overflow &&
               overflow_machine.current() == State::idle && overflow_trace.size() == 0 &&
               overflow_trace.sequence() == std::numeric_limits<std::uint64_t>::max());
}

}  // namespace

int main() {
  const std::size_t allocation_attempts_before = allocation_attempts;
  if (!check(check_disabled_trace_and_invalid_states())) {
    return 1;
  }
  if (!check(check_enabled_trace_records())) {
    return 2;
  }
  if (!check(check_distinct_full_tuple_rules())) {
    return 3;
  }
  if (!check(check_trace_failure_is_atomic())) {
    return 4;
  }
  return allocation_attempts == allocation_attempts_before ? 0 : 5;
}
