// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/os/socket_io.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>

#include <sys/socket.h>
#include <sys/uio.h>

#include <laghu/os/internal/socket_io.hpp>

namespace laghu::os::internal {
namespace {

[[nodiscard]] ssize_t system_receive(void*, int descriptor, void* output,
                                     std::size_t size, int flags) noexcept {
  return ::recv(descriptor, output, size, flags);
}

[[nodiscard]] ssize_t system_send(void*, int descriptor, const void* input,
                                  std::size_t size, int flags) noexcept {
  return ::send(descriptor, input, size, flags);
}

[[nodiscard]] ssize_t system_send_message(void*, int descriptor,
                                          const msghdr* message,
                                          int flags) noexcept {
  return ::sendmsg(descriptor, message, flags);
}

[[nodiscard]] int system_get_option(void*, int descriptor, int level, int option,
                                    void* value, socklen_t* size) noexcept {
  return ::getsockopt(descriptor, level, option, value, size);
}

[[nodiscard]] int system_set_option(void*, int descriptor, int level, int option,
                                    const void* value, socklen_t size) noexcept {
  return ::setsockopt(descriptor, level, option, value, size);
}

[[nodiscard]] int system_shutdown(void*, int descriptor, int how) noexcept {
  return ::shutdown(descriptor, how);
}

constexpr SocketIoOperations default_operations{
    nullptr, system_receive, system_send, system_send_message,
    system_get_option, system_set_option, system_shutdown};

}  // namespace

const SocketIoOperations& default_socket_io_operations() noexcept {
  return default_operations;
}

}  // namespace laghu::os::internal

