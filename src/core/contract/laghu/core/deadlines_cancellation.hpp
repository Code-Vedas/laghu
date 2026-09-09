// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <laghu/core/contract.hpp>

namespace laghu::core {

using MonotonicInstant = std::uint64_t;

// Clocks are supplied by the owner.  Production code can inject its POSIX
// monotonic clock and tests can inject a deterministic clock without a timer
// thread or dynamic dispatch.
template <class Clock>
concept MonotonicClock = requires(const Clock& clock) {
  { clock.now() } noexcept -> std::same_as<MonotonicInstant>;
};

class Deadline final {
 public:
  [[nodiscard]] static constexpr Deadline at(MonotonicInstant instant) noexcept {
    return Deadline{instant};
  }

  template <MonotonicClock Clock>
  [[nodiscard]] static constexpr Result<Deadline> after(const Clock& clock,
                                                         MonotonicInstant duration) noexcept {
    const MonotonicInstant now = clock.now();
    if (duration > std::numeric_limits<MonotonicInstant>::max() - now) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::overflow, 0,
                                   "monotonic deadline overflows"}};
    }
    return Deadline{now + duration};
  }

  [[nodiscard]] static constexpr Deadline child(Deadline parent, Deadline requested) noexcept {
    return Deadline{parent.instant_ < requested.instant_ ? parent.instant_ : requested.instant_};
  }

  [[nodiscard]] constexpr MonotonicInstant instant() const noexcept { return instant_; }

  template <MonotonicClock Clock>
  [[nodiscard]] constexpr bool expired(const Clock& clock) const noexcept {
    return clock.now() >= instant_;
  }

  template <MonotonicClock Clock>
  [[nodiscard]] constexpr Result<void> require_not_expired(const Clock& clock) const noexcept {
    if (expired(clock)) {
      return std::unexpected{deadline_error()};
    }
    return {};
  }

  [[nodiscard]] static constexpr Error deadline_error() noexcept {
    return Error{ErrorDomain::core, ErrorCode::deadline, 0, "monotonic deadline expired"};
  }

 private:
  explicit constexpr Deadline(MonotonicInstant instant) noexcept : instant_(instant) {}

  MonotonicInstant instant_;
};

enum class CancellationTerminal : std::uint8_t {
  pending,
  completed,
  cancelled,
};

enum class CancellationCause : std::uint8_t {
  none,
  explicit_request,
  deadline_expired,
};

struct CancellationOutcome final {
  CancellationTerminal terminal;
  CancellationCause cause;

  [[nodiscard]] constexpr bool is_terminal() const noexcept {
    return terminal != CancellationTerminal::pending;
  }

  [[nodiscard]] constexpr bool is_cancelled() const noexcept {
    return terminal == CancellationTerminal::cancelled;
  }

  [[nodiscard]] constexpr Result<void> require_active() const noexcept {
    if (terminal == CancellationTerminal::cancelled) {
      return std::unexpected{cancellation_error(cause)};
    }
    if (terminal == CancellationTerminal::completed) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                                   "operation has already completed"}};
    }
    return {};
  }

  [[nodiscard]] static constexpr Error cancellation_error(CancellationCause cause) noexcept {
    if (cause == CancellationCause::deadline_expired) {
      return Deadline::deadline_error();
    }
    return Error{ErrorDomain::core, ErrorCode::cancellation, 0,
                 "operation cancellation was requested"};
  }
};

class CancellationState final {
 public:
  constexpr CancellationState() noexcept = default;
  CancellationState(const CancellationState&) = delete;
  CancellationState& operator=(const CancellationState&) = delete;
  CancellationState(CancellationState&&) = delete;
  CancellationState& operator=(CancellationState&&) = delete;

 private:
  enum class StoredTerminal : std::uint8_t {
    pending,
    completed,
    cancelled_explicit,
    cancelled_deadline,
  };

  friend class CancellationSource;
  friend class CancellationToken;

  [[nodiscard]] CancellationOutcome outcome() const noexcept {
    // The successful compare-exchange below publishes both terminal state and
    // its cause in one atomic value.  Once terminal, no writer can alter it.
    switch (terminal_.load(std::memory_order_acquire)) {
      case StoredTerminal::pending:
        return {CancellationTerminal::pending, CancellationCause::none};
      case StoredTerminal::completed:
        return {CancellationTerminal::completed, CancellationCause::none};
      case StoredTerminal::cancelled_explicit:
        return {CancellationTerminal::cancelled, CancellationCause::explicit_request};
      case StoredTerminal::cancelled_deadline:
        return {CancellationTerminal::cancelled, CancellationCause::deadline_expired};
    }
    return {CancellationTerminal::pending, CancellationCause::none};
  }

