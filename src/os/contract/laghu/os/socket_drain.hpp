// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

#include <laghu/core/handles.hpp>
#include <laghu/core/views.hpp>

namespace laghu::os {

namespace internal {
struct SocketDrainOperations;
class SocketDrainTestAccess;
}  // namespace internal

struct SocketDrainBudget final {
  std::size_t maximum_bytes;
  std::uint32_t maximum_operations;
  std::chrono::nanoseconds maximum_duration;
};

enum class SocketDrainState : std::uint8_t {
  would_block,
  peer_closed,
  peer_reset,
  broken_pipe,
  resource_pressure,
  budget_exhausted,
};

struct SocketDrainResult final {
  std::size_t bytes;
  std::uint32_t operations;
  SocketDrainState state;
};

struct AcceptDrainBudget final {
  std::uint32_t maximum_accepts;
  std::uint32_t maximum_operations;
  std::chrono::nanoseconds maximum_duration;
};

struct AcceptDrainResult final {
  std::size_t accepted;
  std::uint32_t operations;
  SocketDrainState state;
};

// These helpers are Linux event-loop primitives. Other POSIX targets return
// unavailable_capability; their native draining is introduced with kqueue.
[[nodiscard]] core::Result<SocketDrainResult> drain_socket_read(
    core::BorrowedSocketHandle socket, core::MutableByteView output,
    SocketDrainBudget budget) noexcept;
[[nodiscard]] core::Result<SocketDrainResult> drain_socket_write(
    core::BorrowedSocketHandle socket, core::ByteView input,
    SocketDrainBudget budget) noexcept;
[[nodiscard]] core::Result<AcceptDrainResult> drain_accept(
    core::BorrowedSocketHandle listener, std::span<core::SocketHandle> output,
    AcceptDrainBudget budget) noexcept;

}  // namespace laghu::os
