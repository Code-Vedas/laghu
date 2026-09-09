// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

#include <laghu/core/slab_pool.hpp>

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

using laghu::core::ErrorCode;
using laghu::core::Result;
using laghu::core::SlabPool;
using laghu::core::SlabPoolDebugHooks;
using laghu::core::SlabPoolSlot;
using laghu::core::WorkerId;

struct Probe final {
  explicit Probe(std::uint64_t initial_value) noexcept : value(initial_value) { ++constructions; }
  ~Probe() { ++destructions; }

  std::uint64_t value{};
  static std::size_t constructions;
  static std::size_t destructions;
};

std::size_t Probe::constructions{};
std::size_t Probe::destructions{};

struct alignas(64) AlignedProbe final {
  explicit AlignedProbe(std::uint64_t initial_value) noexcept : value(initial_value) {}
  std::uint64_t value{};
};

template <class T, std::size_t Capacity>
struct PoolStorage final {
  static constexpr std::size_t bytes = *SlabPool<T>::required_storage_bytes(Capacity);
  static constexpr std::size_t alignment = SlabPool<T>::required_storage_alignment();
  std::array<SlabPoolSlot<T>, Capacity> slots{};
};

template <class T, std::size_t Capacity>
struct alignas(128) OveralignedPoolStorage final {
  std::array<SlabPoolSlot<T>, Capacity> slots{};
};

constexpr std::size_t misaligned_selected_alignment = PoolStorage<Probe, 1>::alignment * 2;

struct alignas(misaligned_selected_alignment) MisalignedSelectedStorage final {
  std::byte prefix{};
  std::array<SlabPoolSlot<Probe>, 1> slots{};
};

static_assert(offsetof(MisalignedSelectedStorage, slots) % misaligned_selected_alignment != 0);

struct DebugRecorder final {
  std::size_t double_releases{};
};

void record_double_release(void* context) noexcept {
  ++static_cast<DebugRecorder*>(context)->double_releases;
}

[[nodiscard]] constexpr bool check(bool condition) noexcept { return condition; }

[[nodiscard]] Result<WorkerId> make_worker(std::uint64_t value) noexcept {
  return WorkerId::from_uint64(value);
}

[[nodiscard]] bool check_capacity_reuse_and_lifetime(WorkerId worker) noexcept {
  Probe::constructions = 0;
  Probe::destructions = 0;
  PoolStorage<Probe, 3> storage{};
  SlabPool<Probe> pool{worker, storage.slots, 3, PoolStorage<Probe, 3>::alignment};
  auto first = pool.try_acquire(worker, 1);
  auto second = pool.try_acquire(worker, 2);
  auto third = pool.try_acquire(worker, 3);
  const auto exhausted = pool.try_acquire(worker, 4);
  if (!check(first.has_value() && second.has_value() && third.has_value() && !exhausted.has_value() &&
             exhausted.error().code() == ErrorCode::exhaustion && pool.live_leases() == 3 &&
             pool.available() == 0 && first->get()->value == 1 && second->get()->value == 2 &&
             third->get()->value == 3)) {
    return false;
  }
  Probe* const reused_address = second->get();
  if (!check(second->release(worker).has_value() && Probe::destructions == 1 &&
             pool.live_leases() == 2 && pool.available() == 1)) {
    return false;
  }
  auto reused = pool.try_acquire(worker, 9);
  if (!check(reused.has_value() && reused->get() == reused_address && reused->get()->value == 9 &&
             Probe::constructions == 4)) {
    return false;
  }
  return check(first->release(worker).has_value() && third->release(worker).has_value() &&
               reused->release(worker).has_value() && Probe::destructions == 4 &&
               pool.live_leases() == 0 && pool.available() == 3);
}

