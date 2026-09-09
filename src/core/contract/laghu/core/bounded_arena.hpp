// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory_resource>
#include <type_traits>

#include <laghu/core/memory_budget.hpp>
#include <laghu/core/views.hpp>

namespace laghu::core {

class BoundedArena;

class ArenaView final {
 public:
  constexpr ArenaView() noexcept = default;

  [[nodiscard]] constexpr std::size_t size() const noexcept { return bytes_.size(); }
  [[nodiscard]] constexpr bool empty() const noexcept { return bytes_.empty(); }
  [[nodiscard]] constexpr std::uint64_t generation() const noexcept { return generation_; }
  [[nodiscard]] Result<MutableByteView> bytes() const noexcept;

 private:
  friend class BoundedArena;

  constexpr ArenaView(const BoundedArena& arena, MutableByteView bytes,
                      std::uint64_t generation) noexcept
      : arena_(&arena), bytes_(bytes), generation_(generation) {}

  const BoundedArena* arena_{};
  MutableByteView bytes_{};
  std::uint64_t generation_{};
};

struct ArenaBlockSource final {
  using Acquire = Result<MutableByteView> (*)(void* context, std::size_t minimum_capacity) noexcept;
  using Reset = Result<void> (*)(void* context) noexcept;

  void* context{};
  Acquire acquire{};
  Reset reset{};
};

class ArenaQuiescentBoundary final {
 public:
  ArenaQuiescentBoundary(const ArenaQuiescentBoundary&) = delete;
  ArenaQuiescentBoundary& operator=(const ArenaQuiescentBoundary&) = delete;
  ArenaQuiescentBoundary(ArenaQuiescentBoundary&&) noexcept = default;
  ArenaQuiescentBoundary& operator=(ArenaQuiescentBoundary&&) noexcept = delete;

 private:
  friend class BoundedArena;

  constexpr ArenaQuiescentBoundary(const BoundedArena& arena, std::uint64_t generation) noexcept
      : arena_(&arena), generation_(generation) {}

  const BoundedArena* arena_{};
  std::uint64_t generation_{};
};

class ArenaPmrResource;

class BoundedArena final {
 public:
  constexpr BoundedArena(WorkerId worker, MemoryBudget& budget, ArenaBlockSource source,
                         std::size_t initial_capacity, std::size_t maximum_capacity) noexcept
      : worker_(worker), budget_(&budget), source_(source), initial_capacity_(initial_capacity),
        maximum_capacity_(maximum_capacity) {}

  BoundedArena(const BoundedArena&) = delete;
  BoundedArena& operator=(const BoundedArena&) = delete;
  BoundedArena(BoundedArena&&) = delete;
  BoundedArena& operator=(BoundedArena&&) = delete;

  ~BoundedArena() {
    if (reservation_.is_active()) {
      std::terminate();
    }
  }

  [[nodiscard]] constexpr std::size_t initial_capacity() const noexcept { return initial_capacity_; }
  [[nodiscard]] constexpr std::size_t maximum_capacity() const noexcept { return maximum_capacity_; }
  [[nodiscard]] constexpr std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] constexpr std::size_t used() const noexcept { return offset_; }
  [[nodiscard]] constexpr std::uint64_t generation() const noexcept { return generation_; }

