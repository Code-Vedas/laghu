// SPDX-License-Identifier: AGPL-3.0-only
#include <cerrno>
#include <cstdint>
#include <limits>

#include <time.h>

#include <laghu/core/clocks.hpp>
#include <laghu/core/internal/clock_operations.hpp>

namespace laghu::core {
namespace {

inline constexpr std::int64_t nanoseconds_per_second = 1000000000;

[[nodiscard]] Result<MonotonicInstant> posix_monotonic_now(void*) noexcept {
  timespec value{};
  if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
    return std::unexpected{Error::from_errno(errno, "CLOCK_MONOTONIC query failed")};
  }
  if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= nanoseconds_per_second) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::corrupt_data, 0,
                                 "CLOCK_MONOTONIC result is invalid"}};
  }
  const auto seconds = static_cast<MonotonicInstant>(value.tv_sec);
  const auto nanoseconds = static_cast<MonotonicInstant>(value.tv_nsec);
  if (seconds > (std::numeric_limits<MonotonicInstant>::max() - nanoseconds) /
                    static_cast<MonotonicInstant>(nanoseconds_per_second)) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::overflow, 0,
                                 "CLOCK_MONOTONIC result overflows"}};
  }
  return seconds * static_cast<MonotonicInstant>(nanoseconds_per_second) + nanoseconds;
}

[[nodiscard]] Result<RealtimeInstant> posix_realtime_now(void*) noexcept {
  timespec value{};
  if (::clock_gettime(CLOCK_REALTIME, &value) != 0) {
    return std::unexpected{Error::from_errno(errno, "CLOCK_REALTIME query failed")};
  }
  return internal::realtime_instant_from_parts(static_cast<RealtimeInstant>(value.tv_sec),
                                               static_cast<RealtimeInstant>(value.tv_nsec));
}

const ClockOperations default_operations{nullptr, posix_monotonic_now, posix_realtime_now};

}  // namespace

Result<MonotonicInstant> read_monotonic_clock(const ClockOperations& operations) noexcept {
  if (operations.monotonic_now == nullptr) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                                 "monotonic clock operation is missing"}};
  }
  return operations.monotonic_now(operations.context);
}

Result<RealtimeInstant> read_realtime_clock(const ClockOperations& operations) noexcept {
  if (operations.realtime_now == nullptr) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
                                 "realtime clock operation is missing"}};
  }
  return operations.realtime_now(operations.context);
}

const ClockOperations& system_clock_operations() noexcept { return default_operations; }

namespace internal {

Result<RealtimeInstant> realtime_instant_from_parts(RealtimeInstant seconds,
                                                     RealtimeInstant nanoseconds) noexcept {
  if (nanoseconds < 0 || nanoseconds >= nanoseconds_per_second) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::corrupt_data, 0,
                                 "CLOCK_REALTIME result is invalid"}};
  }

  constexpr RealtimeInstant maximum = std::numeric_limits<RealtimeInstant>::max();
  constexpr RealtimeInstant minimum = std::numeric_limits<RealtimeInstant>::min();
  constexpr RealtimeInstant maximum_seconds = maximum / nanoseconds_per_second;
  constexpr RealtimeInstant maximum_nanoseconds = maximum % nanoseconds_per_second;
  constexpr RealtimeInstant minimum_truncated_seconds = minimum / nanoseconds_per_second;
  constexpr RealtimeInstant minimum_remainder = minimum % nanoseconds_per_second;
  constexpr RealtimeInstant minimum_seconds = minimum_truncated_seconds - 1;
  constexpr RealtimeInstant minimum_nanoseconds = nanoseconds_per_second + minimum_remainder;

  if (seconds > maximum_seconds ||
      (seconds == maximum_seconds && nanoseconds > maximum_nanoseconds) ||
      seconds < minimum_seconds ||
      (seconds == minimum_seconds && nanoseconds < minimum_nanoseconds)) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::overflow, 0,
                                 "CLOCK_REALTIME result overflows"}};
  }
  if (seconds == minimum_seconds) {
    return minimum + (nanoseconds - minimum_nanoseconds);
  }
  return seconds * nanoseconds_per_second + nanoseconds;
}

}  // namespace internal
}  // namespace laghu::core
