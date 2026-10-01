// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <span>

#include <sys/types.h>

#include <laghu/core/clocks.hpp>
#include <laghu/os/socket_drain.hpp>

namespace laghu::os::internal {

using DrainAcceptFunction = int (*)(void*, int) noexcept;
using DrainGetFlagsFunction = int (*)(void*, int) noexcept;
using DrainReadFunction = ssize_t (*)(void*, int, void*, std::size_t) noexcept;
using DrainWriteFunction = ssize_t (*)(void*, int, const void*, std::size_t) noexcept;

struct SocketDrainOperations final {
  void* context;
  DrainAcceptFunction accept;
  DrainGetFlagsFunction get_flags;
  DrainReadFunction read;
  DrainWriteFunction write;
  core::ClockOperations clock;
};

[[nodiscard]] const SocketDrainOperations& default_socket_drain_operations() noexcept;

class SocketDrainTestAccess final {
 public:
  [[nodiscard]] static core::Result<SocketDrainResult> read(
      core::BorrowedSocketHandle socket, core::MutableByteView output,
      SocketDrainBudget budget, const SocketDrainOperations& operations) noexcept;
  [[nodiscard]] static core::Result<SocketDrainResult> write(
      core::BorrowedSocketHandle socket, core::ByteView input,
      SocketDrainBudget budget, const SocketDrainOperations& operations) noexcept;
  [[nodiscard]] static core::Result<AcceptDrainResult> accept(
      core::BorrowedSocketHandle listener, std::span<core::SocketHandle> output,
      AcceptDrainBudget budget, const SocketDrainOperations& operations) noexcept;
};

}  // namespace laghu::os::internal
