// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <memory_resource>
#include <new>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>

#include <laghu/core/bounded_arena.hpp>

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

using laghu::core::ArenaBlockSource;
using laghu::core::ArenaPmrResource;
using laghu::core::BoundedArena;
using laghu::core::Error;
using laghu::core::ErrorCode;
using laghu::core::ErrorDomain;
using laghu::core::MemoryBudget;
using laghu::core::MutableByteView;
using laghu::core::Result;
using laghu::core::WorkerId;

struct FixedBlockSource final {
  alignas(64) std::array<std::byte, 128> storage{};
  std::size_t acquire_calls{};
  std::size_t reset_calls{};
  bool fail_acquire{};
  bool fail_reset{};
};

struct DestructionProbe final {
  ~DestructionProbe() { ++destructions; }

  static std::size_t destructions;
};

std::size_t DestructionProbe::destructions{};

[[nodiscard]] Result<MutableByteView> acquire_fixed_block(void* context,
                                                          std::size_t minimum_capacity) noexcept {
  auto* const source = static_cast<FixedBlockSource*>(context);
  ++source->acquire_calls;
  if (source->fail_acquire) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                 "injected bounded arena block acquisition failure"}};
  }
  if (minimum_capacity > source->storage.size()) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                 "fixed bounded arena block is too small"}};
  }
  return MutableByteView::from(source->storage);
}

[[nodiscard]] Result<void> reset_fixed_block(void* context) noexcept {
  auto* const source = static_cast<FixedBlockSource*>(context);
  ++source->reset_calls;
  if (source->fail_reset) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::io, 0,
                                 "injected bounded arena block reset failure"}};
  }
  return {};
}

[[nodiscard]] constexpr bool check(bool condition) noexcept { return condition; }

[[nodiscard]] Result<WorkerId> make_worker(std::uint64_t value) noexcept {
  return WorkerId::from_uint64(value);
}

[[nodiscard]] ArenaBlockSource fixed_source(FixedBlockSource& source) noexcept {
  return ArenaBlockSource{&source, acquire_fixed_block, reset_fixed_block};
}

[[nodiscard]] bool check_growth_alignment_and_budget(WorkerId worker) noexcept {
  FixedBlockSource source{};
  MemoryBudget root{worker, 32};
  MemoryBudget budget{root, 32};
  BoundedArena arena{worker, budget, fixed_source(source), 8, 32};
  const auto first = arena.try_allocate(worker, 1, 8);
  const auto second = arena.try_allocate(worker, 1, 16);
  const auto oversized = arena.try_allocate(worker, 17, 1);
  if (!check(first.has_value() && second.has_value() && !oversized.has_value() &&
             arena.initial_capacity() == 8 && arena.maximum_capacity() == 32 &&
             arena.capacity() == 32 && arena.used() == 17 && source.acquire_calls == 1 &&
             budget.charged(worker).has_value() && *budget.charged(worker) == 32 &&
             root.charged(worker).has_value() && *root.charged(worker) == 32 &&
             oversized.error().code() == ErrorCode::exhaustion)) {
    return false;
  }
  const auto first_bytes = first->bytes();
  const auto second_bytes = second->bytes();
  if (!check(first_bytes.has_value() && second_bytes.has_value() &&
             reinterpret_cast<std::uintptr_t>(first_bytes->data()) % 8 == 0 &&
             reinterpret_cast<std::uintptr_t>(second_bytes->data()) % 16 == 0)) {
    return false;
  }
  const auto boundary = arena.quiescent_boundary(worker);
  if (!check(boundary.has_value() && arena.reset(worker, *boundary).has_value())) {
    return false;
  }
  return check(arena.capacity() == 0 && arena.used() == 0 && arena.generation() == 2 &&
               source.reset_calls == 1 && *budget.charged(worker) == 0 &&
               *root.charged(worker) == 0 && budget.ready_for_destruction(worker).has_value());
}

[[nodiscard]] bool check_failures_and_rollback(WorkerId worker, WorkerId other_worker) noexcept {
  FixedBlockSource source{};
  source.fail_acquire = true;
  MemoryBudget root{worker, 16};
  MemoryBudget budget{root, 16};
  BoundedArena arena{worker, budget, fixed_source(source), 8, 16};
  const auto failed = arena.try_allocate(worker, 1, 1);
  const auto wrong_worker = arena.try_allocate(other_worker, 1, 1);
  const auto invalid_alignment = arena.try_allocate(worker, 1, 3);
  if (!check(!failed.has_value() && !wrong_worker.has_value() && !invalid_alignment.has_value() &&
             failed.error().code() == ErrorCode::exhaustion &&
             wrong_worker.error().code() == ErrorCode::invalid_state &&
             invalid_alignment.error().code() == ErrorCode::invalid_input &&
             arena.capacity() == 0 && arena.used() == 0 && *budget.charged(worker) == 0 &&
             *root.charged(worker) == 0 && budget.ready_for_destruction(worker).has_value())) {
    return false;
  }

  BoundedArena invalid{worker, budget, ArenaBlockSource{}, 1, 1};
  const auto invalid_source = invalid.try_allocate(worker, 1, 1);
  return check(!invalid_source.has_value() && invalid_source.error().code() == ErrorCode::invalid_input);
}

