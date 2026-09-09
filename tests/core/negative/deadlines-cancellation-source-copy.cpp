// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/deadlines_cancellation.hpp>

void reject_copy(laghu::core::CancellationState& state) {
  laghu::core::CancellationSource source{state};
  laghu::core::CancellationSource copy{source};
}
