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
#include <laghu/core/views.hpp>
#include <laghu/os/internal/socket_drain.hpp>

namespace {

using laghu::os::SocketDrainState;
using laghu::os::internal::SocketDrainOperations;
using laghu::os::internal::SocketDrainTestAccess;

struct Fixture final {
  std::array<ssize_t, 4> results{};
  std::array<int, 4> errors{};
  std::size_t index{};
  std::uint64_t now{};
  std::uint64_t step{};
  int accepted_descriptor{-1};
};

[[nodiscard]] ssize_t next_result(Fixture& fixture, void* output,
                                  std::size_t size) noexcept {
  const std::size_t index = fixture.index++;
  const ssize_t result = fixture.results[index];
  if (result < 0) errno = fixture.errors[index];
  static_cast<void>(output);
  static_cast<void>(size);
  return result;
}
[[nodiscard]] ssize_t injected_read(void* context, int, void* output,
                                    std::size_t size) noexcept {
  return next_result(*static_cast<Fixture*>(context), output, size);
}
[[nodiscard]] ssize_t injected_write(void* context, int, const void*,
                                     std::size_t size) noexcept {
  return next_result(*static_cast<Fixture*>(context), nullptr, size);
}
[[nodiscard]] int injected_accept(void* context, int) noexcept {
  auto& fixture = *static_cast<Fixture*>(context);
  const ssize_t result = next_result(fixture, nullptr, 0);
  if (result >= 0) return fixture.accepted_descriptor;
  return -1;
}
[[nodiscard]] int injected_get_flags(void*, int) noexcept { return O_NONBLOCK; }
[[nodiscard]] laghu::core::Result<laghu::core::MonotonicInstant> now(
    void* context) noexcept {
  auto& fixture = *static_cast<Fixture*>(context);
  const std::uint64_t value = fixture.now;
  fixture.now += fixture.step;
  return value;
}
[[nodiscard]] laghu::core::Result<laghu::core::RealtimeInstant> realtime(
    void*) noexcept { return 0; }
[[nodiscard]] SocketDrainOperations operations(Fixture& fixture) noexcept {
  return {&fixture, injected_accept, injected_get_flags, injected_read,
          injected_write,
          {&fixture, now, realtime}};
}

[[nodiscard]] laghu::core::Result<laghu::core::SocketHandle> socket_handle() noexcept {
  int descriptors[2]{};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
    return std::unexpected{laghu::core::Error::from_errno(errno)};
  }
  static_cast<void>(::close(descriptors[1]));
  return laghu::core::SocketHandle::adopt(descriptors[0]);
}

[[nodiscard]] bool check_read_eintr_partial_eagain() noexcept {
  auto socket = socket_handle();
  std::array<std::byte, 8> storage{};
  const auto output = laghu::core::MutableByteView::from(storage);
  Fixture fixture{{-1, 3, -1}, {EINTR, 0, EAGAIN}};
  auto ops = operations(fixture);
  const auto result = SocketDrainTestAccess::read(
      socket->borrow(), *output, {8, 4, std::chrono::seconds{1}}, ops);
  return result && result->bytes == 3U && result->operations == 3U &&
         result->state == SocketDrainState::would_block;
}

[[nodiscard]] bool check_write_partial_and_operation_budget() noexcept {
  auto socket = socket_handle();
  constexpr std::array bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  const auto input = laghu::core::ByteView::from(bytes);
  Fixture fixture{{1, 1}};
  auto ops = operations(fixture);
  const auto result = SocketDrainTestAccess::write(
      socket->borrow(), *input, {3, 2, std::chrono::seconds{1}}, ops);
  return result && result->bytes == 2U && result->operations == 2U &&
         result->state == SocketDrainState::budget_exhausted;
}

[[nodiscard]] bool check_write_partial_terminal_error() noexcept {
  auto socket = socket_handle();
  constexpr std::array bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  const auto input = laghu::core::ByteView::from(bytes);
  Fixture fixture{{2, -1}, {0, EIO}};
  auto ops = operations(fixture);
  const auto result = SocketDrainTestAccess::write(
      socket->borrow(), *input, {3, 3, std::chrono::seconds{1}}, ops);
  return result && result->bytes == 2U && result->operations == 2U &&
         result->state == SocketDrainState::failed &&
         result->terminal_error.has_value() &&
         result->terminal_error->native_code() == EIO;
}

