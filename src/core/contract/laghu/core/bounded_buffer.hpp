// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <exception>
#include <limits>
#include <span>
#include <type_traits>

#include <laghu/core/memory_budget.hpp>
#include <laghu/core/views.hpp>

namespace laghu::core {

struct BufferBlockSource final {
  using Acquire = Result<MutableByteView> (*)(void* context, std::size_t minimum_capacity) noexcept;
  using Release = void (*)(void* context, MutableByteView block) noexcept;

  void* context{};
  Acquire acquire{};
  Release release{};
};

class BoundedBuffer final {
 public:
  constexpr BoundedBuffer(WorkerId worker, MemoryBudget& budget, BufferBlockSource source,
                          std::size_t initial_capacity, std::size_t maximum_capacity) noexcept
      : worker_(worker), budget_(&budget), source_(source), initial_capacity_(initial_capacity),
        maximum_capacity_(maximum_capacity) {}

  BoundedBuffer(const BoundedBuffer&) = delete;
  BoundedBuffer& operator=(const BoundedBuffer&) = delete;
  BoundedBuffer(BoundedBuffer&&) = delete;
  BoundedBuffer& operator=(BoundedBuffer&&) = delete;

  ~BoundedBuffer() { release_unchecked(); }

  [[nodiscard]] constexpr std::size_t initial_capacity() const noexcept { return initial_capacity_; }
  [[nodiscard]] constexpr std::size_t maximum_capacity() const noexcept { return maximum_capacity_; }
  [[nodiscard]] constexpr std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return write_offset_ - read_offset_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size() == 0; }

