// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/slab_pool.hpp>

struct ThrowingConstructor final {
  ThrowingConstructor() noexcept(false) {}
  ~ThrowingConstructor() noexcept = default;
};

void reject_throwing_slab_value(laghu::core::SlabPool<ThrowingConstructor>& pool,
                                laghu::core::WorkerId worker) {
  (void)pool.try_acquire(worker);
}
