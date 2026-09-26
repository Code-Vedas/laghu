// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>

#include <sys/socket.h>
#include <sys/types.h>

#include <laghu/os/socket_io.hpp>

namespace laghu::os::internal {

using SocketReceiveFunction = ssize_t (*)(void*, int, void*, std::size_t, int) noexcept;
using SocketSendFunction = ssize_t (*)(void*, int, const void*, std::size_t, int) noexcept;
using SocketSendMessageFunction = ssize_t (*)(void*, int, const msghdr*, int) noexcept;
using SocketGetOptionFunction = int (*)(void*, int, int, int, void*, socklen_t*) noexcept;
using SocketSetOptionFunction = int (*)(void*, int, int, int, const void*, socklen_t) noexcept;
using SocketShutdownFunction = int (*)(void*, int, int) noexcept;

struct SocketIoOperations final {
  void* context;
  SocketReceiveFunction receive;
  SocketSendFunction send;
  SocketSendMessageFunction send_message;
  SocketGetOptionFunction get_option;
  SocketSetOptionFunction set_option;
  SocketShutdownFunction shutdown;
};

[[nodiscard]] const SocketIoOperations& default_socket_io_operations() noexcept;

class SocketIoTestAccess final {
 public:
  [[nodiscard]] static core::Result<SocketIoResult> read(
      core::BorrowedSocketHandle socket, core::MutableByteView output,
      SocketIoBudget budget, const SocketIoOperations& operations) noexcept;
  [[nodiscard]] static core::Result<SocketIoResult> write(
      core::BorrowedSocketHandle socket, core::ByteView input,
      SocketIoBudget budget, const SocketIoOperations& operations) noexcept;
  [[nodiscard]] static core::Result<SocketIoResult> write_vectored(
      core::BorrowedSocketHandle socket, core::IoSliceList& input,
      SocketIoBudget budget, const SocketIoOperations& operations) noexcept;
};

}  // namespace laghu::os::internal