  [[nodiscard]] Result<ArenaView> try_allocate(WorkerId worker, std::size_t bytes,
                                                 std::size_t alignment) noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    if (!is_valid_alignment(alignment)) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_input, 0,
                                   "bounded arena alignment must be a nonzero power of two"}};
    }
    if (bytes == 0) {
      return ArenaView{*this, MutableByteView{}, generation_};
    }

    const std::size_t remainder = offset_ % alignment;
    const std::size_t padding = remainder == 0 ? 0 : alignment - remainder;
    const auto aligned_offset = checked_add(offset_, padding);
    if (!aligned_offset.has_value()) {
      return std::unexpected{aligned_offset.error()};
    }
    const auto required = checked_add(*aligned_offset, bytes);
    if (!required.has_value()) {
      return std::unexpected{required.error()};
    }
    if (*required > maximum_capacity_) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                   "bounded arena maximum capacity is exhausted"}};
    }
    if (const auto capacity = ensure_capacity(worker, *required); !capacity.has_value()) {
      return std::unexpected{capacity.error()};
    }

    const auto allocation = MutableByteView::from(storage_.span().subspan(*aligned_offset, bytes));
    if (!allocation.has_value()) {
      std::terminate();
    }
    offset_ = *required;
    return ArenaView{*this, *allocation, generation_};
  }

  [[nodiscard]] Result<ArenaQuiescentBoundary> quiescent_boundary(WorkerId worker) const noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    return ArenaQuiescentBoundary{*this, generation_};
  }

  [[nodiscard]] Result<void> reset(WorkerId worker, const ArenaQuiescentBoundary& boundary) noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    if (boundary.arena_ != this || boundary.generation_ != generation_) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                                   "bounded arena reset requires its current quiescent boundary"}};
    }
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::overflow, 0,
                                   "bounded arena generation is exhausted"}};
    }
    if (source_.reset == nullptr) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_input, 0,
                                   "bounded arena has an invalid block source"}};
    }
    if (const auto source_reset = source_.reset(source_.context); !source_reset.has_value()) {
      return std::unexpected{source_reset.error()};
    }

    (void)reservation_.release(worker);
    storage_ = MutableByteView{};
    capacity_ = 0;
    offset_ = 0;
    ++generation_;
    return {};
  }

 private:
  friend class ArenaView;
  friend class ArenaPmrResource;

  [[nodiscard]] static constexpr bool is_valid_alignment(std::size_t alignment) noexcept {
    return alignment != 0 && (alignment & (alignment - 1)) == 0;
  }

  [[nodiscard]] Result<void> require_worker(WorkerId worker) const noexcept {
    if (worker.value() != worker_.value()) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                                   "bounded arena accessed by a different worker"}};
    }
    return {};
  }

  [[nodiscard]] Result<void> ensure_capacity(WorkerId worker, std::size_t required) noexcept {
    if (source_.acquire == nullptr || source_.reset == nullptr || initial_capacity_ == 0 ||
        initial_capacity_ > maximum_capacity_) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_input, 0,
                                   "bounded arena has an invalid block source or capacity"}};
    }
    if (required <= capacity_) {
      return {};
    }
    std::size_t target = capacity_ == 0 ? initial_capacity_ : capacity_;
    while (target < required) {
      if (target > maximum_capacity_ - target) {
        target = maximum_capacity_;
      } else {
        target *= 2;
      }
    }
    if (target > maximum_capacity_) {
      target = maximum_capacity_;
    }

    if (!reservation_.is_active()) {
      MutableByteView acquired;
      auto reservation = budget_->reserve_for_allocation(worker, target, [&]() noexcept -> Result<void> {
        auto source_block = source_.acquire(source_.context, maximum_capacity_);
        if (!source_block.has_value()) {
          return std::unexpected{source_block.error()};
        }
        if (source_block->size() < maximum_capacity_) {
          return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                       "bounded arena block source is smaller than the maximum"}};
        }
        acquired = *source_block;
        return {};
      });
      if (!reservation.has_value()) {
        return std::unexpected{reservation.error()};
      }
      storage_ = acquired;
      reservation_ = std::move(*reservation);
      capacity_ = target;
      return {};
    }

    const auto expanded = reservation_.grow(worker, target - capacity_);
    if (!expanded.has_value()) {
      return std::unexpected{expanded.error()};
    }
    capacity_ = target;
    return {};
  }

  [[nodiscard]] Result<MutableByteView> view_for(MutableByteView bytes,
                                                   std::uint64_t generation) const noexcept {
    if (generation != generation_) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                                   "bounded arena view was invalidated by reset"}};
    }
    return bytes;
  }

  WorkerId worker_;
  MemoryBudget* budget_{};
  ArenaBlockSource source_{};
  MutableByteView storage_{};
  MemoryReservation reservation_{};
  std::size_t initial_capacity_{};
  std::size_t maximum_capacity_{};
  std::size_t capacity_{};
  std::size_t offset_{};
  std::uint64_t generation_{1};
};

class ArenaPmrResource final : public std::pmr::memory_resource {
 public:
  constexpr ArenaPmrResource(BoundedArena& arena, WorkerId worker) noexcept
      : arena_(&arena), worker_(worker) {}

 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    const auto allocation = arena_->try_allocate(worker_, bytes == 0 ? 1 : bytes, alignment);
    if (!allocation.has_value()) {
      std::terminate();
    }
    const auto bytes_view = allocation->bytes();
    if (!bytes_view.has_value()) {
      std::terminate();
    }
    return bytes_view->data();
  }

  void do_deallocate(void*, std::size_t, std::size_t) override {}

  [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

  BoundedArena* arena_{};
  WorkerId worker_;
};

inline Result<MutableByteView> ArenaView::bytes() const noexcept {
  if (arena_ == nullptr) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                                 "bounded arena view is empty"}};
  }
  return arena_->view_for(bytes_, generation_);
}

template <>
struct is_borrowed_view<ArenaView> : std::true_type {};

static_assert(!std::is_copy_constructible_v<BoundedArena>);
static_assert(!std::is_move_constructible_v<BoundedArena>);
static_assert(!std::is_copy_constructible_v<ArenaQuiescentBoundary>);
static_assert(std::is_nothrow_move_constructible_v<ArenaQuiescentBoundary>);
static_assert(is_borrowed_view_v<ArenaView>);

}  // namespace laghu::core
