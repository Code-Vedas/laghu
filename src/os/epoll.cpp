// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/os/event_dispatch.hpp>

#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>

#if defined(__linux__)
#include <sys/epoll.h>
#include <unistd.h>
#endif

namespace laghu::os {
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

[[nodiscard]] std::uint32_t native_interests(EventInterests interests) noexcept {
  std::uint32_t events = EPOLLET | EPOLLRDHUP;
  if (interests.contains(EventInterest::readable)) events |= EPOLLIN;
  if (interests.contains(EventInterest::writable)) events |= EPOLLOUT;
  return events;
}

[[nodiscard]] core::Result<void> control(int epoll_descriptor, int operation,
                                         int descriptor, std::uint64_t token,
                                         EventInterests interests) noexcept {
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

core::Result<EventDispatcher> EventDispatcher::create(
    std::span<EventRegistration> registrations) noexcept {
#if defined(__linux__)
  if (registrations.empty()) {
    return std::unexpected{invalid("event registration capacity must be nonzero")};
  }
  const int descriptor = ::epoll_create1(EPOLL_CLOEXEC);
  if (descriptor < 0) {
    return std::unexpected{core::Error::from_errno(errno, "epoll creation failed")};
  }
  auto handle = core::FileHandle::adopt(descriptor);
  if (!handle) {
    static_cast<void>(::close(descriptor));
    return std::unexpected{handle.error()};
  }
  for (auto& registration : registrations) registration.clear();
  return EventDispatcher{std::move(*handle), registrations};
#else
  static_cast<void>(registrations);
  return std::unexpected{unavailable()};
#endif
}

core::Result<void> EventDispatcher::add(int descriptor, std::uint64_t token,
                                        EventInterests interests) noexcept {
#if defined(__linux__)
  if (descriptor < 0 || token == 0U) {
    return std::unexpected{invalid("event registration is invalid")};
  }
  EventRegistration* available{};
  for (auto& registration : registrations_) {
    if (registration.descriptor_matches(descriptor)) {
      return std::unexpected{invalid("event descriptor is already registered")};
    }
    if (available == nullptr && registration.available()) available = &registration;
  }
  if (available == nullptr) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::exhaustion, 0,
                                       "event registration capacity exhausted"}};
  }
  const auto added = control(descriptor_.native_handle(), EPOLL_CTL_ADD,
                             descriptor, token, interests);
  if (!added) return std::unexpected{added.error()};
  available->assign(descriptor, token);
  return {};
#else
  static_cast<void>(descriptor); static_cast<void>(token); static_cast<void>(interests);
  return std::unexpected{unavailable()};
#endif
}

core::Result<void> EventDispatcher::modify(
    int descriptor, std::uint64_t expected_token,
    EventInterests interests) noexcept {
#if defined(__linux__)
  if (descriptor < 0 || expected_token == 0U) {
    return std::unexpected{invalid("event registration is invalid")};
  }
  for (const auto& registration : registrations_) {
    if (!registration.descriptor_matches(descriptor)) continue;
    if (!registration.token_matches(expected_token)) {
      return std::unexpected{invalid("event registration token does not match")};
    }
    return control(descriptor_.native_handle(), EPOLL_CTL_MOD, descriptor,
                   expected_token, interests);
  }
  return std::unexpected{invalid("event descriptor is not registered")};
#else
  static_cast<void>(descriptor); static_cast<void>(expected_token);
  static_cast<void>(interests);
  return std::unexpected{unavailable()};
#endif
}

core::Result<void> EventDispatcher::remove(
    int descriptor, std::uint64_t expected_token) noexcept {
#if defined(__linux__)
  if (descriptor < 0 || expected_token == 0U) {
    return std::unexpected{invalid("event registration is invalid")};
  }
  for (auto& registration : registrations_) {
    if (!registration.descriptor_matches(descriptor)) continue;
    if (!registration.token_matches(expected_token)) {
      return std::unexpected{invalid("event registration token does not match")};
    }
    if (::epoll_ctl(descriptor_.native_handle(), EPOLL_CTL_DEL, descriptor,
                    nullptr) != 0) {
      return std::unexpected{core::Error::from_errno(errno, "epoll removal failed")};
    }
    registration.clear();
    return {};
  }
  return std::unexpected{invalid("event descriptor is not registered")};
#else
  static_cast<void>(descriptor); static_cast<void>(expected_token);
  return std::unexpected{unavailable()};
#endif
}

core::Result<EventWaitResult> EventDispatcher::wait(
    std::span<Event> output, std::chrono::nanoseconds maximum_wait,
    std::uint32_t maximum_wait_calls) const noexcept {
#if defined(__linux__)
  if (output.empty() || output.size() > maximum_events || maximum_wait.count() < 0 ||
      maximum_wait_calls == 0U) {
    return std::unexpected{invalid("epoll wait arguments are invalid")};
  }
  std::array<epoll_event, maximum_events> events;
  const auto started = std::chrono::steady_clock::now();
  std::uint32_t call{};
  while (call < maximum_wait_calls) {
    ++call;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started);
    const auto remaining = elapsed >= maximum_wait
                               ? std::chrono::nanoseconds{0}
                               : maximum_wait - elapsed;
    const int count = ::epoll_wait(descriptor_.native_handle(), events.data(),
                                   static_cast<int>(output.size()),
                                   wait_milliseconds(remaining));
    if (count < 0) {
      if (errno == EINTR) continue;
      return std::unexpected{core::Error::from_errno(errno, "epoll wait failed")};
    }
    for (int index = 0; index < count; ++index) {
      EventNotifications notifications;
      const std::uint32_t flags = events[static_cast<std::size_t>(index)].events;
      if ((flags & EPOLLIN) != 0U) notifications.add(EventNotification::readable);
      if ((flags & EPOLLOUT) != 0U) notifications.add(EventNotification::writable);
      if ((flags & EPOLLERR) != 0U) notifications.add(EventNotification::error);
      if ((flags & (EPOLLHUP | EPOLLRDHUP)) != 0U) {
        notifications.add(EventNotification::hangup);
      }
      output[static_cast<std::size_t>(index)] =
          Event{events[static_cast<std::size_t>(index)].data.u64, notifications};
    }
    return EventWaitResult{static_cast<std::size_t>(count), call,
                           static_cast<std::size_t>(count) == output.size()};
  }
  return EventWaitResult{0, maximum_wait_calls, false};
#else
  static_cast<void>(output); static_cast<void>(maximum_wait);
  static_cast<void>(maximum_wait_calls); return std::unexpected{unavailable()};
#endif
}

}  // namespace laghu::os
