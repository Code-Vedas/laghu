// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <type_traits>

#include <laghu/core/bounded_buffer.hpp>

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

using laghu::core::BoundedBuffer;
using laghu::core::BufferBlockSource;
using laghu::core::ByteView;
using laghu::core::Error;
using laghu::core::ErrorCode;
using laghu::core::ErrorDomain;
using laghu::core::MemoryBudget;
using laghu::core::MutableByteView;
using laghu::core::Result;
using laghu::core::WorkerId;

struct FixedBlockSource final {
  alignas(64) std::array<std::array<std::byte, 32>, 4> blocks{};
  std::size_t acquire_calls{};
  std::size_t release_calls{};
  std::size_t next_block{};
  bool fail_acquire{};
  bool undersized{};
};

[[nodiscard]] constexpr bool check(bool condition) noexcept { return condition; }

[[nodiscard]] Result<WorkerId> make_worker(std::uint64_t value) noexcept {
  return WorkerId::from_uint64(value);
}

[[nodiscard]] Result<MutableByteView> acquire_block(void* context,
                                                     std::size_t minimum_capacity) noexcept {
  auto* const source = static_cast<FixedBlockSource*>(context);
  ++source->acquire_calls;
  if (source->fail_acquire) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                 "injected bounded buffer acquisition failure"}};
  }
  if (source->next_block == source->blocks.size()) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                 "fixed bounded buffer source is exhausted"}};
  }
  auto& block = source->blocks[source->next_block++];
  const std::size_t provided = source->undersized && minimum_capacity != 0
                                   ? minimum_capacity - 1
                                   : block.size();
  return MutableByteView::from(std::span<std::byte>{block}.first(provided));
}

void release_block(void* context, MutableByteView) noexcept {
  ++static_cast<FixedBlockSource*>(context)->release_calls;
}

[[nodiscard]] BufferBlockSource block_source(FixedBlockSource& source) noexcept {
  return BufferBlockSource{&source, acquire_block, release_block};
}

[[nodiscard]] bool has_bytes(ByteView view, std::initializer_list<unsigned int> expected) noexcept {
  if (view.size() != expected.size()) {
    return false;
  }
  std::size_t index = 0;
  for (const unsigned int value : expected) {
    if (std::to_integer<unsigned int>(view.span()[index]) != value) {
      return false;
    }
    ++index;
  }
  return true;
}

void fill(MutableByteView view, unsigned int first) noexcept {
  for (std::size_t index = 0; index < view.size(); ++index) {
    view.span()[index] = static_cast<std::byte>(first + index);
  }
}

[[nodiscard]] bool check_growth_compaction_and_release(WorkerId worker) noexcept {
  FixedBlockSource source{};
  MemoryBudget root{worker, 16};
  MemoryBudget budget{root, 16};
  BoundedBuffer buffer{worker, budget, block_source(source), 4, 16};

  const auto first = buffer.reserve(worker, 3);
  if (!check(first.has_value() && first->size() == 3 && source.acquire_calls == 1 &&
             buffer.capacity() == 4 && *budget.charged(worker) == 4 &&
             *root.charged(worker) == 4)) {
    return false;
  }
  fill(*first, 1);
  if (!check(buffer.commit(worker, 3).has_value() && buffer.consume(worker, 2).has_value())) {
    return false;
  }

  const auto compacted = buffer.reserve(worker, 3);
  if (!check(compacted.has_value() && source.acquire_calls == 1 && buffer.capacity() == 4 &&
             buffer.size() == 1 && has_bytes(*buffer.readable(worker), {3}))) {
    return false;
  }
  fill(*compacted, 4);
  if (!check(buffer.commit(worker, 3).has_value() &&
             has_bytes(*buffer.readable(worker), {3, 4, 5, 6}))) {
    return false;
  }

  if (!check(buffer.consume(worker, 1).has_value())) {
    return false;
  }
  const auto grown = buffer.reserve(worker, 5);
  if (!check(grown.has_value() && source.acquire_calls == 2 && source.release_calls == 1 &&
             buffer.capacity() == 8 && *budget.charged(worker) == 8 && *root.charged(worker) == 8 &&
             has_bytes(*buffer.readable(worker), {4, 5, 6}))) {
    return false;
  }
  fill(*grown, 7);
  if (!check(buffer.commit(worker, 5).has_value() &&
             has_bytes(*buffer.readable(worker), {4, 5, 6, 7, 8, 9, 10, 11}))) {
    return false;
  }

  const auto hard_maximum = buffer.reserve(worker, 9);
  if (!check(!hard_maximum.has_value() && hard_maximum.error().code() == ErrorCode::exhaustion &&
             source.acquire_calls == 2)) {
    return false;
  }
  if (!check(buffer.release(worker).has_value() && source.release_calls == 2 &&
             buffer.capacity() == 0 && buffer.size() == 0 && *budget.charged(worker) == 0 &&
             *root.charged(worker) == 0)) {
    return false;
  }
  return check(budget.ready_for_destruction(worker).has_value());
}