namespace laghu::os {
namespace {

constexpr std::size_t maximum_iovecs = 64;

[[nodiscard]] core::Error invalid(const char* diagnostic) noexcept {
  return core::Error{core::ErrorDomain::core, core::ErrorCode::invalid_input, 0,
                     diagnostic};
}

[[nodiscard]] core::Result<void> validate(
    core::BorrowedSocketHandle socket, SocketIoBudget budget,
    const internal::SocketIoOperations& operations) noexcept {
  if (!socket.is_valid() || budget.maximum_bytes == 0 ||
      budget.maximum_syscalls == 0 || operations.receive == nullptr ||
      operations.send == nullptr || operations.send_message == nullptr ||
      operations.get_option == nullptr || operations.set_option == nullptr ||
      operations.shutdown == nullptr) {
    return std::unexpected{invalid("socket I/O arguments are invalid")};
  }
  return {};
}

[[nodiscard]] constexpr int receive_flags() noexcept {
#if defined(MSG_DONTWAIT)
  return MSG_DONTWAIT;
#else
  return 0;
#endif
}

[[nodiscard]] constexpr int send_flags() noexcept {
  int flags = receive_flags();
#if defined(MSG_NOSIGNAL)
  flags |= MSG_NOSIGNAL;
#endif
  return flags;
}

[[nodiscard]] core::Result<void> suppress_sigpipe(
    int descriptor, const internal::SocketIoOperations& operations) noexcept {
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
  const int enabled = 1;
  if (operations.set_option(operations.context, descriptor, SOL_SOCKET,
                            SO_NOSIGPIPE, &enabled,
                            static_cast<socklen_t>(sizeof(enabled))) != 0) {
    return std::unexpected{
        core::Error::from_errno(errno, "socket SIGPIPE suppression failed")};
  }
#else
  static_cast<void>(descriptor);
  static_cast<void>(operations);
#endif
  return {};
}

[[nodiscard]] SocketIoResult expected_result(ssize_t result,
                                             std::uint32_t calls,
                                             bool reading) noexcept {
  if (result > 0) {
    return SocketIoResult{static_cast<std::size_t>(result), calls,
                          SocketIoState::progress};
  }
  if (result == 0) {
    return SocketIoResult{0, calls, reading ? SocketIoState::peer_closed
                                            : SocketIoState::progress};
  }
  if (errno == EAGAIN
#if EWOULDBLOCK != EAGAIN
      || errno == EWOULDBLOCK
#endif
  ) {
    return SocketIoResult{0, calls, SocketIoState::would_block};
  }
  if (errno == ECONNRESET) {
    return SocketIoResult{0, calls, SocketIoState::peer_reset};
  }
  if (errno == EPIPE) {
    return SocketIoResult{0, calls, SocketIoState::broken_pipe};
  }
  return SocketIoResult{0, calls, SocketIoState::budget_exhausted};
}

template <class Operation>
[[nodiscard]] core::Result<SocketIoResult> invoke_bounded(
    SocketIoBudget budget, bool reading, Operation operation) noexcept {
  for (std::uint32_t index = 0; index < budget.maximum_syscalls; ++index) {
    const std::uint32_t calls = index + 1U;
    const ssize_t result = operation();
    if (result >= 0 || errno == EAGAIN
#if EWOULDBLOCK != EAGAIN
        || errno == EWOULDBLOCK
#endif
        || errno == ECONNRESET || errno == EPIPE) {
      return expected_result(result, calls, reading);
    }
    if (errno != EINTR) {
      return std::unexpected{
          core::Error::from_errno(errno, "nonblocking socket I/O failed")};
    }
  }
  return SocketIoResult{0, budget.maximum_syscalls,
                        SocketIoState::budget_exhausted};
}

[[nodiscard]] core::Result<SocketIoResult> read_with_operations(
    core::BorrowedSocketHandle socket, core::MutableByteView output,
    SocketIoBudget budget,
    const internal::SocketIoOperations& operations) noexcept {
  if (output.empty()) {
    return SocketIoResult{0, 0, SocketIoState::progress};
  }
  if (const auto valid = validate(socket, budget, operations); !valid) {
    return std::unexpected{valid.error()};
  }
  const std::size_t size = std::min(output.size(), budget.maximum_bytes);
  return invoke_bounded(budget, true, [&]() noexcept {
    return operations.receive(operations.context, socket.native_handle(),
                              output.data(), size, receive_flags());
  });
}

[[nodiscard]] core::Result<SocketIoResult> write_with_operations(
    core::BorrowedSocketHandle socket, core::ByteView input,
    SocketIoBudget budget,
    const internal::SocketIoOperations& operations) noexcept {
  if (input.empty()) {
    return SocketIoResult{0, 0, SocketIoState::progress};
  }
  if (const auto valid = validate(socket, budget, operations); !valid) {
    return std::unexpected{valid.error()};
  }
  if (const auto suppressed = suppress_sigpipe(socket.native_handle(), operations);
      !suppressed) {
    return std::unexpected{suppressed.error()};
  }
  const std::size_t size = std::min(input.size(), budget.maximum_bytes);
  return invoke_bounded(budget, false, [&]() noexcept {
    return operations.send(operations.context, socket.native_handle(),
                           input.data(), size, send_flags());
  });
}

[[nodiscard]] core::Result<SocketIoResult> write_vectored_with_operations(
    core::BorrowedSocketHandle socket, core::IoSliceList& input,
    SocketIoBudget budget,
    const internal::SocketIoOperations& operations) noexcept {
  if (input.empty()) {
    return SocketIoResult{0, 0, SocketIoState::progress};
  }
  if (const auto valid = validate(socket, budget, operations); !valid) {
    return std::unexpected{valid.error()};
  }
  if (input.size() > maximum_iovecs) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::exhaustion, 0,
                                       "socket I/O vector limit exceeded"}};
  }
  if (const auto suppressed = suppress_sigpipe(socket.native_handle(), operations);
      !suppressed) {
    return std::unexpected{suppressed.error()};
  }

  std::array<iovec, maximum_iovecs> vectors{};
  std::size_t count{};
  std::size_t remaining = budget.maximum_bytes;
  for (const core::IoSlice& slice : input.slices()) {
    if (remaining == 0) {
      break;
    }
    const std::size_t size = std::min(slice.size(), remaining);
    vectors[count] = iovec{const_cast<std::byte*>(slice.bytes().data()), size};
    ++count;
    remaining -= size;
  }
  msghdr message{};
  message.msg_iov = vectors.data();
  message.msg_iovlen = static_cast<decltype(message.msg_iovlen)>(count);
  auto result = invoke_bounded(budget, false, [&]() noexcept {
    return operations.send_message(operations.context, socket.native_handle(),
                                   &message, send_flags());
  });
  if (result && result->bytes != 0) {
    if (const auto consumed = input.consume(result->bytes); !consumed) {
      return std::unexpected{consumed.error()};
    }
  }
  return result;
}

}  // namespace