  [[nodiscard]] Result<ByteView> readable(WorkerId worker) const noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    return ByteView::from(storage_.span().subspan(read_offset_, size()));
  }

  [[nodiscard]] Result<MutableByteView> writable(WorkerId worker) noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    return MutableByteView::from(storage_.span().subspan(write_offset_, capacity_ - write_offset_));
  }

  [[nodiscard]] Result<MutableByteView> reserve(WorkerId worker,
                                                  std::size_t minimum_writable) noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    if (minimum_writable == 0) {
      return MutableByteView{};
    }
    if (minimum_writable > maximum_capacity_ - size()) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                   "bounded buffer maximum capacity is exhausted"}};
    }
    if (minimum_writable > capacity_ - write_offset_) {
      const auto required = checked_add(size(), minimum_writable);
      if (!required.has_value()) {
        return std::unexpected{required.error()};
      }
      if (*required <= capacity_) {
        compact_unchecked();
      } else if (const auto grown = grow(worker, *required); !grown.has_value()) {
        return std::unexpected{grown.error()};
      }
    }
    const auto region = MutableByteView::from(
        storage_.span().subspan(write_offset_, minimum_writable));
    if (!region.has_value()) {
      std::terminate();
    }
    return *region;
  }

  [[nodiscard]] Result<void> commit(WorkerId worker, std::size_t bytes) noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    if (bytes > capacity_ - write_offset_) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_range, 0,
                                   "bounded buffer commit exceeds writable region"}};
    }
    const auto next = checked_add(write_offset_, bytes);
    if (!next.has_value()) {
      return std::unexpected{next.error()};
    }
    write_offset_ = *next;
    return {};
  }

  [[nodiscard]] Result<void> consume(WorkerId worker, std::size_t bytes) noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    if (bytes > size()) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_range, 0,
                                   "bounded buffer consume exceeds readable region"}};
    }
    const auto next = checked_add(read_offset_, bytes);
    if (!next.has_value()) {
      return std::unexpected{next.error()};
    }
    read_offset_ = *next;
    if (read_offset_ == write_offset_) {
      read_offset_ = 0;
      write_offset_ = 0;
    }
    return {};
  }

  [[nodiscard]] Result<void> compact(WorkerId worker) noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    compact_unchecked();
    return {};
  }

  [[nodiscard]] Result<void> release(WorkerId worker) noexcept {
    if (const auto owner = require_worker(worker); !owner.has_value()) {
      return std::unexpected{owner.error()};
    }
    release_unchecked();
    return {};
  }

 private:
  [[nodiscard]] Result<void> require_worker(WorkerId worker) const noexcept {
    if (worker.value() != worker_.value()) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                                   "bounded buffer accessed by a different worker"}};
    }
    return {};
  }

  [[nodiscard]] Result<void> validate_source() const noexcept {
    if (budget_ == nullptr || source_.acquire == nullptr || source_.release == nullptr ||
        initial_capacity_ == 0 || initial_capacity_ > maximum_capacity_) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_input, 0,
                                   "bounded buffer has an invalid block source or capacity"}};
    }
    return {};
  }

  [[nodiscard]] Result<std::size_t> next_capacity(std::size_t required) const noexcept {
    std::size_t target = capacity_ == 0 ? initial_capacity_ : capacity_;
    while (target < required) {
      if (target > maximum_capacity_ - target) {
        target = maximum_capacity_;
      } else {
        target *= 2;
      }
    }
    return target;
  }

  [[nodiscard]] Result<void> grow(WorkerId worker, std::size_t required) noexcept {
    if (const auto valid = validate_source(); !valid.has_value()) {
      return std::unexpected{valid.error()};
    }
    const auto target = next_capacity(required);
    if (!target.has_value()) {
      return std::unexpected{target.error()};
    }
    if (*target > maximum_capacity_) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                   "bounded buffer maximum capacity is exhausted"}};
    }

    MutableByteView acquired;
    if (!reservation_.is_active()) {
      auto reservation = budget_->reserve_for_allocation(worker, *target, [&]() noexcept -> Result<void> {
        auto block = source_.acquire(source_.context, *target);
        if (!block.has_value()) {
          return std::unexpected{block.error()};
        }
        if (block->size() < *target) {
          source_.release(source_.context, *block);
          return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                       "bounded buffer block source is too small"}};
        }
        acquired = *block;
        return {};
      });
      if (!reservation.has_value()) {
        return std::unexpected{reservation.error()};
      }
      const auto storage = acquired.slice(0, *target);
      if (!storage.has_value()) {
        source_.release(source_.context, acquired);
        return std::unexpected{storage.error()};
      }
      block_ = acquired;
      storage_ = *storage;
      reservation_ = std::move(*reservation);
      capacity_ = *target;
      return {};
    }

    const std::size_t additional = *target - capacity_;
    if (const auto charged = reservation_.grow(worker, additional); !charged.has_value()) {
      return std::unexpected{charged.error()};
    }
    auto block = source_.acquire(source_.context, *target);
    if (!block.has_value() || block->size() < *target) {
      if (block.has_value()) {
        source_.release(source_.context, *block);
      }
      if (const auto rollback = reservation_.shrink(worker, additional); !rollback.has_value()) {
        std::terminate();
      }
      if (!block.has_value()) {
        return std::unexpected{block.error()};
      }
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                   "bounded buffer block source is too small"}};
    }
    const auto replacement = block->slice(0, *target);
    if (!replacement.has_value()) {
      source_.release(source_.context, *block);
      if (const auto rollback = reservation_.shrink(worker, additional); !rollback.has_value()) {
        std::terminate();
      }
      return std::unexpected{replacement.error()};
    }

    const std::size_t readable_bytes = size();
    if (readable_bytes != 0) {
      const std::span<const std::byte> input = storage_.span().subspan(read_offset_, readable_bytes);
      std::span<std::byte> output = replacement->span();
      for (std::size_t index = 0; index < readable_bytes; ++index) {
        output[index] = input[index];
      }
    }
    source_.release(source_.context, block_);
    block_ = *block;
    storage_ = *replacement;
    capacity_ = *target;
    read_offset_ = 0;
    write_offset_ = readable_bytes;
    return {};
  }

  void compact_unchecked() noexcept {
    if (read_offset_ == 0) {
      return;
    }
    const std::size_t readable_bytes = size();
    if (readable_bytes != 0) {
      std::span<const std::byte> input = storage_.span().subspan(read_offset_, readable_bytes);
      std::span<std::byte> output = storage_.span();
      for (std::size_t index = 0; index < readable_bytes; ++index) {
        output[index] = input[index];
      }
    }
    read_offset_ = 0;
    write_offset_ = readable_bytes;
  }

  void release_unchecked() noexcept {
    if (block_.data() != nullptr) {
      source_.release(source_.context, block_);
    }
    block_ = MutableByteView{};
    storage_ = MutableByteView{};
    capacity_ = 0;
    read_offset_ = 0;
    write_offset_ = 0;
    reservation_ = MemoryReservation{};
  }

  WorkerId worker_;
  MemoryBudget* budget_{};
  BufferBlockSource source_{};
  MutableByteView block_{};
  MutableByteView storage_{};
  MemoryReservation reservation_{};
  std::size_t initial_capacity_{};
  std::size_t maximum_capacity_{};
  std::size_t capacity_{};
  std::size_t read_offset_{};
  std::size_t write_offset_{};
};

