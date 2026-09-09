// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

#include <laghu/core/checked_arithmetic.hpp>
#include <laghu/core/identifiers.hpp>

namespace laghu::core {

struct SlabPoolDebugHooks final {
  void* context{};
  void (*double_release)(void* context) noexcept{};
};

template <class T>
struct SlabPoolSlot final {
  static_assert(!std::is_const_v<T>);

  union Object final {
    constexpr Object() noexcept : vacant{} {}
    ~Object() {}

    std::byte vacant;
    T value;
  } object;
  std::size_t next{};
  bool occupied{};
};

template <class T>
class SlabPool final {
  static_assert(!std::is_const_v<T>);
  static_assert(std::is_nothrow_destructible_v<T>);

 public:
  class Lease final {
   public:
    constexpr Lease() noexcept = default;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    Lease(Lease&& other) noexcept { move_from(std::move(other)); }

    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        release_unchecked();
        move_from(std::move(other));
      }
      return *this;
    }

    ~Lease() { release_unchecked(); }

    [[nodiscard]] constexpr bool is_active() const noexcept { return active_; }
    [[nodiscard]] constexpr T* get() const noexcept { return value_; }
    [[nodiscard]] constexpr T& operator*() const noexcept { return *value_; }
    [[nodiscard]] constexpr T* operator->() const noexcept { return value_; }

    [[nodiscard]] Result<void> release(WorkerId worker) noexcept {
      if (pool_ == nullptr) {
        return std::unexpected{inactive_lease_error()};
      }
      if (!active_) {
        pool_->report_double_release();
        return std::unexpected{inactive_lease_error()};
      }
      return pool_->release(*this, worker);
    }

   private:
    friend class SlabPool;

    constexpr Lease(SlabPool& pool, std::size_t index, T* value) noexcept
        : pool_(&pool), index_(index), value_(value), active_(true) {}

