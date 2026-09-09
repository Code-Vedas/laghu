// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/bounded_buffer.hpp>

void reject_copy(laghu::core::BoundedBuffer& buffer) {
  laghu::core::BoundedBuffer copy{buffer};
}