class IoSlice final {
 public:
  constexpr IoSlice() noexcept = default;
  explicit constexpr IoSlice(ByteView bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] constexpr std::size_t size() const noexcept { return bytes_.size(); }
  [[nodiscard]] constexpr bool empty() const noexcept { return bytes_.empty(); }
  [[nodiscard]] constexpr ByteView bytes() const noexcept { return bytes_; }

  [[nodiscard]] Result<void> consume(std::size_t bytes) noexcept {
    if (bytes > bytes_.size()) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_range, 0,
                                   "I/O slice consume exceeds readable region"}};
    }
    const auto remaining = bytes_.slice(bytes, bytes_.size() - bytes);
    if (!remaining.has_value()) {
      return std::unexpected{remaining.error()};
    }
    bytes_ = *remaining;
    return {};
  }

 private:
  ByteView bytes_{};
};

class IoSliceList final {
 public:
  constexpr IoSliceList() noexcept = default;
  explicit constexpr IoSliceList(std::span<IoSlice> storage) noexcept : storage_(storage) {}

  [[nodiscard]] constexpr std::size_t capacity() const noexcept { return storage_.size(); }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] constexpr std::size_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] constexpr std::span<const IoSlice> slices() const noexcept {
    return storage_.first(count_);
  }

  [[nodiscard]] Result<void> append(ByteView bytes) noexcept {
    if (bytes.empty()) {
      return {};
    }
    if (count_ == storage_.size()) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                   "I/O slice list capacity is exhausted"}};
    }
    const auto total = checked_add(bytes_, bytes.size());
    if (!total.has_value()) {
      return std::unexpected{total.error()};
    }
    storage_[count_] = IoSlice{bytes};
    ++count_;
    bytes_ = *total;
    return {};
  }

  [[nodiscard]] Result<void> consume(std::size_t bytes) noexcept {
    if (bytes > bytes_) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_range, 0,
                                   "I/O slice list consume exceeds readable regions"}};
    }
    std::size_t remaining = bytes;
    std::size_t removed = 0;
    while (removed < count_ && remaining >= storage_[removed].size()) {
      remaining -= storage_[removed].size();
      ++removed;
    }
    if (remaining != 0) {
      if (const auto consumed = storage_[removed].consume(remaining); !consumed.has_value()) {
        std::terminate();
      }
    }
    if (removed != 0) {
      for (std::size_t index = removed; index < count_; ++index) {
        storage_[index - removed] = storage_[index];
      }
      count_ -= removed;
    }
    bytes_ -= bytes;
    return {};
  }

 private:
  std::span<IoSlice> storage_{};
  std::size_t count_{};
  std::size_t bytes_{};
};

static_assert(!std::is_copy_constructible_v<BoundedBuffer>);
static_assert(!std::is_move_constructible_v<BoundedBuffer>);
static_assert(std::is_trivially_copyable_v<IoSlice>);

}  // namespace laghu::core
