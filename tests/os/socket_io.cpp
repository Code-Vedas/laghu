// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "laghu_test_support.hpp"

#include <laghu/core/handles.hpp>
#include <laghu/core/views.hpp>
#include <laghu/os/internal/socket_io.hpp>
#include <laghu/os/socket_io.hpp>

namespace {

using laghu::core::ByteView;
using laghu::core::IoSlice;
using laghu::core::IoSliceList;
using laghu::core::MutableByteView;
using laghu::core::SocketHandle;
using laghu::os::SocketIoBudget;
using laghu::os::SocketIoState;
using laghu::os::internal::SocketIoOperations;
using laghu::os::internal::SocketIoTestAccess;

struct SocketPair final {
  SocketHandle first;
  SocketHandle second;
};

[[nodiscard]] laghu::core::Result<SocketPair> make_pair() noexcept {
  int descriptors[2]{};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
    return std::unexpected{laghu::core::Error::from_errno(errno, "socketpair failed")};
  }
  auto first = SocketHandle::adopt(descriptors[0]);
  auto second = SocketHandle::adopt(descriptors[1]);
  if (!first || !second) {
    static_cast<void>(::close(descriptors[0]));
    static_cast<void>(::close(descriptors[1]));
    return std::unexpected{laghu::core::Error::from_errno(EINVAL, "socket adoption failed")};
  }
  return SocketPair{std::move(*first), std::move(*second)};
}

[[nodiscard]] bool set_nonblocking(int descriptor) noexcept {
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  return flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

[[nodiscard]] bool check_socketpair_io() noexcept {
  auto pair = make_pair();
  if (!pair || !set_nonblocking(pair->first.native_handle()) ||
      !set_nonblocking(pair->second.native_handle())) {
    return false;
  }
  std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const auto input = ByteView::from(bytes);
  std::array<std::byte, 4> received{};
  const auto output = MutableByteView::from(received);
  if (!input || !output) {
    return false;
  }
  const auto written = laghu::os::write_socket(pair->first.borrow(), *input, {2, 1});
  const auto read = laghu::os::read_socket(pair->second.borrow(), *output, {4, 1});
  const auto blocked = laghu::os::read_socket(pair->second.borrow(), *output, {4, 1});
  return written && written->bytes == 2 && written->state == SocketIoState::progress &&
         read && read->bytes == 2 && read->state == SocketIoState::progress &&
         blocked && blocked->state == SocketIoState::would_block;
}

[[nodiscard]] bool check_half_close_and_broken_pipe() noexcept {
  auto pair = make_pair();
  if (!pair || !set_nonblocking(pair->first.native_handle()) ||
      !set_nonblocking(pair->second.native_handle()) ||
      !laghu::os::shutdown_socket_write(pair->first.borrow())) {
    return false;
  }
  std::array<std::byte, 1> storage{};
  const auto output = MutableByteView::from(storage);
  if (!output) {
    return false;
  }
  const auto closed = laghu::os::read_socket(pair->second.borrow(), *output, {1, 1});
  if (!closed || closed->state != SocketIoState::peer_closed) {
    return false;
  }
  if (!pair->second.close()) {
    return false;
  }
  const auto input = ByteView::from(storage);
  const auto broken = laghu::os::write_socket(pair->first.borrow(), *input, {1, 1});
  return broken && broken->state == SocketIoState::broken_pipe;
}

struct Injected final {
  std::uint32_t calls{};
  int error{};
  std::uint32_t interrupted{};
  ssize_t result{};
};

[[nodiscard]] ssize_t injected_receive(void* context, int, void*, std::size_t,
                                       int) noexcept {
  auto& state = *static_cast<Injected*>(context);
  ++state.calls;
  if (state.calls <= state.interrupted) {
    errno = EINTR;
    return -1;
  }
  if (state.error != 0) {
    errno = state.error;
    return -1;
  }
  return state.result;
}

[[nodiscard]] ssize_t injected_send(void* context, int descriptor,
                                    const void* input, std::size_t size,
                                    int flags) noexcept {
  return injected_receive(context, descriptor, const_cast<void*>(input), size, flags);
}

[[nodiscard]] ssize_t injected_sendmsg(void* context, int descriptor,
                                       const msghdr*, int flags) noexcept {
  return injected_receive(context, descriptor, nullptr, 0, flags);
}

[[nodiscard]] int injected_get_option(void*, int, int, int, void*, socklen_t*) noexcept {
  return 0;
}

[[nodiscard]] int injected_set_option(void*, int, int, int, const void*, socklen_t) noexcept {
  return 0;
}

[[nodiscard]] int injected_shutdown(void*, int, int) noexcept { return 0; }

[[nodiscard]] SocketIoOperations operations(Injected& state) noexcept {
  return SocketIoOperations{&state, injected_receive, injected_send, injected_sendmsg,
                            injected_get_option, injected_set_option, injected_shutdown};
}

[[nodiscard]] bool check_bounded_eintr_and_reset() noexcept {
  auto pair = make_pair();
  std::array<std::byte, 1> storage{};
  const auto output = MutableByteView::from(storage);
  if (!pair || !output) {
    return false;
  }
  Injected interrupted{0, 0, 3, 1};
  auto interrupted_operations = operations(interrupted);
  const auto exhausted = SocketIoTestAccess::read(pair->first.borrow(), *output,
                                                   {1, 2}, interrupted_operations);
  Injected reset{0, ECONNRESET, 0, 0};
  auto reset_operations = operations(reset);
  const auto reset_result = SocketIoTestAccess::read(pair->first.borrow(), *output,
                                                     {1, 1}, reset_operations);
  return exhausted && exhausted->state == SocketIoState::budget_exhausted &&
         exhausted->syscalls == 2 && interrupted.calls == 2 && reset_result &&
         reset_result->state == SocketIoState::peer_reset;
}

[[nodiscard]] bool check_vectored_partial_consumption() noexcept {
  auto pair = make_pair();
  std::array<std::byte, 2> first{};
  std::array<std::byte, 3> second{};
  const auto first_view = ByteView::from(first);
  const auto second_view = ByteView::from(second);
  std::array<IoSlice, 2> storage{};
  IoSliceList slices{storage};
  if (!pair || !first_view || !second_view || !slices.append(*first_view) ||
      !slices.append(*second_view)) {
    return false;
  }
  Injected state{0, 0, 0, 3};
  auto injected = operations(state);
  const auto result = SocketIoTestAccess::write_vectored(
      pair->first.borrow(), slices, {5, 1}, injected);
  return result && result->bytes == 3 && result->state == SocketIoState::progress &&
         slices.size() == 1 && slices.bytes() == 2;
}

[[nodiscard]] bool check_invalid_budget() noexcept {
  auto pair = make_pair();
  std::array<std::byte, 1> storage{};
  const auto input = ByteView::from(storage);
  if (!pair || !input) {
    return false;
  }
  const auto result = laghu::os::write_socket(pair->first.borrow(), *input, {0, 1});
  return !result && result.error().code() == laghu::core::ErrorCode::invalid_input;
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"os.socket_io.partial_and_backpressure", check_socketpair_io},
      laghu::test::TestCase{"os.socket_io.half_close_and_sigpipe", check_half_close_and_broken_pipe},
      laghu::test::TestCase{"os.socket_io.bounded_eintr_and_reset", check_bounded_eintr_and_reset},
      laghu::test::TestCase{"os.socket_io.vectored_partial", check_vectored_partial_consumption},
      laghu::test::TestCase{"os.socket_io.invalid_budget", check_invalid_budget},
  };
  return laghu::test::run_tests(tests);
}