[[nodiscard]] bool check_alignment_and_zero_capacity(WorkerId worker) noexcept {
  constexpr std::size_t selected_alignment = 64;
  static_assert(PoolStorage<Probe, 1>::alignment <= selected_alignment);
  OveralignedPoolStorage<Probe, 1> over_aligned_storage{};
  SlabPool<Probe> over_aligned_pool{worker, over_aligned_storage.slots, 1, selected_alignment};
  auto over_aligned = over_aligned_pool.try_acquire(worker, 7);
  if (!check(over_aligned.has_value() && over_aligned_pool.alignment() == selected_alignment &&
             reinterpret_cast<std::uintptr_t>(over_aligned->get()) % alignof(Probe) == 0 &&
             over_aligned->release(worker).has_value())) {
    return false;
  }

  static_assert(sizeof(SlabPoolSlot<AlignedProbe>) % PoolStorage<AlignedProbe, 3>::alignment == 0);
  PoolStorage<AlignedProbe, 3> multi_slot_storage{};
  SlabPool<AlignedProbe> multi_slot_pool{worker, multi_slot_storage.slots, 3,
                                         PoolStorage<AlignedProbe, 3>::alignment};
  auto first = multi_slot_pool.try_acquire(worker, 1);
  auto second = multi_slot_pool.try_acquire(worker, 2);
  auto third = multi_slot_pool.try_acquire(worker, 3);
  if (!check(first.has_value() && second.has_value() && third.has_value() &&
             reinterpret_cast<std::uintptr_t>(first->get()) % multi_slot_pool.alignment() == 0 &&
             reinterpret_cast<std::uintptr_t>(second->get()) % multi_slot_pool.alignment() == 0 &&
             reinterpret_cast<std::uintptr_t>(third->get()) % multi_slot_pool.alignment() == 0 &&
             first->release(worker).has_value() && second->release(worker).has_value() &&
             third->release(worker).has_value())) {
    return false;
  }

  PoolStorage<AlignedProbe, 1> aligned_storage{};
  SlabPool<AlignedProbe> aligned_pool{worker, aligned_storage.slots, 1,
                                      PoolStorage<AlignedProbe, 1>::alignment};
  auto aligned = aligned_pool.try_acquire(worker, 7);
  if (!check(aligned.has_value() &&
             reinterpret_cast<std::uintptr_t>(aligned->get()) % alignof(AlignedProbe) == 0 &&
             aligned->release(worker).has_value())) {
    return false;
  }

  std::array<SlabPoolSlot<Probe>, 0> empty_storage{};
  SlabPool<Probe> empty_pool{worker, empty_storage, 0, PoolStorage<Probe, 1>::alignment};
  const auto exhausted = empty_pool.try_acquire(worker, 1);
  return check(!exhausted.has_value() && exhausted.error().code() == ErrorCode::exhaustion &&
               empty_pool.capacity() == 0 && empty_pool.available() == 0);
}

[[nodiscard]] bool check_worker_storage_and_overflow_failures(WorkerId worker,
                                                               WorkerId other_worker) noexcept {
  PoolStorage<Probe, 1> storage{};
  SlabPool<Probe> pool{worker, storage.slots, 1, PoolStorage<Probe, 1>::alignment};
  const auto wrong_worker = pool.try_acquire(other_worker, 1);
  if (!check(!wrong_worker.has_value() && wrong_worker.error().code() == ErrorCode::invalid_state)) {
    return false;
  }

  std::array<SlabPoolSlot<Probe>, 0> too_small_storage{};
  SlabPool<Probe> too_small{worker, too_small_storage, 1, PoolStorage<Probe, 1>::alignment};
  const auto too_small_result = too_small.try_acquire(worker, 1);
  if (!check(!too_small_result.has_value() && too_small_result.error().code() == ErrorCode::invalid_input)) {
    return false;
  }

  SlabPool<Probe> underspecified{worker, storage.slots, 1,
                                  PoolStorage<Probe, 1>::alignment / 2};
  const auto underspecified_result = underspecified.try_acquire(worker, 1);
  if (!check(!underspecified_result.has_value() &&
             underspecified_result.error().code() == ErrorCode::invalid_input)) {
    return false;
  }

  SlabPool<Probe> non_power_of_two{worker, storage.slots, 1,
                                   PoolStorage<Probe, 1>::alignment + 1};
  const auto non_power_of_two_result = non_power_of_two.try_acquire(worker, 1);
  if (!check(!non_power_of_two_result.has_value() &&
             non_power_of_two_result.error().code() == ErrorCode::invalid_input)) {
    return false;
  }

  static_assert(sizeof(SlabPoolSlot<Probe>) % misaligned_selected_alignment != 0);
  OveralignedPoolStorage<Probe, 2> incompatible_stride_storage{};
  SlabPool<Probe> incompatible_stride{worker, incompatible_stride_storage.slots, 2,
                                      misaligned_selected_alignment};
  const auto incompatible_stride_result = incompatible_stride.try_acquire(worker, 1);
  if (!check(!incompatible_stride_result.has_value() &&
             incompatible_stride_result.error().code() == ErrorCode::invalid_input)) {
    return false;
  }

  MisalignedSelectedStorage misaligned_storage{};
  std::span<SlabPoolSlot<Probe>, 1> misaligned_slot{misaligned_storage.slots};
  SlabPool<Probe> misaligned{worker, misaligned_slot, 1, misaligned_selected_alignment};
  const auto misaligned_result = misaligned.try_acquire(worker, 1);
  if (!check(!misaligned_result.has_value() &&
             misaligned_result.error().code() == ErrorCode::invalid_input)) {
    return false;
  }

  const auto overflowing = SlabPool<Probe>::required_storage_bytes(
      std::numeric_limits<std::size_t>::max());
  return check(!overflowing.has_value() && overflowing.error().code() == ErrorCode::overflow);
}

