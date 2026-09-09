// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <exception>
#include <limits>
#include <type_traits>
#include <utility>

#include <laghu/core/identifiers.hpp>

namespace laghu::core {

class MemoryBudget;

class MemoryReservation final {
 public:
  constexpr MemoryReservation() noexcept = default;
  MemoryReservation(const MemoryReservation&) = delete;
  MemoryReservation& operator=(const MemoryReservation&) = delete;
  MemoryReservation(MemoryReservation&& other) noexcept;
  MemoryReservation& operator=(MemoryReservation&& other) noexcept;
  ~MemoryReservation();

  [[nodiscard]] constexpr bool is_active() const noexcept { return budget_ != nullptr; }
  [[nodiscard]] constexpr std::size_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] Result<void> release(WorkerId worker) noexcept;

 private:
  friend class MemoryBudget;

  constexpr MemoryReservation(MemoryBudget& budget, std::size_t bytes) noexcept
      : budget_(&budget), bytes_(bytes) {}

  void release_unchecked() noexcept;
  void move_from(MemoryReservation&& other) noexcept;

  MemoryBudget* budget_{};
  std::size_t bytes_{};
};

class MemoryBudget final {
 public:
  // A root budget is owned by exactly one worker identity.
  explicit constexpr MemoryBudget(WorkerId worker, std::size_t limit) noexcept
      : worker_(worker), limit_(limit) {}

  // A child permanently inherits its parent's worker and remains parent-owned.
  explicit MemoryBudget(MemoryBudget& parent, std::size_t limit) noexcept
      : worker_(parent.worker_), limit_(limit), parent_(&parent) {
    parent_->register_child();
  }

  MemoryBudget(const MemoryBudget&) = delete;
  MemoryBudget& operator=(const MemoryBudget&) = delete;
  MemoryBudget(MemoryBudget&&) = delete;
  MemoryBudget& operator=(MemoryBudget&&) = delete;

  ~MemoryBudget() {
    // A parent must outlive all children and reservations.  Callers can check
    // ready_for_destruction() before leaving a controlled owner boundary.
    if (live_reservations_ != 0 || live_children_ != 0) {
      std::terminate();
    }
    if (parent_ != nullptr) {
      parent_->unregister_child();
    }
  }

  [[nodiscard]] constexpr std::size_t limit() const noexcept { return limit_; }

  [[nodiscard]] Result<std::size_t> charged(WorkerId worker) const noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    return charged_;
  }

  [[nodiscard]] Result<std::size_t> available(WorkerId worker) const noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    return limit_ - charged_;
  }

  [[nodiscard]] Result<void> ready_for_destruction(WorkerId worker) const noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    if (live_reservations_ != 0 || live_children_ != 0) {
      return std::unexpected{lifetime_error()};
    }
    return {};
  }

  [[nodiscard]] Result<MemoryReservation> reserve(WorkerId worker, std::size_t bytes) noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    if (const auto preflight = preflight_reservation(bytes); !preflight.has_value()) {
      return std::unexpected{preflight.error()};
    }

    for (MemoryBudget* current = this; current != nullptr; current = current->parent_) {
      current->charged_ += bytes;
      ++current->live_reservations_;
    }
    return MemoryReservation{*this, bytes};
  }

  // This is the explicit charge-before-acquire path.  An acquisition failure
  // destroys the temporary reservation and therefore rolls every charge back.
  template <class Acquisition>
    requires std::is_nothrow_invocable_r_v<Result<void>, Acquisition&&>
  [[nodiscard]] Result<MemoryReservation> reserve_for_allocation(
      WorkerId worker, std::size_t bytes, Acquisition&& acquire) noexcept {
    auto reservation = reserve(worker, bytes);
    if (!reservation.has_value()) {
      return std::unexpected{reservation.error()};
    }
    const Result<void> acquisition = std::forward<Acquisition>(acquire)();
    if (!acquisition.has_value()) {
      return std::unexpected{acquisition.error()};
    }
    return std::move(*reservation);
  }

 private:
  friend class MemoryReservation;

  [[nodiscard]] constexpr bool owns(WorkerId worker) const noexcept {
    return worker_.value() == worker.value();
  }

  [[nodiscard]] Result<void> require_worker(WorkerId worker) const noexcept {
    if (!owns(worker)) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                                   "memory budget accessed by a different worker"}};
    }
    return {};
  }

  [[nodiscard]] Result<void> preflight_reservation(std::size_t bytes) const noexcept {
    for (const MemoryBudget* current = this; current != nullptr; current = current->parent_) {
      if (bytes > current->limit_ - current->charged_) {
        return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                     "memory budget capacity is exhausted"}};
      }
      if (current->live_reservations_ == std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{Error{ErrorDomain::core, ErrorCode::overflow, 0,
                                     "memory budget reservation count overflow"}};
      }
    }
    return {};
  }

  [[nodiscard]] static constexpr Error lifetime_error() noexcept {
    return Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                 "memory budget still has live children or reservations"};
  }

  void release(std::size_t bytes) noexcept {
    for (MemoryBudget* current = this; current != nullptr; current = current->parent_) {
      if (current->charged_ < bytes || current->live_reservations_ == 0) {
        std::terminate();
      }
      current->charged_ -= bytes;
      --current->live_reservations_;
    }
  }

  void register_child() noexcept {
    if (live_children_ == std::numeric_limits<std::size_t>::max()) {
      std::terminate();
    }
    ++live_children_;
  }

  void unregister_child() noexcept {
    if (live_children_ == 0) {
      std::terminate();
    }
    --live_children_;
  }

  WorkerId worker_;
  std::size_t limit_{};
  std::size_t charged_{};
  std::size_t live_reservations_{};
  std::size_t live_children_{};
  MemoryBudget* const parent_{};
};

