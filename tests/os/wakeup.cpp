// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <fcntl.h>
#include <unistd.h>

#include "laghu_test_support.hpp"

#include <laghu/os/internal/wakeup.hpp>
#include <laghu/os/wakeup.hpp>

namespace {

using laghu::os::WakeupChannel;
using laghu::os::WakeupMechanism;
using laghu::os::WakeupNotifyResult;
using laghu::os::internal::WakeupTestAccess;

[[nodiscard]] bool descriptor_is_nonblocking_and_close_on_exec(int descriptor) noexcept {
  const int status = ::fcntl(descriptor, F_GETFL, 0);
  const int flags = ::fcntl(descriptor, F_GETFD, 0);
  return status >= 0 && (status & O_NONBLOCK) != 0 && flags >= 0 &&
         (flags & FD_CLOEXEC) != 0;
}

[[nodiscard]] bool fill_notification_channel(WakeupChannel& channel) noexcept {
  const int descriptor = WakeupTestAccess::write_descriptor(channel);
  if (channel.mechanism() == WakeupMechanism::event_counter) {
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max() - 1U;
    return ::write(descriptor, &maximum, sizeof(maximum)) ==
           static_cast<ssize_t>(sizeof(maximum));
  }

  constexpr std::array<std::byte, 256> bytes{};
  for (;;) {
    const ssize_t written = ::write(descriptor, bytes.data(), bytes.size());
    if (written > 0) {
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
  }
}

[[nodiscard]] bool check_platform_channel() noexcept {
  auto channel = WakeupChannel::create();
  if (!channel) {
    return false;
  }
  const auto descriptor = channel->notification_descriptor();
  if (!descriptor ||
      !descriptor_is_nonblocking_and_close_on_exec(*descriptor) ||
      !descriptor_is_nonblocking_and_close_on_exec(
          WakeupTestAccess::write_descriptor(*channel))) {
    return false;
  }
#if defined(__linux__)
  if (channel->mechanism() != WakeupMechanism::event_counter) {
    return false;
  }
#else
  if (channel->mechanism() != WakeupMechanism::pipe) {
    return false;
  }
#endif
  return channel->close().has_value() && !channel->notification_descriptor();
}

[[nodiscard]] bool check_saturation_coalesces_safely() noexcept {
  auto channel = WakeupChannel::create();
  if (!channel || !fill_notification_channel(*channel)) {
    return false;
  }
  const auto saturated = channel->notify();
  const auto consumed = channel->consume();
  const auto signaled = channel->notify();
  const auto final = channel->consume();
  return saturated && *saturated == WakeupNotifyResult::coalesced && consumed &&
         consumed->observed && !consumed->pending && signaled &&
         *signaled == WakeupNotifyResult::signaled && final && final->observed &&
         !final->pending;
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"os.wakeup.platform", check_platform_channel},
      laghu::test::TestCase{"os.wakeup.saturation", check_saturation_coalesces_safely},
  };
  return laghu::test::run_tests(tests);
}