[[nodiscard]] bool check_reset_invalidates_views_and_skips_destructors(WorkerId worker) noexcept {
  FixedBlockSource source{};
  MemoryBudget budget{worker, 64};
  BoundedArena arena{worker, budget, fixed_source(source), 8, 64};
  const auto allocation = arena.try_allocate(worker, sizeof(DestructionProbe), alignof(DestructionProbe));
  if (!check(allocation.has_value())) {
    return false;
  }
  const auto bytes = allocation->bytes();
  if (!check(bytes.has_value())) {
    return false;
  }
  auto* const probe = std::construct_at(reinterpret_cast<DestructionProbe*>(bytes->data()));
  const auto boundary = arena.quiescent_boundary(worker);
  if (!check(boundary.has_value() && arena.reset(worker, *boundary).has_value() &&
             DestructionProbe::destructions == 0)) {
    return false;
  }
  const auto stale = allocation->bytes();
  std::destroy_at(probe);
  return check(!stale.has_value() && stale.error().code() == ErrorCode::invalid_state &&
               DestructionProbe::destructions == 1);
}

[[nodiscard]] bool check_reset_failures_preserve_state(WorkerId worker) noexcept {
  FixedBlockSource source{};
  MemoryBudget budget{worker, 32};
  BoundedArena arena{worker, budget, fixed_source(source), 8, 32};
  const auto allocation = arena.try_allocate(worker, 4, 4);
  const auto boundary = arena.quiescent_boundary(worker);
  source.fail_reset = true;
  const auto failed_reset = arena.reset(worker, *boundary);
  if (!check(allocation.has_value() && boundary.has_value() && !failed_reset.has_value() &&
             failed_reset.error().code() == ErrorCode::io && arena.capacity() == 8 &&
             arena.used() == 4 && arena.generation() == 1 && *budget.charged(worker) == 8)) {
    return false;
  }
  source.fail_reset = false;
  return check(arena.reset(worker, *boundary).has_value() && *budget.charged(worker) == 0);
}

[[nodiscard]] bool check_pmr_success(WorkerId worker) noexcept {
  FixedBlockSource source{};
  MemoryBudget budget{worker, 32};
  BoundedArena arena{worker, budget, fixed_source(source), 8, 32};
  ArenaPmrResource resource{arena, worker};
  std::pmr::polymorphic_allocator<std::byte> allocator{&resource};
  std::byte* const allocation = allocator.allocate(8);
  if (!check(allocation != nullptr && reinterpret_cast<std::uintptr_t>(allocation) % alignof(std::byte) == 0 &&
             arena.used() == 8 && *budget.charged(worker) == 8)) {
    return false;
  }
  allocator.deallocate(allocation, 8);
  const auto boundary = arena.quiescent_boundary(worker);
  return check(boundary.has_value() && arena.reset(worker, *boundary).has_value());
}

[[nodiscard]] bool child_terminates_for_pmr_exhaustion(WorkerId worker) noexcept {
  const pid_t process = ::fork();
  if (process == 0) {
    FixedBlockSource source{};
    MemoryBudget budget{worker, 8};
    BoundedArena arena{worker, budget, fixed_source(source), 8, 8};
    ArenaPmrResource resource{arena, worker};
    (void)resource.allocate(9, 1);
    _exit(0);
  }
  if (process < 0) {
    return false;
  }
  int status{};
  return ::waitpid(process, &status, 0) == process && !WIFEXITED(status);
}

[[nodiscard]] bool child_terminates_for_pmr_misuse(WorkerId worker) noexcept {
  const pid_t process = ::fork();
  if (process == 0) {
    FixedBlockSource source{};
    MemoryBudget budget{worker, 8};
    BoundedArena arena{worker, budget, fixed_source(source), 8, 8};
    ArenaPmrResource resource{arena, worker};
    std::size_t invalid_alignment = 4;
    if (::getpid() != 0) {
      invalid_alignment = 3;
    }
    (void)resource.allocate(1, invalid_alignment);
    _exit(0);
  }
  if (process < 0) {
    return false;
  }
  int status{};
  return ::waitpid(process, &status, 0) == process && !WIFEXITED(status);
}

static_assert(!std::is_copy_constructible_v<BoundedArena>);
static_assert(!std::is_move_constructible_v<BoundedArena>);
static_assert(laghu::core::is_borrowed_view_v<laghu::core::ArenaView>);

}  // namespace

int main() {
  const auto worker = make_worker(1);
  const auto other_worker = make_worker(2);
  if (!check(worker.has_value() && other_worker.has_value())) {
    return 1;
  }
  const std::size_t allocation_attempts_before = allocation_attempts;
  if (!check(check_growth_alignment_and_budget(*worker))) {
    return 2;
  }
  if (!check(check_failures_and_rollback(*worker, *other_worker))) {
    return 3;
  }
  if (!check(check_reset_invalidates_views_and_skips_destructors(*worker))) {
    return 4;
  }
  if (!check(check_reset_failures_preserve_state(*worker))) {
    return 5;
  }
  if (!check(check_pmr_success(*worker))) {
    return 6;
  }
  if (!check(child_terminates_for_pmr_exhaustion(*worker) &&
             child_terminates_for_pmr_misuse(*worker))) {
    return 7;
  }
  return allocation_attempts == allocation_attempts_before ? 0 : 8;
}