class EmergencyMemoryReserve final {
 public:
  explicit constexpr EmergencyMemoryReserve(WorkerId worker, std::size_t limit) noexcept
      : budget_(worker, limit) {}

  EmergencyMemoryReserve(const EmergencyMemoryReserve&) = delete;
  EmergencyMemoryReserve& operator=(const EmergencyMemoryReserve&) = delete;
  EmergencyMemoryReserve(EmergencyMemoryReserve&&) = delete;
  EmergencyMemoryReserve& operator=(EmergencyMemoryReserve&&) = delete;

  [[nodiscard]] Result<MemoryReservation> reserve_emergency(WorkerId worker,
                                                              std::size_t bytes) noexcept {
    return budget_.reserve(worker, bytes);
  }

  template <class Acquisition>
    requires std::is_nothrow_invocable_r_v<Result<void>, Acquisition&&>
  [[nodiscard]] Result<MemoryReservation> reserve_emergency_for_allocation(
      WorkerId worker, std::size_t bytes, Acquisition&& acquire) noexcept {
    return budget_.reserve_for_allocation(worker, bytes, std::forward<Acquisition>(acquire));
  }

  [[nodiscard]] Result<std::size_t> charged(WorkerId worker) const noexcept {
    return budget_.charged(worker);
  }

  [[nodiscard]] Result<void> ready_for_destruction(WorkerId worker) const noexcept {
    return budget_.ready_for_destruction(worker);
  }

 private:
  MemoryBudget budget_;
};

inline MemoryReservation::MemoryReservation(MemoryReservation&& other) noexcept {
  move_from(std::move(other));
}

inline MemoryReservation& MemoryReservation::operator=(MemoryReservation&& other) noexcept {
  if (this != &other) {
    release_unchecked();
    move_from(std::move(other));
  }
  return *this;
}

inline MemoryReservation::~MemoryReservation() { release_unchecked(); }

inline Result<void> MemoryReservation::release(WorkerId worker) noexcept {
  if (budget_ == nullptr) {
    return {};
  }
  if (const auto owner = budget_->require_worker(worker); !owner.has_value()) {
    return std::unexpected{owner.error()};
  }
  release_unchecked();
  return {};
}

inline void MemoryReservation::release_unchecked() noexcept {
  if (budget_ != nullptr) {
    MemoryBudget* const budget = budget_;
    const std::size_t bytes = bytes_;
    budget_ = nullptr;
    bytes_ = 0;
    budget->release(bytes);
  }
}

inline void MemoryReservation::move_from(MemoryReservation&& other) noexcept {
  budget_ = other.budget_;
  bytes_ = other.bytes_;
  other.budget_ = nullptr;
  other.bytes_ = 0;
}

static_assert(!std::is_copy_constructible_v<MemoryBudget>);
static_assert(!std::is_copy_assignable_v<MemoryBudget>);
static_assert(!std::is_move_constructible_v<MemoryBudget>);
static_assert(!std::is_move_assignable_v<MemoryBudget>);
static_assert(!std::is_copy_constructible_v<MemoryReservation>);
static_assert(!std::is_copy_assignable_v<MemoryReservation>);
static_assert(std::is_nothrow_move_constructible_v<MemoryReservation>);
static_assert(std::is_nothrow_move_assignable_v<MemoryReservation>);
static_assert(!std::is_convertible_v<EmergencyMemoryReserve, MemoryBudget>);

}  // namespace laghu::core
