// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>

#include <laghu/core/contract.hpp>

namespace laghu::core {

using MonotonicInstant = std::uint64_t;
using RealtimeInstant = std::int64_t;

using MonotonicNowFunction = Result<MonotonicInstant> (*)(void* context) noexcept;
using RealtimeNowFunction = Result<RealtimeInstant> (*)(void* context) noexcept;

// A caller owns the context and every function referenced by this table.
// Monotonic time is used for expiry; realtime is for externally visible time.
struct ClockOperations final {
  void* context{};
  MonotonicNowFunction monotonic_now{};
  RealtimeNowFunction realtime_now{};
};

[[nodiscard]] Result<MonotonicInstant> read_monotonic_clock(
    const ClockOperations& operations) noexcept;
[[nodiscard]] Result<RealtimeInstant> read_realtime_clock(
    const ClockOperations& operations) noexcept;

// Returns a non-owning reference to Laghu's process-static POSIX clock table.
// It remains valid for the process lifetime and exposes no POSIX types.
[[nodiscard]] const ClockOperations& system_clock_operations() noexcept;

}  // namespace laghu::core