[[nodiscard]] bool check_move_and_debug_double_release(WorkerId worker) noexcept {
  PoolStorage<Probe, 1> storage{};
  DebugRecorder recorder{};
  SlabPool<Probe> pool{worker, storage.slots, 1, PoolStorage<Probe, 1>::alignment,
                       SlabPoolDebugHooks{&recorder, record_double_release}};
  auto original = pool.try_acquire(worker, 4);
  if (!check(original.has_value())) {
    return false;
  }
  typename SlabPool<Probe>::Lease moved{std::move(*original)};
  const auto moved_from_release = original->release(worker);
  if (!check(!moved_from_release.has_value() && moved_from_release.error().code() == ErrorCode::invalid_state &&
             recorder.double_releases == 0 && moved.is_active() && !original->is_active())) {
    return false;
  }
  if (!check(moved.release(worker).has_value() && !moved.is_active() && pool.live_leases() == 0)) {
    return false;
  }
  const auto double_release = moved.release(worker);
  return check(!double_release.has_value() && double_release.error().code() == ErrorCode::invalid_state &&
               recorder.double_releases == 1);
}

[[nodiscard]] bool check_raii_release(WorkerId worker) noexcept {
  Probe::constructions = 0;
  Probe::destructions = 0;
  PoolStorage<Probe, 1> storage{};
  SlabPool<Probe> pool{worker, storage.slots, 1, PoolStorage<Probe, 1>::alignment};
  {
    auto lease = pool.try_acquire(worker, 6);
    if (!check(lease.has_value() && lease->is_active() && pool.live_leases() == 1)) {
      return false;
    }
  }
  return check(pool.live_leases() == 0 && pool.available() == 1 && Probe::constructions == 1 &&
               Probe::destructions == 1);
}

[[nodiscard]] bool child_terminates_for_owner_lifetime(WorkerId worker) noexcept {
  const pid_t process = ::fork();
  if (process == 0) {
    PoolStorage<Probe, 1> storage{};
    typename SlabPool<Probe>::Lease lease;
    {
      SlabPool<Probe> pool{worker, storage.slots, 1, PoolStorage<Probe, 1>::alignment};
      auto acquired = pool.try_acquire(worker, 1);
      if (!acquired.has_value()) {
        _exit(3);
      }
      lease = std::move(*acquired);
    }
    _exit(0);
  }
  if (process < 0) {
    return false;
  }
  int status{};
  return ::waitpid(process, &status, 0) == process && WIFSIGNALED(status);
}

static_assert(!std::is_copy_constructible_v<SlabPool<Probe>>);
static_assert(!std::is_move_constructible_v<SlabPool<Probe>>);
static_assert(!std::is_copy_constructible_v<SlabPool<Probe>::Lease>);
static_assert(std::is_nothrow_move_constructible_v<SlabPool<Probe>::Lease>);
static_assert(std::is_nothrow_move_assignable_v<SlabPool<Probe>::Lease>);

}  // namespace

int main() {
  const auto worker = make_worker(1);
  const auto other_worker = make_worker(2);
  if (!check(worker.has_value() && other_worker.has_value())) {
    return 1;
  }
  const std::size_t allocation_attempts_before = allocation_attempts;
  if (!check(check_capacity_reuse_and_lifetime(*worker))) {
    return 2;
  }
  if (!check(check_alignment_and_zero_capacity(*worker))) {
    return 3;
  }
  if (!check(check_worker_storage_and_overflow_failures(*worker, *other_worker))) {
    return 4;
  }
  if (!check(check_move_and_debug_double_release(*worker))) {
    return 5;
  }
  if (!check(check_raii_release(*worker))) {
    return 6;
  }
  if (!check(child_terminates_for_owner_lifetime(*worker))) {
    return 7;
  }
  return allocation_attempts == allocation_attempts_before ? 0 : 8;
}
