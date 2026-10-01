// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/os/socket_drain.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <laghu/core/clocks.hpp>
#include <laghu/os/internal/socket_drain.hpp>

namespace laghu::os::internal {
namespace {

[[nodiscard]] int system_accept(void*, int descriptor) noexcept {
#if defined(__linux__) && defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
  return ::accept4(descriptor, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
  return ::accept(descriptor, nullptr, nullptr);
#endif
}
[[nodiscard]] ssize_t system_read(void*, int descriptor, void* output,
                                  std::size_t size) noexcept {
  return ::recv(descriptor, output, size, MSG_DONTWAIT);
}
[[nodiscard]] ssize_t system_write(void*, int descriptor, const void* input,
                                   std::size_t size) noexcept {
  int flags = MSG_DONTWAIT;
#if defined(MSG_NOSIGNAL)
  flags |= MSG_NOSIGNAL;
#endif
  return ::send(descriptor, input, size, flags);
}

const SocketDrainOperations default_operations{
    nullptr, system_accept, system_read, system_write,
    core::system_clock_operations()};

}  // namespace

const SocketDrainOperations& default_socket_drain_operations() noexcept {
  return default_operations;
}

}  // namespace laghu::os::internal

namespace laghu::os {
namespace {

[[nodiscard]] core::Error invalid(const char* text) noexcept {
  return {core::ErrorDomain::core, core::ErrorCode::invalid_input, 0, text};
}

#if !defined(__linux__)
[[nodiscard]] core::Error unavailable() noexcept {
  return {core::ErrorDomain::core, core::ErrorCode::unavailable_capability, 0,
          "Linux socket draining is unavailable on this platform"};
}
#endif

[[nodiscard]] bool would_block(int code) noexcept {
  return code == EAGAIN
#if EWOULDBLOCK != EAGAIN
         || code == EWOULDBLOCK
#endif
      ;
}

[[nodiscard]] bool pending_accept_network_error(int code) noexcept {
#if defined(__linux__)
  return code == ENETDOWN || code == EPROTO || code == ENOPROTOOPT ||
         code == EHOSTDOWN || code == ENONET || code == EHOSTUNREACH ||
         code == EOPNOTSUPP || code == ENETUNREACH;
#else
  static_cast<void>(code);
  return false;
#endif
}

[[nodiscard]] core::Result<bool> expired(
    core::MonotonicInstant started, std::chrono::nanoseconds maximum_duration,
    const core::ClockOperations& clock) noexcept {
  const auto now = core::read_monotonic_clock(clock);
  if (!now) return std::unexpected{now.error()};
  const auto duration = static_cast<std::uint64_t>(maximum_duration.count());
  return *now < started || *now - started >= duration;
}

[[nodiscard]] core::Result<void> validate(SocketDrainBudget budget,
                                           const internal::SocketDrainOperations& ops) noexcept {
  if (budget.maximum_bytes == 0U || budget.maximum_operations == 0U ||
      budget.maximum_duration.count() <= 0 || ops.read == nullptr ||
      ops.write == nullptr || ops.clock.monotonic_now == nullptr) {
    return std::unexpected{invalid("socket drain arguments are invalid")};
  }
  return {};
}

template <class View, class Invoke>
[[nodiscard]] core::Result<SocketDrainResult> drain_io(
    View bytes, SocketDrainBudget budget,
    const internal::SocketDrainOperations& operations, bool reading,
    Invoke invoke) noexcept {
  if (const auto valid = validate(budget, operations); !valid) return std::unexpected{valid.error()};
  const auto started = core::read_monotonic_clock(operations.clock);
  if (!started) return std::unexpected{started.error()};
  const std::size_t limit = std::min(bytes.size(), budget.maximum_bytes);
  std::size_t transferred{};
  std::uint32_t calls{};
  while (transferred < limit && calls < budget.maximum_operations) {
    const auto timed_out = expired(*started, budget.maximum_duration, operations.clock);
    if (!timed_out) {
      if (transferred == 0U && calls == 0U) {
        return std::unexpected{timed_out.error()};
      }
      return SocketDrainResult{transferred, calls, SocketDrainState::failed,
                               timed_out.error()};
    }
    if (*timed_out) return SocketDrainResult{transferred, calls, SocketDrainState::budget_exhausted};
    ++calls;
    const auto remaining = bytes.span().subspan(transferred, limit - transferred);
    const ssize_t result = invoke(remaining.data(), remaining.size());
    if (result > 0) {
      transferred += static_cast<std::size_t>(result);
      continue;
    }
    if (result == 0) {
      return SocketDrainResult{transferred, calls,
                               reading ? SocketDrainState::peer_closed
                                       : SocketDrainState::budget_exhausted};
    }
    const int code = errno;
    if (code == EINTR) continue;
    if (would_block(code)) return SocketDrainResult{transferred, calls, SocketDrainState::would_block};
    if (code == ECONNRESET) return SocketDrainResult{transferred, calls, SocketDrainState::peer_reset};
    if (code == EPIPE) return SocketDrainResult{transferred, calls, SocketDrainState::broken_pipe};
    return SocketDrainResult{transferred, calls, SocketDrainState::failed,
                             core::Error::from_errno(code, "socket drain failed")};
  }
  return SocketDrainResult{transferred, calls, SocketDrainState::budget_exhausted};
}

[[nodiscard]] core::Result<SocketDrainResult> read_with_operations(
    core::BorrowedSocketHandle socket, core::MutableByteView output,
    SocketDrainBudget budget, const internal::SocketDrainOperations& operations) noexcept {
  if (!socket.is_valid()) return std::unexpected{invalid("socket drain descriptor is invalid")};
  return drain_io(output, budget, operations, true, [&](std::byte* data, std::size_t size) noexcept {
    return operations.read(operations.context, socket.native_handle(), data, size);
  });
}

[[nodiscard]] core::Result<SocketDrainResult> write_with_operations(
    core::BorrowedSocketHandle socket, core::ByteView input,
    SocketDrainBudget budget, const internal::SocketDrainOperations& operations) noexcept {
  if (!socket.is_valid()) return std::unexpected{invalid("socket drain descriptor is invalid")};
  return drain_io(input, budget, operations, false, [&](const std::byte* data, std::size_t size) noexcept {
    return operations.write(operations.context, socket.native_handle(), data, size);
  });
}

[[nodiscard]] core::Result<AcceptDrainResult> accept_with_operations(
    core::BorrowedSocketHandle listener, std::span<core::SocketHandle> output,
    AcceptDrainBudget budget, const internal::SocketDrainOperations& operations) noexcept {
  if (!listener.is_valid() || output.empty() || budget.maximum_accepts == 0U ||
      budget.maximum_accepts > output.size() || budget.maximum_operations == 0U ||
      budget.maximum_duration.count() <= 0 || operations.accept == nullptr ||
      operations.clock.monotonic_now == nullptr) {
    return std::unexpected{invalid("accept drain arguments are invalid")};
  }
  const auto started = core::read_monotonic_clock(operations.clock);
  if (!started) return std::unexpected{started.error()};
  std::size_t accepted{};
  std::uint32_t calls{};
  while (accepted < budget.maximum_accepts && calls < budget.maximum_operations) {
    const auto timed_out = expired(*started, budget.maximum_duration, operations.clock);
    if (!timed_out) {
      if (accepted == 0U && calls == 0U) {
        return std::unexpected{timed_out.error()};
      }
      return AcceptDrainResult{accepted, calls, SocketDrainState::failed,
                               timed_out.error()};
    }
    if (*timed_out) return AcceptDrainResult{accepted, calls, SocketDrainState::budget_exhausted};
    ++calls;
    const int descriptor = operations.accept(operations.context, listener.native_handle());
    if (descriptor >= 0) {
      auto handle = core::SocketHandle::adopt(descriptor);
      if (!handle) {
        static_cast<void>(::close(descriptor));
        return AcceptDrainResult{accepted, calls, SocketDrainState::failed,
                                 handle.error()};
      }
      output[accepted] = std::move(*handle);
      ++accepted;
      continue;
    }
    const int code = errno;
    if (code == EINTR) continue;
    if (would_block(code)) return AcceptDrainResult{accepted, calls, SocketDrainState::would_block};
    if (pending_accept_network_error(code)) continue;
    if (code == EMFILE || code == ENFILE) {
      return AcceptDrainResult{accepted, calls, SocketDrainState::resource_pressure};
    }
    return AcceptDrainResult{accepted, calls, SocketDrainState::failed,
                             core::Error::from_errno(code, "accept drain failed")};
  }
  return AcceptDrainResult{accepted, calls, SocketDrainState::budget_exhausted};
}

}  // namespace

core::Result<SocketDrainResult> drain_socket_read(core::BorrowedSocketHandle socket,
    core::MutableByteView output, SocketDrainBudget budget) noexcept {
#if defined(__linux__)
  return read_with_operations(socket, output, budget, internal::default_socket_drain_operations());
#else
  static_cast<void>(socket); static_cast<void>(output); static_cast<void>(budget);
  return std::unexpected{unavailable()};
#endif
}
core::Result<SocketDrainResult> drain_socket_write(core::BorrowedSocketHandle socket,
    core::ByteView input, SocketDrainBudget budget) noexcept {
#if defined(__linux__)
  return write_with_operations(socket, input, budget, internal::default_socket_drain_operations());
#else
  static_cast<void>(socket); static_cast<void>(input); static_cast<void>(budget);
  return std::unexpected{unavailable()};
#endif
}
core::Result<AcceptDrainResult> drain_accept(core::BorrowedSocketHandle listener,
    std::span<core::SocketHandle> output, AcceptDrainBudget budget) noexcept {
#if defined(__linux__)
  if (!listener.is_valid()) {
    return std::unexpected{invalid("accept drain descriptor is invalid")};
  }
  const int flags = ::fcntl(listener.native_handle(), F_GETFL, 0);
  if (flags < 0) {
    return std::unexpected{
        core::Error::from_errno(errno, "accept drain listener query failed")};
  }
  if ((flags & O_NONBLOCK) == 0) {
    return std::unexpected{
        invalid("accept drain listener must be nonblocking")};
  }
  return accept_with_operations(listener, output, budget, internal::default_socket_drain_operations());
#else
  static_cast<void>(listener); static_cast<void>(output); static_cast<void>(budget);
  return std::unexpected{unavailable()};
#endif
}
core::Result<SocketDrainResult> internal::SocketDrainTestAccess::read(
    core::BorrowedSocketHandle socket, core::MutableByteView output, SocketDrainBudget budget,
    const SocketDrainOperations& operations) noexcept {
  return read_with_operations(socket, output, budget, operations);
}
core::Result<SocketDrainResult> internal::SocketDrainTestAccess::write(
    core::BorrowedSocketHandle socket, core::ByteView input, SocketDrainBudget budget,
    const SocketDrainOperations& operations) noexcept {
  return write_with_operations(socket, input, budget, operations);
}
core::Result<AcceptDrainResult> internal::SocketDrainTestAccess::accept(
    core::BorrowedSocketHandle listener, std::span<core::SocketHandle> output,
    AcceptDrainBudget budget, const SocketDrainOperations& operations) noexcept {
  return accept_with_operations(listener, output, budget, operations);
}

}  // namespace laghu::os
