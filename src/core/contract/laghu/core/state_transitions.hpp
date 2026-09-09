// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

#include <laghu/core/contract.hpp>

namespace laghu::core {

template <class T>
concept TransitionEnum = std::is_enum_v<T>;

template <TransitionEnum State, TransitionEnum Reason>
struct TransitionRule final {
  State current;
  State next;
  Reason reason;
};

template <TransitionEnum State, TransitionEnum Reason>
struct TransitionRecord final {
  std::uint64_t sequence;
  State current;
  State next;
  Reason reason;
};

template <TransitionEnum State, TransitionEnum Reason>
class TransitionTable final {
 public:
  [[nodiscard]] static constexpr Result<TransitionTable> create(
      std::span<const TransitionRule<State, Reason>> rules) noexcept {
    if (rules.empty()) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_input, 0,
                                   "state transition table must not be empty"}};
    }

    for (std::size_t left = 0; left < rules.size(); ++left) {
      for (std::size_t right = left + 1; right < rules.size(); ++right) {
        if (rules[left].current != rules[right].current) {
          continue;
        }
        if (rules[left].next == rules[right].next && rules[left].reason == rules[right].reason) {
          return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_input, 0,
                                       "state transition table has a duplicate rule"}};
        }
      }
    }
    return TransitionTable{rules};
  }

  [[nodiscard]] constexpr bool permits(State current, State next, Reason reason) const noexcept {
    for (const TransitionRule<State, Reason>& rule : rules_) {
      if (rule.current == current && rule.next == next && rule.reason == reason) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] constexpr std::size_t size() const noexcept { return rules_.size(); }

 private:
  explicit constexpr TransitionTable(std::span<const TransitionRule<State, Reason>> rules) noexcept
      : rules_(rules) {}

  std::span<const TransitionRule<State, Reason>> rules_;
};

template <TransitionEnum State, TransitionEnum Reason, bool Enabled>
class TransitionTrace;

template <TransitionEnum State, TransitionEnum Reason>
class TransitionTrace<State, Reason, false> final {
 public:
  static constexpr bool enabled = false;
};

template <TransitionEnum State, TransitionEnum Reason>
class TransitionTrace<State, Reason, true> final {
 public:
  static constexpr bool enabled = true;

  constexpr explicit TransitionTrace(std::span<TransitionRecord<State, Reason>> records,
                                     std::uint64_t sequence = 0) noexcept
      : records_(records), sequence_(sequence) {}

  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr std::size_t capacity() const noexcept { return records_.size(); }
  [[nodiscard]] constexpr std::uint64_t sequence() const noexcept { return sequence_; }
  [[nodiscard]] constexpr std::span<const TransitionRecord<State, Reason>> records() const noexcept {
    return records_.first(size_);
  }

 private:
  template <TransitionEnum, TransitionEnum, bool>
  friend class StateMachine;

  [[nodiscard]] constexpr Result<void> append(State current, State next, Reason reason) noexcept {
    if (size_ == records_.size()) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                   "state transition trace capacity is exhausted"}};
    }
    if (sequence_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::overflow, 0,
                                   "state transition trace sequence overflow"}};
    }
    records_[size_] = TransitionRecord<State, Reason>{sequence_ + 1, current, next, reason};
    ++size_;
    ++sequence_;
    return {};
  }

  std::span<TransitionRecord<State, Reason>> records_;
  std::size_t size_{};
  std::uint64_t sequence_{};
};

template <TransitionEnum State, TransitionEnum Reason, bool TraceEnabled = false>
class StateMachine;

template <TransitionEnum State, TransitionEnum Reason>
class StateMachine<State, Reason, false> final {
 public:
  constexpr StateMachine(State initial, const TransitionTable<State, Reason>& transitions) noexcept
      : current_(initial), transitions_(&transitions) {}

  [[nodiscard]] constexpr State current() const noexcept { return current_; }

  [[nodiscard]] constexpr Result<void> transition(State next, Reason reason) noexcept {
    if (!transitions_->permits(current_, next, reason)) {
      return std::unexpected{invalid_transition_error()};
    }
    current_ = next;
    return {};
  }

 private:
  [[nodiscard]] static constexpr Error invalid_transition_error() noexcept {
    return Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                 "state transition is not permitted"};
  }

  State current_;
  const TransitionTable<State, Reason>* transitions_;
};

template <TransitionEnum State, TransitionEnum Reason>
class StateMachine<State, Reason, true> final {
 public:
  constexpr StateMachine(State initial, const TransitionTable<State, Reason>& transitions,
                         TransitionTrace<State, Reason, true>& trace) noexcept
      : current_(initial), transitions_(&transitions), trace_(&trace) {}

  [[nodiscard]] constexpr State current() const noexcept { return current_; }

  [[nodiscard]] constexpr Result<void> transition(State next, Reason reason) noexcept {
    if (!transitions_->permits(current_, next, reason)) {
      return std::unexpected{invalid_transition_error()};
    }
    if (const Result<void> appended = trace_->append(current_, next, reason);
        !appended.has_value()) {
      return std::unexpected{appended.error()};
    }
    current_ = next;
    return {};
  }

 private:
  [[nodiscard]] static constexpr Error invalid_transition_error() noexcept {
    return Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                 "state transition is not permitted"};
  }

  State current_;
  const TransitionTable<State, Reason>* transitions_;
  TransitionTrace<State, Reason, true>* trace_;
};

}  // namespace laghu::core
