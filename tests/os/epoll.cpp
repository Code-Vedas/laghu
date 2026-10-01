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
#include <laghu/os/internal/epoll.hpp>

namespace {

using laghu::os::internal::EpollDispatcher;
using laghu::os::internal::EpollEvent;
using laghu::os::internal::EpollInterest;
using laghu::os::internal::EpollInterests;
using laghu::os::internal::EpollNotification;

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
  auto dispatcher = EpollDispatcher::create();
  auto pair = make_pair();
  if (!dispatcher || !pair) return false;
  EpollInterests interests{EpollInterest::readable};
  interests.add(EpollInterest::writable);
  if (!dispatcher->add(pair->first.native_handle(), 41U, interests)) return false;
  constexpr std::byte byte{'x'};
  if (::send(pair->second.native_handle(), &byte, 1, MSG_NOSIGNAL) != 1) return false;
  std::array<EpollEvent, 4> events{};
  const auto ready = dispatcher->wait(events, std::chrono::milliseconds{50}, 2);
  if (!ready || ready->event_count == 0U || events[0].token != 41U ||
      !events[0].notifications.contains(EpollNotification::readable) ||
      !events[0].notifications.contains(EpollNotification::writable)) return false;
  if (!dispatcher->modify(pair->first.native_handle(), 42U,
                          EpollInterests{EpollInterest::readable}) ||
      ::shutdown(pair->second.native_handle(), SHUT_WR) != 0) return false;
  std::array<std::byte, 1> drained{};
  if (::recv(pair->first.native_handle(), drained.data(), drained.size(), 0) != 1) return false;
  const auto closed = dispatcher->wait(events, std::chrono::milliseconds{50}, 2);
  return closed && closed->event_count != 0U && events[0].token == 42U &&
         events[0].notifications.contains(EpollNotification::hangup) &&
         dispatcher->remove(pair->first.native_handle());
#else
  const auto dispatcher = EpollDispatcher::create();
  return !dispatcher && dispatcher.error().code() ==
                            laghu::core::ErrorCode::unavailable_capability;
#endif
}

[[nodiscard]] bool check_descriptor_reuse_and_churn() noexcept {
#if defined(__linux__)
  auto dispatcher = EpollDispatcher::create();
  auto old_pair = make_pair();
  if (!dispatcher || !old_pair) return false;
  const int reused_descriptor = old_pair->first.native_handle();
  const int retained_descriptor = ::dup(reused_descriptor);
  if (retained_descriptor < 0) return false;
  auto retained = laghu::core::SocketHandle::adopt(retained_descriptor);
  if (!retained ||
      !dispatcher->add(reused_descriptor, 101U,
                       EpollInterests{EpollInterest::readable}) ||
      !old_pair->first.close()) {
    return false;
  }
  auto new_pair = make_pair();
  if (!new_pair || new_pair->first.native_handle() != reused_descriptor ||
      !dispatcher->add(reused_descriptor, 202U,
                       EpollInterests{EpollInterest::readable})) {
    return false;
  }
  constexpr std::byte reuse_byte{'x'};
  if (::send(old_pair->second.native_handle(), &reuse_byte, 1, MSG_NOSIGNAL) != 1 ||
      ::send(new_pair->second.native_handle(), &reuse_byte, 1, MSG_NOSIGNAL) != 1) {
    return false;
  }
  std::array<EpollEvent, 2> reused_events{};
  const auto reused = dispatcher->wait(
      reused_events, std::chrono::milliseconds{50}, 2);
  if (!reused || reused->event_count != reused_events.size()) return false;
  const bool first_order = reused_events[0].token == 101U &&
                           reused_events[1].token == 202U;
  const bool second_order = reused_events[0].token == 202U &&
                            reused_events[1].token == 101U;
  if ((!first_order && !second_order) ||
      !dispatcher->remove(new_pair->first.native_handle())) {
    return false;
  }

  constexpr std::size_t iterations = 512;
  for (std::size_t index = 0; index < iterations; ++index) {
    auto pair = make_pair();
    if (!pair) return false;
    const std::uint64_t token = index + 1U;
    if (!dispatcher->add(pair->first.native_handle(), token,
                         EpollInterests{EpollInterest::readable})) return false;
    constexpr std::byte byte{'x'};
    if (::send(pair->second.native_handle(), &byte, 1, MSG_NOSIGNAL) != 1) return false;
    std::array<EpollEvent, 1> event{};
    const auto ready = dispatcher->wait(event, std::chrono::milliseconds{50}, 2);
    if (!ready || ready->event_count != 1U || event[0].token != token ||
        !dispatcher->remove(pair->first.native_handle())) return false;
  }
  return true;
#else
  return true;
#endif
}

[[nodiscard]] bool check_writable_only_half_close() noexcept {
#if defined(__linux__)
  auto dispatcher = EpollDispatcher::create();
  auto pair = make_pair();
  if (!dispatcher || !pair ||
      !dispatcher->add(pair->first.native_handle(), 55U,
                       EpollInterests{EpollInterest::writable})) {
    return false;
  }
  std::array<EpollEvent, 1> event{};
  const auto writable = dispatcher->wait(event, std::chrono::milliseconds{50}, 2);
  if (!writable || writable->event_count != 1U || event[0].token != 55U ||
      !event[0].notifications.contains(EpollNotification::writable) ||
      ::shutdown(pair->second.native_handle(), SHUT_WR) != 0) {
    return false;
  }
  const auto closed = dispatcher->wait(event, std::chrono::milliseconds{50}, 2);
  return closed && closed->event_count == 1U && event[0].token == 55U &&
         event[0].notifications.contains(EpollNotification::hangup);
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
  auto dispatcher = EpollDispatcher::create();
  if (!socket || !dispatcher ||
      !dispatcher->add(descriptor, 77U, EpollInterests{EpollInterest::writable})) {
    return false;
  }
  const int connected = ::connect(descriptor,
                                  reinterpret_cast<const sockaddr*>(&address),
                                  sizeof(address));
  if (connected == 0 || (errno != EINPROGRESS && errno != ECONNREFUSED)) return false;
  std::array<EpollEvent, 2> events{};
  const auto ready = dispatcher->wait(events, std::chrono::milliseconds{100}, 2);
  return ready && ready->event_count != 0U && events[0].token == 77U &&
         events[0].notifications.contains(EpollNotification::error);
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
      laghu::test::TestCase{"os.epoll.tcp_error", check_tcp_error_mapping},
  };
  return laghu::test::run_tests(tests);
}