    [[nodiscard]] static constexpr Error inactive_lease_error() noexcept {
      return Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                   "slab pool lease is inactive"};
    }

    void release_unchecked() noexcept {
      if (active_) {
        pool_->release_unchecked(*this);
      }
    }

    void move_from(Lease&& other) noexcept {
      pool_ = other.pool_;
      index_ = other.index_;
      value_ = other.value_;
      active_ = other.active_;
      other.pool_ = nullptr;
      other.index_ = no_slot();
      other.value_ = nullptr;
      other.active_ = false;
    }

    [[nodiscard]] static constexpr std::size_t no_slot() noexcept {
      return std::numeric_limits<std::size_t>::max();
    }

    SlabPool* pool_{};
    std::size_t index_{no_slot()};
    T* value_{};
    bool active_{};
  };

  // The caller owns slots and must retain them, with this pool, until every
  // lease has been released. The selected alignment must describe that storage.
  constexpr SlabPool(WorkerId worker, std::span<SlabPoolSlot<T>> storage, std::size_t capacity,
                     std::size_t alignment, SlabPoolDebugHooks debug_hooks = {}) noexcept
      : worker_(worker), storage_(storage), capacity_(capacity), alignment_(alignment),
        debug_hooks_(debug_hooks) {}

  SlabPool(const SlabPool&) = delete;
  SlabPool& operator=(const SlabPool&) = delete;
  SlabPool(SlabPool&&) = delete;
  SlabPool& operator=(SlabPool&&) = delete;

  ~SlabPool() {
    if (live_leases_ != 0) {
      std::terminate();
    }
  }

  [[nodiscard]] static constexpr std::size_t required_storage_alignment() noexcept {
    return alignof(SlabPoolSlot<T>);
  }

  [[nodiscard]] static constexpr Result<std::size_t> required_storage_bytes(
      std::size_t capacity) noexcept {
    return checked_multiply(capacity, sizeof(SlabPoolSlot<T>));
  }

  [[nodiscard]] constexpr std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] constexpr std::size_t alignment() const noexcept { return alignment_; }
  [[nodiscard]] constexpr std::size_t available() const noexcept {
    return capacity_ - live_leases_;
  }
  [[nodiscard]] constexpr std::size_t live_leases() const noexcept { return live_leases_; }

  template <class... Args>
    requires std::is_nothrow_constructible_v<T, Args&&...>
  [[nodiscard]] Result<Lease> try_acquire(WorkerId worker, Args&&... args) noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    if (const auto ready = initialize(); !ready.has_value()) {
      return std::unexpected{ready.error()};
    }
    if (free_head_ == no_slot()) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                   "slab pool capacity is exhausted"}};
    }

    SlabPoolSlot<T>& slot = storage_[free_head_];
    if (slot.occupied || live_leases_ == capacity_) {
      std::terminate();
    }
    const std::size_t index = free_head_;
    free_head_ = slot.next;
    T* const value = std::construct_at(std::addressof(slot.object.value), std::forward<Args>(args)...);
    slot.occupied = true;
    ++live_leases_;
    return Lease{*this, index, value};
  }

 private:
  [[nodiscard]] static constexpr std::size_t no_slot() noexcept {
    return std::numeric_limits<std::size_t>::max();
  }

  [[nodiscard]] Result<void> require_worker(WorkerId worker) const noexcept {
    if (worker.value() != worker_.value()) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                                   "slab pool accessed by a different worker"}};
    }
    return {};
  }

  [[nodiscard]] Result<void> initialize() noexcept {
    if (initialized_) {
      return {};
    }
    const auto required = required_storage_bytes(capacity_);
    if (!required.has_value()) {
      return std::unexpected{required.error()};
    }
    if (!is_valid_alignment(alignment_) || alignment_ < required_storage_alignment()) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_input, 0,
                                   "slab pool alignment is invalid"}};
    }
    if (capacity_ > 1 && sizeof(SlabPoolSlot<T>) % alignment_ != 0) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_input, 0,
                                   "slab pool slot stride does not satisfy selected alignment"}};
    }
    if (capacity_ == 0) {
      initialized_ = true;
      return {};
    }
    if (storage_.size() < capacity_ || storage_.data() == nullptr ||
        reinterpret_cast<std::uintptr_t>(storage_.data()) % alignment_ != 0) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_input, 0,
                                   "slab pool storage does not satisfy its layout"}};
    }

    for (std::size_t index = 0; index < capacity_; ++index) {
      const std::size_t next = index + 1 == capacity_ ? no_slot() : index + 1;
      storage_[index].next = next;
      storage_[index].occupied = false;
    }
    free_head_ = 0;
    initialized_ = true;
    return {};
  }

  [[nodiscard]] static constexpr bool is_valid_alignment(std::size_t alignment) noexcept {
    return alignment != 0 && (alignment & (alignment - 1)) == 0;
  }

  [[nodiscard]] Result<void> release(Lease& lease, WorkerId worker) noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    release_unchecked(lease);
    return {};
  }

  void release_unchecked(Lease& lease) noexcept {
    if (!initialized_ || lease.index_ >= capacity_ || live_leases_ == 0) {
      std::terminate();
    }
    SlabPoolSlot<T>& slot = storage_[lease.index_];
    if (!slot.occupied) {
      std::terminate();
    }
    std::destroy_at(lease.value_);
    slot.occupied = false;
    slot.next = free_head_;
    free_head_ = lease.index_;
    --live_leases_;
    lease.value_ = nullptr;
    lease.active_ = false;
  }

  void report_double_release() const noexcept {
#ifndef NDEBUG
    if (debug_hooks_.double_release != nullptr) {
      debug_hooks_.double_release(debug_hooks_.context);
    } else {
      std::terminate();
    }
#endif
  }

  WorkerId worker_;
  std::span<SlabPoolSlot<T>> storage_{};
  std::size_t capacity_{};
  std::size_t alignment_{};
  std::size_t free_head_{no_slot()};
  std::size_t live_leases_{};
  SlabPoolDebugHooks debug_hooks_{};
  bool initialized_{};
};

}  // namespace laghu::core
