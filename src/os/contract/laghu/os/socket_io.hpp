// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>

#include <laghu/core/bounded_buffer.hpp>
#include <laghu/core/handles.hpp>

namespace laghu::os {

namespace internal {
struct SocketIoOperations;
class SocketIoTestAccess;
}  // namespace internal

struct SocketIoBudget final {
  std::size_t maximum_bytes;
  std::uint32_t maximum_syscalls;
};

enum class SocketIoState : std::uint8_t {
  progress,
  would_block,
  peer_closed,
  peer_reset,
  broken_pipe,
  budget_exhausted,
};

struct SocketIoResult final {
  std::size_t bytes;
  std::uint32_t syscalls;
  SocketIoState state;
};

[[nodiscard]] core::Result<SocketIoResult> read_socket(
    core::BorrowedSocketHandle socket, core::MutableByteView output,
    SocketIoBudget budget) noexcept;
[[nodiscard]] core::Result<SocketIoResult> write_socket(
    core::BorrowedSocketHandle socket, core::ByteView input,
    SocketIoBudget budget) noexcept;
[[nodiscard]] core::Result<SocketIoResult> write_socket_vectored(
    core::BorrowedSocketHandle socket, core::IoSliceList& input,
    SocketIoBudget budget) noexcept;

// Returns zero when no asynchronous socket error is pending. Otherwise the
// returned Error contains the pending POSIX error and its normalized class.
[[nodiscard]] core::Result<void> check_socket_error(
    core::BorrowedSocketHandle socket) noexcept;
[[nodiscard]] core::Result<void> shutdown_socket_write(
    core::BorrowedSocketHandle socket) noexcept;

}  // namespace laghu::os
