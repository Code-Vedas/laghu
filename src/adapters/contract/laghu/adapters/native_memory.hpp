// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>

#include <laghu/core/bounded_arena.hpp>

namespace laghu::adapters {

namespace internal {
struct ArenaMemoryAccess;
}

// Caller-owned reusable storage for native dependency allocations. The pool
// must outlive every adapter session that uses it.
class NativeMemoryPool final {
 public:
  constexpr NativeMemoryPool(core::WorkerId worker, core::BoundedArena& arena) noexcept
      : arena_(&arena), worker_(worker), generation_(arena.generation()) {}

  NativeMemoryPool(const NativeMemoryPool&) = delete;
  NativeMemoryPool& operator=(const NativeMemoryPool&) = delete;
  NativeMemoryPool(NativeMemoryPool&&) = delete;
  NativeMemoryPool& operator=(NativeMemoryPool&&) = delete;

 private:
  friend struct internal::ArenaMemoryAccess;
  core::BoundedArena* arena_{};
  core::WorkerId worker_;
  std::uint64_t generation_{};
  void* free_list_{};
};

}  // namespace laghu::adapters
