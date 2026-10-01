// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "laghu_test_support.hpp"

#include <laghu/core/handles.hpp>
#include <laghu/os/event_dispatch.hpp>

namespace {

using laghu::os::Event;
using laghu::os::EventDispatcher;
using laghu::os::EventInterest;
using laghu::os::EventInterests;
using laghu::os::EventNotification;
using laghu::os::EventRegistration;

#if defined(__linux__)
struct Pair final {
  laghu::core::SocketHandle first;
  laghu::core::SocketHandle second;
};

[[nodiscard]] laghu::core::Result<Pair> make_pair() noexcept {
  int descriptors[2]{};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                   descriptors) != 0) {
    return std::unexpected{laghu::core::Error::from_errno(errno, "socketpair failed")};
  }
  auto first = laghu::core::SocketHandle::adopt(descriptors[0]);
  auto second = laghu::core::SocketHandle::adopt(descriptors[1]);
  if (!first || !second) return std::unexpected{laghu::core::Error::from_errno(EINVAL)};
  return Pair{std::move(*first), std::move(*second)};
}
#endif

[[nodiscard]] bool check_read_write_half_close_and_tokens() noexcept {
#if defined(__linux__)
  std::array<EventRegistration, 2> registrations{};
  auto dispatcher = EventDispatcher::create(registrations);
  auto pair = make_pair();
  if (!dispatcher || !pair) return false;
  EventInterests interests{EventInterest::readable};
  interests.add(EventInterest::writable);
  if (!dispatcher->add(pair->first.native_handle(), 41U, interests)) return false;
  constexpr std::byte byte{'x'};
  if (::send(pair->second.native_handle(), &byte, 1, MSG_NOSIGNAL) != 1) return false;
  std::array<Event, 4> events{};
  const auto ready = dispatcher->wait(events, std::chrono::milliseconds{50}, 2);
  if (!ready || ready->event_count == 0U || events[0].token != 41U ||
      !events[0].notifications.contains(EventNotification::readable) ||
      !events[0].notifications.contains(EventNotification::writable)) return false;
  if (!dispatcher->modify(pair->first.native_handle(), 41U,
                          EventInterests{EventInterest::readable}) ||
      ::shutdown(pair->second.native_handle(), SHUT_WR) != 0) return false;
  std::array<std::byte, 1> drained{};
  if (::recv(pair->first.native_handle(), drained.data(), drained.size(), 0) != 1) return false;
  const auto closed = dispatcher->wait(events, std::chrono::milliseconds{50}, 2);
  return closed && closed->event_count != 0U && events[0].token == 41U &&
         events[0].notifications.contains(EventNotification::hangup) &&
         dispatcher->remove(pair->first.native_handle(), 41U);
#else
  std::array<EventRegistration, 1> registrations{};
  const auto dispatcher = EventDispatcher::create(registrations);
  return !dispatcher && dispatcher.error().code() ==
                            laghu::core::ErrorCode::unavailable_capability;
#endif
}

[[nodiscard]] bool check_descriptor_reuse_and_churn() noexcept {
#if defined(__linux__)
  std::array<EventRegistration, 2> registrations{};
  auto dispatcher = EventDispatcher::create(registrations);
  if (!dispatcher) return false;
  {
    auto old_pair = make_pair();
    if (!old_pair) return false;
    const int reused_descriptor = old_pair->first.native_handle();
    if (!dispatcher->add(reused_descriptor, 101U,
                         EventInterests{EventInterest::readable})) {
      return false;
    }
    constexpr std::byte old_byte{'o'};
    if (::send(old_pair->second.native_handle(), &old_byte, 1, MSG_NOSIGNAL) != 1) {
      return false;
    }
    std::array<Event, 1> old_event{};
    const auto old_ready = dispatcher->wait(
        old_event, std::chrono::milliseconds{50}, 2);
    if (!old_ready || old_ready->event_count != 1U ||
        old_event[0].token != 101U ||
        !dispatcher->remove(reused_descriptor, 101U) ||
        !old_pair->first.close()) {
      return false;
    }

    auto new_pair = make_pair();
    if (!new_pair || new_pair->first.native_handle() != reused_descriptor ||
        !dispatcher->add(reused_descriptor, 202U,
                         EventInterests{EventInterest::readable})) {
      return false;
    }
    const auto stale_modify = dispatcher->modify(
        reused_descriptor, 101U, EventInterests{EventInterest::writable});
    const auto stale_remove = dispatcher->remove(reused_descriptor, 101U);
    if (stale_modify || stale_remove) return false;

    constexpr std::byte new_byte{'n'};
    if (::send(new_pair->second.native_handle(), &new_byte, 1, MSG_NOSIGNAL) != 1) {
      return false;
    }
    std::array<Event, 1> new_event{};
    const auto new_ready = dispatcher->wait(
        new_event, std::chrono::milliseconds{50}, 2);
    if (!new_ready || new_ready->event_count != 1U ||
        new_event[0].token != 202U || old_event[0].token != 101U ||
        !dispatcher->remove(reused_descriptor, 202U)) {
      return false;
    }
  }

  constexpr std::size_t iterations = 512;
  for (std::size_t index = 0; index < iterations; ++index) {
    auto pair = make_pair();
    if (!pair) return false;
    const std::uint64_t token = index + 1U;
    if (!dispatcher->add(pair->first.native_handle(), token,
                         EventInterests{EventInterest::readable})) return false;
    constexpr std::byte byte{'x'};
    if (::send(pair->second.native_handle(), &byte, 1, MSG_NOSIGNAL) != 1) return false;
    std::array<Event, 1> event{};
    const auto ready = dispatcher->wait(event, std::chrono::milliseconds{50}, 2);
    if (!ready || ready->event_count != 1U || event[0].token != token ||
        !dispatcher->remove(pair->first.native_handle(), token)) return false;
  }
  return true;
#else
  return true;
#endif
}

