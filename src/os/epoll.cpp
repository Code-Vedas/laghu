// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/os/internal/epoll.hpp>

#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>

#if defined(__linux__)
#include <sys/epoll.h>
#include <unistd.h>
#endif

namespace laghu::os::internal {
namespace {

#if !defined(__linux__)
[[nodiscard]] core::Error unavailable() noexcept {
  return {core::ErrorDomain::core, core::ErrorCode::unavailable_capability, 0,
          "epoll is unavailable on this platform"};
}
#endif

#if defined(__linux__)
[[nodiscard]] core::Error invalid(const char* text) noexcept {
  return {core::ErrorDomain::core, core::ErrorCode::invalid_input, 0, text};
}

[[nodiscard]] std::uint32_t native_interests(EpollInterests interests) noexcept {
  std::uint32_t events = EPOLLET;
  if (interests.contains(EpollInterest::readable)) events |= EPOLLIN | EPOLLRDHUP;
  if (interests.contains(EpollInterest::writable)) events |= EPOLLOUT;
  return events;
}

[[nodiscard]] core::Result<void> control(int epoll_descriptor, int operation,
                                         int descriptor, std::uint64_t token,
                                         EpollInterests interests) noexcept {
  if (descriptor < 0 || token == 0U) {
    return std::unexpected{invalid("epoll registration is invalid")};
  }
  epoll_event event{};
  event.events = native_interests(interests);
  event.data.u64 = token;
  if (::epoll_ctl(epoll_descriptor, operation, descriptor, &event) != 0) {
    return std::unexpected{core::Error::from_errno(errno, "epoll control failed")};
  }
  return {};
}

[[nodiscard]] int wait_milliseconds(std::chrono::nanoseconds duration) noexcept {
  if (duration.count() == 0) return 0;
  constexpr auto nanoseconds_per_millisecond =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::milliseconds{1})
          .count();
  const auto rounded =
      1 + ((duration.count() - 1) / nanoseconds_per_millisecond);
  return rounded > INT_MAX ? INT_MAX : static_cast<int>(rounded);
}
#endif

}  // namespace

core::Result<EpollDispatcher> EpollDispatcher::create() noexcept {
#if defined(__linux__)
  const int descriptor = ::epoll_create1(EPOLL_CLOEXEC);
  if (descriptor < 0) {
    return std::unexpected{core::Error::from_errno(errno, "epoll creation failed")};
  }
  auto handle = core::FileHandle::adopt(descriptor);
  if (!handle) {
    static_cast<void>(::close(descriptor));
    return std::unexpected{handle.error()};
  }
  return EpollDispatcher{std::move(*handle)};
#else
  return std::unexpected{unavailable()};
#endif
}

core::Result<void> EpollDispatcher::add(int descriptor, std::uint64_t token,
                                        EpollInterests interests) const noexcept {
#if defined(__linux__)
  return control(descriptor_.native_handle(), EPOLL_CTL_ADD, descriptor, token,
                 interests);
#else
  static_cast<void>(descriptor); static_cast<void>(token); static_cast<void>(interests);
  return std::unexpected{unavailable()};
#endif
}

core::Result<void> EpollDispatcher::modify(int descriptor, std::uint64_t token,
                                           EpollInterests interests) const noexcept {
#if defined(__linux__)
  return control(descriptor_.native_handle(), EPOLL_CTL_MOD, descriptor, token,
                 interests);
#else
  static_cast<void>(descriptor); static_cast<void>(token); static_cast<void>(interests);
  return std::unexpected{unavailable()};
#endif
}

core::Result<void> EpollDispatcher::remove(int descriptor) const noexcept {
#if defined(__linux__)
  if (descriptor < 0) return std::unexpected{invalid("epoll removal is invalid")};
  if (::epoll_ctl(descriptor_.native_handle(), EPOLL_CTL_DEL, descriptor, nullptr) != 0) {
    return std::unexpected{core::Error::from_errno(errno, "epoll removal failed")};
  }
  return {};
#else
  static_cast<void>(descriptor); return std::unexpected{unavailable()};
#endif
}

core::Result<EpollWaitResult> EpollDispatcher::wait(
    std::span<EpollEvent> output, std::chrono::nanoseconds maximum_wait,
    std::uint32_t maximum_wait_calls) const noexcept {
#if defined(__linux__)
  if (output.empty() || output.size() > maximum_events || maximum_wait.count() < 0 ||
      maximum_wait_calls == 0U) {
    return std::unexpected{invalid("epoll wait arguments are invalid")};
  }
  std::array<epoll_event, maximum_events> events;
  for (std::uint32_t call = 1; call <= maximum_wait_calls; ++call) {
    const int count = ::epoll_wait(descriptor_.native_handle(), events.data(),
                                   static_cast<int>(output.size()),
                                   wait_milliseconds(maximum_wait));
    if (count < 0) {
      if (errno == EINTR) continue;
      return std::unexpected{core::Error::from_errno(errno, "epoll wait failed")};
    }
    for (int index = 0; index < count; ++index) {
      EpollNotifications notifications;
      const std::uint32_t flags = events[static_cast<std::size_t>(index)].events;
      if ((flags & EPOLLIN) != 0U) notifications.add(EpollNotification::readable);
      if ((flags & EPOLLOUT) != 0U) notifications.add(EpollNotification::writable);
      if ((flags & EPOLLERR) != 0U) notifications.add(EpollNotification::error);
      if ((flags & (EPOLLHUP | EPOLLRDHUP)) != 0U) {
        notifications.add(EpollNotification::hangup);
      }
      output[static_cast<std::size_t>(index)] =
          EpollEvent{events[static_cast<std::size_t>(index)].data.u64, notifications};
    }
    return EpollWaitResult{static_cast<std::size_t>(count), call,
                           static_cast<std::size_t>(count) == output.size()};
  }
  return EpollWaitResult{0, maximum_wait_calls, false};
#else
  static_cast<void>(output); static_cast<void>(maximum_wait);
  static_cast<void>(maximum_wait_calls); return std::unexpected{unavailable()};
#endif
}

}  // namespace laghu::os::internal