[[nodiscard]] bool check_failures_and_boundaries(WorkerId worker, WorkerId other_worker) noexcept {
  FixedBlockSource failed_source{};
  failed_source.fail_acquire = true;
  MemoryBudget root{worker, 16};
  MemoryBudget budget{root, 16};
  BoundedBuffer failed{worker, budget, block_source(failed_source), 4, 16};
  const auto acquire_failure = failed.reserve(worker, 1);
  if (!check(!acquire_failure.has_value() && acquire_failure.error().code() == ErrorCode::exhaustion &&
             *budget.charged(worker) == 0 && *root.charged(worker) == 0)) {
    return false;
  }

  FixedBlockSource small_source{};
  small_source.undersized = true;
  BoundedBuffer undersized{worker, budget, block_source(small_source), 4, 16};
  const auto undersized_failure = undersized.reserve(worker, 1);
  if (!check(!undersized_failure.has_value() && undersized_failure.error().code() == ErrorCode::exhaustion &&
             small_source.release_calls == 1 && *budget.charged(worker) == 0)) {
    return false;
  }

  FixedBlockSource source{};
  BoundedBuffer buffer{worker, budget, block_source(source), 4, 8};
  const auto wrong_worker = buffer.reserve(other_worker, 1);
  const auto zero = buffer.reserve(worker, 0);
  const auto invalid_commit = buffer.commit(worker, 1);
  const auto invalid_consume = buffer.consume(worker, 1);
  const auto first = buffer.reserve(worker, 4);
  if (!check(!wrong_worker.has_value() && wrong_worker.error().code() == ErrorCode::invalid_state &&
             zero.has_value() && zero->empty() && !invalid_commit.has_value() &&
             !invalid_consume.has_value() && first.has_value() && buffer.commit(worker, 4).has_value())) {
    return false;
  }
  const auto writable = buffer.writable(worker);
  const auto readable = buffer.readable(worker);
  if (!check(writable.has_value() && writable->empty() && readable.has_value() && readable->size() == 4 &&
             !buffer.commit(worker, 1).has_value() && !buffer.consume(worker, 5).has_value())) {
    return false;
  }
  source.fail_acquire = true;
  const auto growth_failure = buffer.reserve(worker, 5);
  source.fail_acquire = false;
  if (!check(!growth_failure.has_value() && growth_failure.error().code() == ErrorCode::exhaustion &&
             buffer.capacity() == 4 && *budget.charged(worker) == 4 &&
             has_bytes(*buffer.readable(worker), {0, 0, 0, 0}))) {
    return false;
  }
  const auto overflow = buffer.reserve(worker, std::numeric_limits<std::size_t>::max());
  return check(!overflow.has_value() && overflow.error().code() == ErrorCode::exhaustion &&
               buffer.release(worker).has_value() && *budget.charged(worker) == 0);
}

[[nodiscard]] bool check_budget_exhaustion(WorkerId worker) noexcept {
  FixedBlockSource source{};
  MemoryBudget budget{worker, 3};
  BoundedBuffer buffer{worker, budget, block_source(source), 4, 8};
  const auto result = buffer.reserve(worker, 1);
  return check(!result.has_value() && result.error().code() == ErrorCode::exhaustion &&
               source.acquire_calls == 0 && *budget.charged(worker) == 0 &&
               budget.ready_for_destruction(worker).has_value());
}

static_assert(!std::is_copy_constructible_v<BoundedBuffer>);
static_assert(!std::is_move_constructible_v<BoundedBuffer>);

}  // namespace

int main() {
  const auto worker = make_worker(1);
  const auto other_worker = make_worker(2);
  if (!check(worker.has_value() && other_worker.has_value())) {
    return 1;
  }
  const std::size_t allocation_attempts_before = allocation_attempts;
  if (!check(check_growth_compaction_and_release(*worker))) {
    return 2;
  }
  if (!check(check_failures_and_boundaries(*worker, *other_worker))) {
    return 3;
  }
  if (!check(check_budget_exhaustion(*worker))) {
    return 4;
  }
  return allocation_attempts == allocation_attempts_before ? 0 : 5;
}
