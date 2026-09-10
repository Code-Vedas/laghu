// SPDX-License-Identifier: AGPL-3.0-only
#include "laghu_test_support.hpp"
#include "laghu_test_time_entropy.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <span>

#include <laghu/core/deadlines_cancellation.hpp>
#include <laghu/core/internal/clock_operations.hpp>

namespace {

template <std::size_t Size>
[[nodiscard]] laghu::core::MutableByteView mutable_view(std::array<std::byte, Size>& bytes) noexcept {
  return *laghu::core::MutableByteView::from(std::span<std::byte>{bytes});
}

[[nodiscard]] bool check_manual_clocks_and_deadlines() noexcept {
  laghu::test::ManualClock clock{100, 1000};
  const auto operations = clock.operations();
  const auto deadline = laghu::core::Deadline::after(operations, 20);
  if (!deadline.has_value() || deadline->instant() != 120 ||
      !deadline->require_not_expired(operations).has_value() ||
      !clock.advance_monotonic_by(19).has_value()) {
    return false;
  }
  const auto before_expiry = deadline->expired(operations);
  if (!before_expiry.has_value() || *before_expiry || !clock.advance_monotonic_to(120).has_value() ||
      !deadline->expired(operations).has_value() || !*deadline->expired(operations) ||
      deadline->require_not_expired(operations).has_value()) {
    return false;
  }
  const auto backward_monotonic = clock.advance_monotonic_to(119);
  if (backward_monotonic.has_value() ||
      backward_monotonic.error().code() != laghu::core::ErrorCode::invalid_range ||
      clock.monotonic() != 120) {
    return false;
  }
  const auto initial_realtime = laghu::core::read_realtime_clock(operations);
  clock.jump_realtime_to(1500);
  const auto forward_realtime = laghu::core::read_realtime_clock(operations);
  clock.jump_realtime_to(750);
  const auto backward_realtime = laghu::core::read_realtime_clock(operations);
  return initial_realtime.has_value() && *initial_realtime == 1000 &&
         forward_realtime.has_value() && *forward_realtime == 1500 &&
         backward_realtime.has_value() && *backward_realtime == 750 && clock.monotonic() == 120;
}

[[nodiscard]] bool check_clock_tables() noexcept {
  const auto& operations = laghu::core::internal::default_clock_operations();
  const auto monotonic = laghu::core::read_monotonic_clock(operations);
  const auto realtime = laghu::core::read_realtime_clock(operations);
  const laghu::core::ClockOperations incomplete{};
  const auto missing_monotonic = laghu::core::read_monotonic_clock(incomplete);
  const auto missing_realtime = laghu::core::read_realtime_clock(incomplete);
  return monotonic.has_value() && realtime.has_value() && !missing_monotonic.has_value() &&
         !missing_realtime.has_value() &&
         missing_monotonic.error().code() == laghu::core::ErrorCode::invalid_state &&
         missing_realtime.error().code() == laghu::core::ErrorCode::invalid_state;
}

[[nodiscard]] bool check_realtime_conversion_boundaries() noexcept {
  constexpr laghu::core::RealtimeInstant nanoseconds_per_second = 1000000000;
  constexpr laghu::core::RealtimeInstant maximum =
      std::numeric_limits<laghu::core::RealtimeInstant>::max();
  constexpr laghu::core::RealtimeInstant minimum =
      std::numeric_limits<laghu::core::RealtimeInstant>::min();
  constexpr laghu::core::RealtimeInstant maximum_seconds = maximum / nanoseconds_per_second;
  constexpr laghu::core::RealtimeInstant maximum_nanoseconds = maximum % nanoseconds_per_second;
  constexpr laghu::core::RealtimeInstant minimum_seconds =
      minimum / nanoseconds_per_second - 1;
  constexpr laghu::core::RealtimeInstant minimum_nanoseconds =
      nanoseconds_per_second + minimum % nanoseconds_per_second;

  const auto exact_maximum = laghu::core::internal::realtime_instant_from_parts(
      maximum_seconds, maximum_nanoseconds);
  const auto maximum_overflow = laghu::core::internal::realtime_instant_from_parts(
      maximum_seconds, maximum_nanoseconds + 1);
  const auto exact_minimum = laghu::core::internal::realtime_instant_from_parts(
      minimum_seconds, minimum_nanoseconds);
  const auto minimum_overflow = laghu::core::internal::realtime_instant_from_parts(
      minimum_seconds, minimum_nanoseconds - 1);
  const auto negative_nanoseconds =
      laghu::core::internal::realtime_instant_from_parts(0, -1);
  const auto excessive_nanoseconds = laghu::core::internal::realtime_instant_from_parts(
      0, nanoseconds_per_second);
  return exact_maximum.has_value() && *exact_maximum == maximum &&
         !maximum_overflow.has_value() &&
         maximum_overflow.error().code() == laghu::core::ErrorCode::overflow &&
         exact_minimum.has_value() && *exact_minimum == minimum &&
         !minimum_overflow.has_value() &&
         minimum_overflow.error().code() == laghu::core::ErrorCode::overflow &&
         !negative_nanoseconds.has_value() &&
         negative_nanoseconds.error().code() == laghu::core::ErrorCode::corrupt_data &&
         !excessive_nanoseconds.has_value() &&
         excessive_nanoseconds.error().code() == laghu::core::ErrorCode::corrupt_data;
}

[[nodiscard]] bool check_deterministic_entropy() noexcept {
  std::array<std::byte, 6> sequence{};
  for (std::size_t index = 0; index < sequence.size(); ++index) {
    sequence[index] = static_cast<std::byte>(index);
  }
  std::array<std::byte, 3> output{};
  laghu::test::DeterministicEntropy entropy{std::span<const std::byte>{sequence}};
  const auto first = entropy.fill(mutable_view(output));
  const auto second = entropy.fill(mutable_view(output));
  if (!first.has_value() || !second.has_value() || entropy.calls() != 2 ||
      entropy.consumed() != sequence.size() || output[0] != sequence[3] ||
      output[1] != sequence[4] || output[2] != sequence[5]) {
    return false;
  }

  laghu::test::DeterministicEntropy failing{std::span<const std::byte>{sequence}};
  if (!failing.fail_on_call(2, EIO)) {
    return false;
  }
  const auto successful = failing.fill(mutable_view(output));
  const auto failure = failing.fill(mutable_view(output));
  return successful.has_value() && !failure.has_value() && failing.calls() == 2 &&
         failing.consumed() == output.size() &&
         failure.error().domain() == laghu::core::ErrorDomain::posix &&
         failure.error().code() == laghu::core::ErrorCode::io && failure.error().native_code() == EIO;
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"time_entropy.manual_clocks_and_deadlines",
                            check_manual_clocks_and_deadlines},
      laghu::test::TestCase{"time_entropy.clock_tables", check_clock_tables},
      laghu::test::TestCase{"time_entropy.realtime_conversion_boundaries",
                            check_realtime_conversion_boundaries},
      laghu::test::TestCase{"time_entropy.deterministic_entropy", check_deterministic_entropy},
  };
  return laghu::test::run_tests(tests);
}