core::Result<SocketIoResult> read_socket(core::BorrowedSocketHandle socket,
                                         core::MutableByteView output,
                                         SocketIoBudget budget) noexcept {
  return read_with_operations(socket, output, budget,
                              internal::default_socket_io_operations());
}

core::Result<SocketIoResult> write_socket(core::BorrowedSocketHandle socket,
                                          core::ByteView input,
                                          SocketIoBudget budget) noexcept {
  return write_with_operations(socket, input, budget,
                               internal::default_socket_io_operations());
}

core::Result<SocketIoResult> write_socket_vectored(
    core::BorrowedSocketHandle socket, core::IoSliceList& input,
    SocketIoBudget budget) noexcept {
  return write_vectored_with_operations(socket, input, budget,
                                        internal::default_socket_io_operations());
}

core::Result<void> check_socket_error(core::BorrowedSocketHandle socket) noexcept {
  if (!socket.is_valid()) {
    return std::unexpected{invalid("socket error query has an invalid socket")};
  }
  int pending{};
  socklen_t size = sizeof(pending);
  const auto& operations = internal::default_socket_io_operations();
  if (operations.get_option(operations.context, socket.native_handle(), SOL_SOCKET,
                            SO_ERROR, &pending, &size) != 0) {
    return std::unexpected{
        core::Error::from_errno(errno, "socket error query failed")};
  }
  if (size != sizeof(pending)) {
    return std::unexpected{invalid("socket error query returned an invalid size")};
  }
  if (pending != 0) {
    return std::unexpected{
        core::Error::from_errno(pending, "socket has a pending error")};
  }
  return {};
}

core::Result<void> shutdown_socket_write(core::BorrowedSocketHandle socket) noexcept {
  if (!socket.is_valid()) {
    return std::unexpected{invalid("socket shutdown has an invalid socket")};
  }
  const auto& operations = internal::default_socket_io_operations();
  if (operations.shutdown(operations.context, socket.native_handle(), SHUT_WR) != 0) {
    return std::unexpected{
        core::Error::from_errno(errno, "socket write shutdown failed")};
  }
  return {};
}

core::Result<SocketIoResult> internal::SocketIoTestAccess::read(
    core::BorrowedSocketHandle socket, core::MutableByteView output,
    SocketIoBudget budget, const SocketIoOperations& operations) noexcept {
  return read_with_operations(socket, output, budget, operations);
}

core::Result<SocketIoResult> internal::SocketIoTestAccess::write(
    core::BorrowedSocketHandle socket, core::ByteView input,
    SocketIoBudget budget, const SocketIoOperations& operations) noexcept {
  return write_with_operations(socket, input, budget, operations);
}

core::Result<SocketIoResult> internal::SocketIoTestAccess::write_vectored(
    core::BorrowedSocketHandle socket, core::IoSliceList& input,
    SocketIoBudget budget, const SocketIoOperations& operations) noexcept {
  return write_vectored_with_operations(socket, input, budget, operations);
}

}  // namespace laghu::os
