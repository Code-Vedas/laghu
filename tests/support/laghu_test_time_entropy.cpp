// SPDX-License-Identifier: AGPL-3.0-only
#include "laghu_test_time_entropy.hpp"

#include <cerrno>
#include <limits>

namespace laghu::test {

core::Result<void> ManualClock::advance_monotonic_to(core::MonotonicInstant target) noexcept {
  if (target < monotonic_) {
    return std::unexpected{core::Error{core::ErrorDomain::core, core::ErrorCode::invalid_range, 0,
                                       "manual monotonic time cannot move backward"}};
  }
  monotonic_ = target;
  return {};
}

core::Result<void> ManualClock::advance_monotonic_by(core::MonotonicInstant elapsed) noexcept {
  if (elapsed > std::numeric_limits<core::MonotonicInstant>::max() - monotonic_) {
    return std::unexpected{core::Error{core::ErrorDomain::core, core::ErrorCode::overflow, 0,
                                       "manual monotonic time overflows"}};
  }
  monotonic_ += elapsed;
  return {};
}

core::ClockOperations ManualClock::operations() noexcept {
  return core::ClockOperations{this, read_monotonic, read_realtime};
}

core::Result<core::MonotonicInstant> ManualClock::read_monotonic(void* context) noexcept {
  if (context == nullptr) {
    return std::unexpected{core::Error{core::ErrorDomain::core, core::ErrorCode::invalid_state, 0,
                                       "manual monotonic clock has no context"}};
  }
  return static_cast<ManualClock*>(context)->monotonic_;
}

core::Result<core::RealtimeInstant> ManualClock::read_realtime(void* context) noexcept {
  if (context == nullptr) {
    return std::unexpected{core::Error{core::ErrorDomain::core, core::ErrorCode::invalid_state, 0,
                                       "manual realtime clock has no context"}};
  }
  return static_cast<ManualClock*>(context)->realtime_;
}

bool DeterministicEntropy::fail_on_call(std::size_t call, int native_error) noexcept {
  if (call == 0 || native_error == 0) {
    return false;
  }
  failure_call_ = call;
  failure_error_ = native_error;
  return true;
}

int DeterministicEntropy::fill(core::MutableByteView output, void* context) noexcept {
  if (context == nullptr) {
    errno = EINVAL;
    return -1;
  }
  auto& entropy = *static_cast<DeterministicEntropy*>(context);
  ++entropy.calls_;
  if (entropy.failure_call_ == entropy.calls_) {
    errno = entropy.failure_error_;
    return -1;
  }
  if (output.size() > entropy.bytes_.size() - entropy.offset_) {
    errno = ENOSPC;
    return -1;
  }
  for (std::size_t index = 0; index < output.size(); ++index) {
    output.span()[index] = entropy.bytes_[entropy.offset_ + index];
  }
  entropy.offset_ += output.size();
  return 0;
}

}  // namespace laghu::test