[[nodiscard]] bool check_writable_only_half_close() noexcept {
#if defined(__linux__)
  std::array<EventRegistration, 1> registrations{};
  auto dispatcher = EventDispatcher::create(registrations);
  auto pair = make_pair();
  if (!dispatcher || !pair ||
      !dispatcher->add(pair->first.native_handle(), 55U,
                       EventInterests{EventInterest::writable})) {
    return false;
  }
  std::array<Event, 1> event{};
  const auto writable = dispatcher->wait(event, std::chrono::milliseconds{50}, 2);
  if (!writable || writable->event_count != 1U || event[0].token != 55U ||
      !event[0].notifications.contains(EventNotification::writable) ||
      ::shutdown(pair->second.native_handle(), SHUT_WR) != 0) {
    return false;
  }
  const auto closed = dispatcher->wait(event, std::chrono::milliseconds{50}, 2);
  return closed && closed->event_count == 1U && event[0].token == 55U &&
         event[0].notifications.contains(EventNotification::hangup);
#else
  return true;
#endif
}

[[nodiscard]] bool check_registration_bounds() noexcept {
#if defined(__linux__)
  std::array<EventRegistration, 1> registrations{};
  auto dispatcher = EventDispatcher::create(registrations);
  auto first = make_pair();
  auto second = make_pair();
  if (!dispatcher || !first || !second ||
      !dispatcher->add(first->first.native_handle(), 301U,
                       EventInterests{EventInterest::readable})) {
    return false;
  }
  const auto duplicate = dispatcher->add(
      first->first.native_handle(), 302U,
      EventInterests{EventInterest::readable});
  const auto exhausted = dispatcher->add(
      second->first.native_handle(), 303U,
      EventInterests{EventInterest::readable});
  if (duplicate || duplicate.error().code() != laghu::core::ErrorCode::invalid_input ||
      exhausted || exhausted.error().code() != laghu::core::ErrorCode::exhaustion ||
      !dispatcher->remove(first->first.native_handle(), 301U) ||
      !dispatcher->add(second->first.native_handle(), 303U,
                       EventInterests{EventInterest::readable})) {
    return false;
  }
  return dispatcher->remove(second->first.native_handle(), 303U).has_value();
#else
  return true;
#endif
}

[[nodiscard]] bool check_tcp_error_mapping() noexcept {
#if defined(__linux__)
  const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener < 0) return false;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    static_cast<void>(::close(listener));
    return false;
  }
  socklen_t size = sizeof(address);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) != 0 ||
      ::close(listener) != 0) return false;
  const int descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (descriptor < 0) return false;
  auto socket = laghu::core::SocketHandle::adopt(descriptor);
  std::array<EventRegistration, 1> registrations{};
  auto dispatcher = EventDispatcher::create(registrations);
  if (!socket || !dispatcher ||
      !dispatcher->add(descriptor, 77U, EventInterests{EventInterest::writable})) {
    return false;
  }
  const int connected = ::connect(descriptor,
                                  reinterpret_cast<const sockaddr*>(&address),
                                  sizeof(address));
  if (connected == 0 || (errno != EINPROGRESS && errno != ECONNREFUSED)) return false;
  std::array<Event, 2> events{};
  const auto ready = dispatcher->wait(events, std::chrono::milliseconds{100}, 2);
  return ready && ready->event_count != 0U && events[0].token == 77U &&
         events[0].notifications.contains(EventNotification::error);
#else
  return true;
#endif
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"os.epoll.events_tokens", check_read_write_half_close_and_tokens},
      laghu::test::TestCase{"os.epoll.descriptor_reuse_churn", check_descriptor_reuse_and_churn},
      laghu::test::TestCase{"os.epoll.writable_only_half_close",
                            check_writable_only_half_close},
      laghu::test::TestCase{"os.epoll.registration_bounds",
                            check_registration_bounds},
      laghu::test::TestCase{"os.epoll.tcp_error", check_tcp_error_mapping},
  };
  return laghu::test::run_tests(tests);
}
