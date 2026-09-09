// SPDX-License-Identifier: AGPL-3.0-only
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <type_traits>
#include <utility>

#include <laghu/core/memory_budget.hpp>

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

using laghu::core::EmergencyMemoryReserve;
using laghu::core::Error;
using laghu::core::ErrorCode;
using laghu::core::ErrorDomain;
using laghu::core::MemoryBudget;
using laghu::core::MemoryReservation;
using laghu::core::Result;
using laghu::core::WorkerId;

[[nodiscard]] constexpr bool check(bool condition) noexcept { return condition; }

[[nodiscard]] Result<WorkerId> make_worker(std::uint64_t value) noexcept {
  return WorkerId::from_uint64(value);
}

[[nodiscard]] bool check_hierarchy_and_release(WorkerId worker) noexcept {
  MemoryBudget root{worker, 100};
  MemoryBudget child{root, 80};
  MemoryBudget leaf{child, 40};
  auto reservation = leaf.reserve(worker, 40);
  if (!check(reservation.has_value() && root.charged(worker).has_value() &&
             child.charged(worker).has_value() && leaf.charged(worker).has_value() &&
             *root.charged(worker) == 40 && *child.charged(worker) == 40 &&
             *leaf.charged(worker) == 40 && !root.ready_for_destruction(worker).has_value())) {
    return false;
  }
  const auto release = reservation->release(worker);
  return check(release.has_value() && !reservation->is_active() && *root.charged(worker) == 0 &&
               *child.charged(worker) == 0 && *leaf.charged(worker) == 0 &&
               leaf.ready_for_destruction(worker).has_value());
}

[[nodiscard]] bool check_preflight_and_limits(WorkerId worker) noexcept {
  MemoryBudget root{worker, 50};
  MemoryBudget child{root, 80};
  auto root_reservation = root.reserve(worker, 30);
  const auto rejected = child.reserve(worker, 30);
  if (!check(root_reservation.has_value() && !rejected.has_value() &&
             rejected.error().code() == ErrorCode::exhaustion && *root.charged(worker) == 30 &&
             *child.charged(worker) == 0)) {
    return false;
  }
  if (!check(root_reservation->release(worker).has_value())) {
    return false;
  }

  MemoryBudget maximal{worker, std::numeric_limits<std::size_t>::max()};
  auto exact = maximal.reserve(worker, std::numeric_limits<std::size_t>::max());
  const auto over_limit = maximal.reserve(worker, 1);
  if (!check(exact.has_value() && !over_limit.has_value() &&
             over_limit.error().code() == ErrorCode::exhaustion &&
             *maximal.charged(worker) == std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  if (!check(exact->release(worker).has_value() && *maximal.charged(worker) == 0)) {
    return false;
  }

  MemoryBudget zero{worker, 0};
  auto zero_reservation = zero.reserve(worker, 0);
  return check(zero_reservation.has_value() && *zero.charged(worker) == 0 &&
               !zero.ready_for_destruction(worker).has_value() &&
               zero_reservation->release(worker).has_value() &&
               zero.ready_for_destruction(worker).has_value());
}

[[nodiscard]] bool check_move_and_worker_affinity(WorkerId worker, WorkerId other_worker) noexcept {
  MemoryBudget budget{worker, 20};
  auto original = budget.reserve(worker, 10);
  if (!check(original.has_value())) {
    return false;
  }
  MemoryReservation moved{std::move(*original)};
  const auto wrong_worker = moved.release(other_worker);
  if (!check(!original->is_active() && moved.is_active() && !wrong_worker.has_value() &&
             wrong_worker.error().code() == ErrorCode::invalid_state && *budget.charged(worker) == 10)) {
    return false;
  }
  if (!check(moved.release(worker).has_value() && !moved.is_active() &&
             *budget.charged(worker) == 0)) {
    return false;
  }

  {
    auto release_once = budget.reserve(worker, 10);
    if (!check(release_once.has_value() && *budget.charged(worker) == 10)) {
      return false;
    }
  }
  return check(*budget.charged(worker) == 0 && budget.ready_for_destruction(worker).has_value());
}

[[nodiscard]] bool check_allocation_rollback(WorkerId worker) noexcept {
  MemoryBudget budget{worker, 16};
  const auto failed = budget.reserve_for_allocation(worker, 12, []() noexcept -> Result<void> {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                 "injected allocation failure"}};
  });
  if (!check(!failed.has_value() && failed.error().code() == ErrorCode::exhaustion &&
             *budget.charged(worker) == 0 && budget.ready_for_destruction(worker).has_value())) {
    return false;
  }
  auto succeeded = budget.reserve_for_allocation(worker, 12, []() noexcept -> Result<void> {
    return {};
  });
  return check(succeeded.has_value() && *budget.charged(worker) == 12 &&
               succeeded->release(worker).has_value() && *budget.charged(worker) == 0);
}

[[nodiscard]] bool check_emergency_isolation(WorkerId worker) noexcept {
  MemoryBudget normal{worker, 8};
  EmergencyMemoryReserve emergency{worker, 4};
  auto normal_reservation = normal.reserve(worker, 8);
  auto emergency_reservation = emergency.reserve_emergency(worker, 4);
  if (!check(normal_reservation.has_value() && emergency_reservation.has_value() &&
             *normal.charged(worker) == 8 && *emergency.charged(worker) == 4)) {
    return false;
  }
  return check(normal_reservation->release(worker).has_value() &&
               emergency_reservation->release(worker).has_value() &&
               normal.ready_for_destruction(worker).has_value() &&
               emergency.ready_for_destruction(worker).has_value());
}

static_assert(!std::is_copy_constructible_v<MemoryBudget>);
static_assert(!std::is_copy_assignable_v<MemoryBudget>);
static_assert(!std::is_move_constructible_v<MemoryBudget>);
static_assert(!std::is_copy_constructible_v<MemoryReservation>);
static_assert(!std::is_convertible_v<EmergencyMemoryReserve, MemoryBudget>);

}  // namespace

int main() {
  const auto worker = make_worker(1);
  const auto other_worker = make_worker(2);
  if (!check(worker.has_value() && other_worker.has_value())) {
    return 1;
  }
  const std::size_t allocation_attempts_before = allocation_attempts;
  if (!check(check_hierarchy_and_release(*worker))) {
    return 2;
  }
  if (!check(check_preflight_and_limits(*worker))) {
    return 3;
  }
  if (!check(check_move_and_worker_affinity(*worker, *other_worker))) {
    return 4;
  }
  if (!check(check_allocation_rollback(*worker))) {
    return 5;
  }
  if (!check(check_emergency_isolation(*worker))) {
    return 6;
  }
  return allocation_attempts == allocation_attempts_before ? 0 : 7;
}
