// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <laghu/core/clocks.hpp>

namespace laghu::core::internal {

[[nodiscard]] Result<RealtimeInstant> realtime_instant_from_parts(
    RealtimeInstant seconds, RealtimeInstant nanoseconds) noexcept;

}  // namespace laghu::core::internal
