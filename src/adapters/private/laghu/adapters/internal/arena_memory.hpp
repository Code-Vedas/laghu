// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstring>
#include <limits>
#include <new>

#include <laghu/core/bounded_arena.hpp>

namespace laghu::adapters::internal {

struct alignas(std::max_align_t) ArenaAllocation final {
  std::size_t capacity{};
  std::size_t used{};
  ArenaAllocation* next{};
};

struct ArenaMemory final {
  core::BoundedArena* arena{};
  core::WorkerId worker;
  ArenaAllocation* free_list{};
};

inline void* arena_malloc(std::size_t requested, void* context) noexcept {
  auto& memory = *static_cast<ArenaMemory*>(context);
  const std::size_t size = requested == 0U ? 1U : requested;
  ArenaAllocation** link = &memory.free_list;
  while (*link != nullptr && (*link)->capacity < size) {
    link = &(*link)->next;
  }
  if (*link != nullptr) {
    ArenaAllocation* const allocation = *link;
    *link = allocation->next;
    allocation->used = size;
    allocation->next = nullptr;
    return allocation + 1;
  }
  if (size > std::numeric_limits<std::size_t>::max() - sizeof(ArenaAllocation)) {
    return nullptr;
  }
  const auto allocation = memory.arena->try_allocate(
      memory.worker, sizeof(ArenaAllocation) + size, alignof(std::max_align_t));
  if (!allocation.has_value()) return nullptr;
  const auto bytes = allocation->bytes();
  if (!bytes.has_value()) return nullptr;
  auto* const header = ::new (bytes->data()) ArenaAllocation{size, size, nullptr};
  return header + 1;
}

inline void arena_free(void* pointer, void* context) noexcept {
  if (pointer == nullptr) return;
  auto& memory = *static_cast<ArenaMemory*>(context);
  auto* const allocation = static_cast<ArenaAllocation*>(pointer) - 1;
  allocation->next = memory.free_list;
  memory.free_list = allocation;
}

inline void* arena_calloc(std::size_t count, std::size_t size, void* context) noexcept {
  if (count != 0U && size > std::numeric_limits<std::size_t>::max() / count) return nullptr;
  const std::size_t total = count * size;
  void* const result = arena_malloc(total, context);
  if (result != nullptr) std::memset(result, 0, total);
  return result;
}

inline void* arena_realloc(void* pointer, std::size_t size, void* context) noexcept {
  if (pointer == nullptr) return arena_malloc(size, context);
  if (size == 0U) {
    arena_free(pointer, context);
    return nullptr;
  }
  auto* const old = static_cast<ArenaAllocation*>(pointer) - 1;
  if (size <= old->capacity) {
    old->used = size;
    return pointer;
  }
  void* const result = arena_malloc(size, context);
  if (result == nullptr) return nullptr;
  std::memcpy(result, pointer, old->used < size ? old->used : size);
  arena_free(pointer, context);
  return result;
}

}  // namespace laghu::adapters::internal