  [[nodiscard]] CancellationOutcome finish(StoredTerminal requested) noexcept {
    StoredTerminal expected = StoredTerminal::pending;
    if (terminal_.compare_exchange_strong(expected, requested, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
      return outcome();
    }
    return outcome();
  }

  std::atomic<StoredTerminal> terminal_{StoredTerminal::pending};
};

class CancellationToken final {
 public:
  static constexpr std::uint8_t maximum_parent_depth = 8;

  constexpr CancellationToken(const CancellationToken&) noexcept = default;
  constexpr CancellationToken& operator=(const CancellationToken&) noexcept = default;

  [[nodiscard]] CancellationOutcome outcome() const noexcept {
    const CancellationOutcome own = states_[0]->outcome();
    if (own.is_terminal()) {
      return own;
    }

    for (std::uint8_t index = 1; index < state_count_; ++index) {
      const CancellationOutcome inherited = states_[index]->outcome();
      if (inherited.is_cancelled()) {
        return inherited;
      }
    }
    return own;
  }

  [[nodiscard]] Result<void> require_active() const noexcept { return outcome().require_active(); }

  template <MonotonicClock Clock>
  [[nodiscard]] Result<void> require_active(const Deadline& deadline,
                                             const Clock& clock) const noexcept {
    if (const Result<void> active = require_active(); !active.has_value()) {
      return std::unexpected{active.error()};
    }
    return deadline.require_not_expired(clock);
  }

  [[nodiscard]] Result<CancellationToken> child(CancellationState& child_state) const noexcept {
    for (std::uint8_t index = 0; index < state_count_; ++index) {
      if (states_[index] == &child_state) {
        return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                                     "cancellation parent cycle is not allowed"}};
      }
    }
    if (state_count_ > maximum_parent_depth) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_range, 0,
                                   "cancellation parent depth exceeds 8"}};
    }
    CancellationToken child{child_state};
    for (std::uint8_t index = 0; index < state_count_; ++index) {
      child.states_[static_cast<std::size_t>(index) + 1U] =
          states_[static_cast<std::size_t>(index)];
    }
    child.state_count_ = static_cast<std::uint8_t>(state_count_ + 1);
    return child;
  }

 private:
  friend class CancellationSource;

  constexpr explicit CancellationToken(CancellationState& state) noexcept : states_{&state} {}

  std::array<CancellationState*, maximum_parent_depth + 1> states_{};
  std::uint8_t state_count_{1};
};

class CancellationSource final {
 public:
  // The caller owns state and must keep it alive for every source and token
  // that refers to it.  Tokens copy only these borrowed state references.
  explicit constexpr CancellationSource(CancellationState& state) noexcept : state_(&state) {}

  CancellationSource(const CancellationSource&) = delete;
  CancellationSource& operator=(const CancellationSource&) = delete;
  CancellationSource(CancellationSource&&) = delete;
  CancellationSource& operator=(CancellationSource&&) = delete;

  [[nodiscard]] constexpr CancellationToken token() const noexcept { return CancellationToken{*state_}; }

  [[nodiscard]] CancellationOutcome complete() noexcept {
    return state_->finish(CancellationState::StoredTerminal::completed);
  }

  [[nodiscard]] CancellationOutcome cancel() noexcept {
    return state_->finish(CancellationState::StoredTerminal::cancelled_explicit);
  }

  template <MonotonicClock Clock>
  [[nodiscard]] CancellationOutcome cancel_if_expired(const Deadline& deadline,
                                                       const Clock& clock) noexcept {
    if (!deadline.expired(clock)) {
      return state_->outcome();
    }
    return state_->finish(CancellationState::StoredTerminal::cancelled_deadline);
  }

 private:
  CancellationState* state_;
};

static_assert(!std::is_copy_constructible_v<CancellationState>);
static_assert(!std::is_copy_constructible_v<CancellationSource>);
static_assert(std::is_copy_constructible_v<CancellationToken>);
static_assert(std::is_trivially_copyable_v<CancellationToken>);

}  // namespace laghu::core
