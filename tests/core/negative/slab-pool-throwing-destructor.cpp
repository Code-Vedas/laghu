// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/slab_pool.hpp>

struct ThrowingDestructor final {
  ThrowingDestructor() noexcept = default;
  ~ThrowingDestructor() noexcept(false) {}
};

static_assert(sizeof(laghu::core::SlabPool<ThrowingDestructor>) > 0);