[[nodiscard]] bool check_time_and_accept_pressure() noexcept {
  auto socket = socket_handle();
  std::array<std::byte, 1> storage{};
  const auto output = laghu::core::MutableByteView::from(storage);
  Fixture timed{};
  timed.results = {1};
  timed.step = 10;
  auto timed_ops = operations(timed);
  const auto timeout = SocketDrainTestAccess::read(
      socket->borrow(), *output, {1, 2, std::chrono::nanoseconds{5}}, timed_ops);
  if (!timeout || timeout->operations != 0U ||
      timeout->state != SocketDrainState::budget_exhausted) return false;

  Fixture accept_timed{};
  accept_timed.step = 10;
  auto accept_timed_ops = operations(accept_timed);
  std::array<laghu::core::SocketHandle, 1> accepted{};
  const auto accept_timeout = SocketDrainTestAccess::accept(
      socket->borrow(), accepted, {1, 2, std::chrono::nanoseconds{5}},
      accept_timed_ops);
  if (!accept_timeout || accept_timeout->accepted != 0U ||
      accept_timeout->operations != 1U ||
      accept_timeout->state != SocketDrainState::budget_exhausted) {
    return false;
  }

  Fixture accept_limited{{-1}, {EAGAIN}};
  auto accept_limited_ops = operations(accept_limited);
  const auto accept_limit = SocketDrainTestAccess::accept(
      socket->borrow(), accepted, {1, 1, std::chrono::seconds{1}},
      accept_limited_ops);
  if (!accept_limit || accept_limit->accepted != 0U ||
      accept_limit->operations != 1U ||
      accept_limit->state != SocketDrainState::budget_exhausted ||
      accept_limited.index != 0U) {
    return false;
  }

  Fixture pressure{{-1}, {EMFILE}};
  auto pressure_ops = operations(pressure);
  const auto result = SocketDrainTestAccess::accept(
      socket->borrow(), accepted, {1, 2, std::chrono::seconds{1}}, pressure_ops);
  return result && result->accepted == 0U && result->operations == 2U &&
         result->state == SocketDrainState::resource_pressure;
}

[[nodiscard]] bool check_accept_progress_to_eagain() noexcept {
  auto listener = socket_handle();
  auto accepted_source = socket_handle();
  if (!listener || !accepted_source) return false;
  const int accepted_descriptor = ::dup(accepted_source->native_handle());
  if (accepted_descriptor < 0) return false;
  Fixture fixture{{1, -1}, {0, EAGAIN}};
  fixture.accepted_descriptor = accepted_descriptor;
  auto ops = operations(fixture);
  std::array<laghu::core::SocketHandle, 2> accepted{};
  const auto result = SocketDrainTestAccess::accept(
      listener->borrow(), accepted, {2, 4, std::chrono::seconds{1}}, ops);
  return result && result->accepted == 1U && result->operations == 3U &&
         result->state == SocketDrainState::would_block && accepted[0].is_valid();
}

[[nodiscard]] bool check_accept_pending_network_error() noexcept {
#if defined(__linux__)
  auto listener = socket_handle();
  auto accepted_source = socket_handle();
  if (!listener || !accepted_source) return false;
  constexpr std::array errors{ECONNABORTED, ENETDOWN,    EPROTO,
                              ENOPROTOOPT,  EHOSTDOWN,   ENONET,
                              EHOSTUNREACH, EOPNOTSUPP,  ENETUNREACH};
  for (const int error : errors) {
    const int accepted_descriptor = ::dup(accepted_source->native_handle());
    if (accepted_descriptor < 0) return false;
    Fixture fixture{{-1, 1, -1}, {error, 0, EAGAIN}};
    fixture.accepted_descriptor = accepted_descriptor;
    auto ops = operations(fixture);
    std::array<laghu::core::SocketHandle, 2> accepted{};
    const auto result = SocketDrainTestAccess::accept(
        listener->borrow(), accepted, {2, 4, std::chrono::seconds{1}}, ops);
    if (!result || result->accepted != 1U || result->operations != 4U ||
        result->state != SocketDrainState::would_block || !accepted[0].is_valid()) {
      if (!accepted[0].is_valid()) static_cast<void>(::close(accepted_descriptor));
      return false;
    }
  }
  return true;
#else
  return true;
#endif
}

