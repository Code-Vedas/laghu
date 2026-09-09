// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/slab_pool.hpp>

void reject_slab_pool_copy(const laghu::core::SlabPool<int>& original) {
  laghu::core::SlabPool<int> copy{original};
  (void)copy;
}
