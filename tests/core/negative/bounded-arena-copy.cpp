// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/bounded_arena.hpp>

void reject_bounded_arena_copy(const laghu::core::BoundedArena& original) {
  laghu::core::BoundedArena copy{original};
  (void)copy;
}