[[nodiscard]] bool check_blocking_listener_rejected() noexcept {
#if defined(__linux__)
  auto listener = socket_handle();
  if (!listener) return false;
  std::array<laghu::core::SocketHandle, 1> accepted{};
  const auto result = laghu::os::drain_accept(
      listener->borrow(), accepted, {1, 1, std::chrono::seconds{1}});
  return !result && result.error().code() == laghu::core::ErrorCode::invalid_input;
#else
  return true;
#endif
}

[[nodiscard]] bool check_public_read_write() noexcept {
#if defined(__linux__)
  int descriptors[2]{};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                   descriptors) != 0) {
    return false;
  }
  auto first = laghu::core::SocketHandle::adopt(descriptors[0]);
  auto second = laghu::core::SocketHandle::adopt(descriptors[1]);
  if (!first || !second) return false;

  constexpr std::array input{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  if (::send(second->native_handle(), input.data(), input.size(), MSG_NOSIGNAL) !=
      static_cast<ssize_t>(input.size())) {
    return false;
  }
  std::array<std::byte, 8> received{};
  const auto output = laghu::core::MutableByteView::from(received);
  const auto read = laghu::os::drain_socket_read(
      first->borrow(), *output, {received.size(), 4, std::chrono::seconds{1}});
  if (!read || read->bytes != input.size() ||
      read->state != SocketDrainState::would_block ||
      received[0] != input[0] || received[1] != input[1] ||
      received[2] != input[2]) {
    return false;
  }

  const auto view = laghu::core::ByteView::from(input);
  const auto written = laghu::os::drain_socket_write(
      first->borrow(), *view, {input.size(), 2, std::chrono::seconds{1}});
  std::array<std::byte, input.size()> peer_received{};
  const ssize_t peer_bytes = ::recv(second->native_handle(), peer_received.data(),
                                    peer_received.size(), 0);
  return written && written->bytes == input.size() &&
         written->state == SocketDrainState::budget_exhausted &&
         peer_bytes == static_cast<ssize_t>(input.size()) && peer_received == input;
#else
  return true;
#endif
}

[[nodiscard]] bool check_public_accept_flags() noexcept {
#if defined(__linux__)
  const int listener_descriptor =
      ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listener_descriptor < 0) return false;
  auto listener = laghu::core::SocketHandle::adopt(listener_descriptor);
  if (!listener) return false;

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(listener->native_handle(), reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 ||
      ::listen(listener->native_handle(), 1) != 0) {
    return false;
  }
  socklen_t address_size = sizeof(address);
  if (::getsockname(listener->native_handle(), reinterpret_cast<sockaddr*>(&address),
                    &address_size) != 0) {
    return false;
  }

  const int client_descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (client_descriptor < 0) return false;
  auto client = laghu::core::SocketHandle::adopt(client_descriptor);
  if (!client ||
      ::connect(client->native_handle(), reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
    return false;
  }

  std::array<laghu::core::SocketHandle, 1> accepted{};
  const auto result = laghu::os::drain_accept(
      listener->borrow(), accepted, {1, 2, std::chrono::seconds{1}});
  if (!result || result->accepted != 1U || result->operations != 2U ||
      !accepted[0].is_valid()) return false;
  const int status = ::fcntl(accepted[0].native_handle(), F_GETFL, 0);
  const int descriptor_flags = ::fcntl(accepted[0].native_handle(), F_GETFD, 0);
  return status >= 0 && (status & O_NONBLOCK) != 0 && descriptor_flags >= 0 &&
         (descriptor_flags & FD_CLOEXEC) != 0;
#else
  return true;
#endif
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"os.socket_drain.read", check_read_eintr_partial_eagain},
      laghu::test::TestCase{"os.socket_drain.write", check_write_partial_and_operation_budget},
      laghu::test::TestCase{"os.socket_drain.write_terminal_error",
                            check_write_partial_terminal_error},
      laghu::test::TestCase{"os.socket_drain.time_accept", check_time_and_accept_pressure},
      laghu::test::TestCase{"os.socket_drain.accept_progress", check_accept_progress_to_eagain},
      laghu::test::TestCase{"os.socket_drain.accept_pending_network_error",
                            check_accept_pending_network_error},
      laghu::test::TestCase{"os.socket_drain.blocking_listener",
                            check_blocking_listener_rejected},
      laghu::test::TestCase{"os.socket_drain.public_read_write",
                            check_public_read_write},
      laghu::test::TestCase{"os.socket_drain.public_accept_flags",
                            check_public_accept_flags},
  };
  return laghu::test::run_tests(tests);
}
