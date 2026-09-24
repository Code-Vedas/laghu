// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/os/wakeup.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/eventfd.h>
#endif

namespace {

using laghu::core::Error;
using laghu::core::ErrorCode;
using laghu::core::ErrorDomain;
using laghu::core::FileHandle;
using laghu::core::Result;
using laghu::os::WakeupChannel;
using laghu::os::WakeupMechanism;
using laghu::os::WakeupNotifyResult;
using laghu::os::WakeupObservation;

[[nodiscard]] Error closed_error() noexcept {
  return Error{ErrorDomain::core, ErrorCode::invalid_state, 0,
               "worker wakeup is closing or closed"};
}

[[nodiscard]] bool would_block(int native_error) noexcept {
#if EAGAIN == EWOULDBLOCK
  return native_error == EAGAIN;
#else
  return native_error == EAGAIN || native_error == EWOULDBLOCK;
#endif
}

[[nodiscard]] bool configure_pipe_descriptor(int descriptor) noexcept {
  const int status = ::fcntl(descriptor, F_GETFL, 0);
  if (status < 0 || ::fcntl(descriptor, F_SETFL, status | O_NONBLOCK) != 0) {
    return false;
  }
  const int flags = ::fcntl(descriptor, F_GETFD, 0);
  return flags >= 0 && ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

[[nodiscard]] bool create_configured_pipe(
    std::array<int, 2>& descriptors) noexcept {
  if (::pipe(descriptors.data()) != 0) {
    return false;
  }
  if (configure_pipe_descriptor(descriptors[0]) &&
      configure_pipe_descriptor(descriptors[1])) {
    return true;
  }
  const int native_error = errno;
  static_cast<void>(::close(descriptors[0]));
  static_cast<void>(::close(descriptors[1]));
  descriptors = {-1, -1};
  errno = native_error;
  return false;
}

[[nodiscard]] bool create_pipe(std::array<int, 2>& descriptors) noexcept {
#if defined(__linux__) || defined(__FreeBSD__)
  if (::pipe2(descriptors.data(), O_NONBLOCK | O_CLOEXEC) == 0) {
    return true;
  }
  if (errno != ENOSYS && errno != EINVAL) {
    return false;
  }
#endif
  return create_configured_pipe(descriptors);
}

}  // namespace

laghu::os::WakeupChannel::WakeupChannel(FileHandle&& read_handle,
                                        FileHandle&& write_handle,
                                        WakeupMechanism mechanism) noexcept
    : read_handle_(std::move(read_handle)),
      write_handle_(std::move(write_handle)),
      mechanism_(mechanism) {}

laghu::os::WakeupChannel::WakeupChannel(WakeupChannel&& other) noexcept
    : read_handle_(std::move(other.read_handle_)),
      write_handle_(std::move(other.write_handle_)),
      mechanism_(other.mechanism_),
      pending_(other.pending_.load()),
      closing_(other.closing_.load()),
      close_completed_(other.close_completed_.load()),
      close_error_(other.close_error_) {}

laghu::os::WakeupChannel::~WakeupChannel() { static_cast<void>(close()); }

Result<WakeupChannel> laghu::os::WakeupChannel::create() noexcept {
#if defined(__linux__)
  const int descriptor = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (descriptor >= 0) {
    auto read_handle = FileHandle::adopt(descriptor);
    if (!read_handle) {
      static_cast<void>(::close(descriptor));
      return std::unexpected{read_handle.error()};
    }
    return WakeupChannel{std::move(*read_handle), FileHandle{},
                         WakeupMechanism::event_counter};
  }
  if (errno != ENOSYS && errno != EINVAL) {
    return std::unexpected{Error::from_errno(errno, "eventfd wakeup creation failed")};
  }
#endif
  std::array<int, 2> descriptors{-1, -1};
  if (!create_pipe(descriptors)) {
    return std::unexpected{Error::from_errno(errno, "pipe wakeup creation failed")};
  }
  auto read_handle = FileHandle::adopt(descriptors[0]);
  auto write_handle = FileHandle::adopt(descriptors[1]);
  if (!read_handle || !write_handle) {
    if (read_handle) {
      static_cast<void>(read_handle->close());
    } else {
      static_cast<void>(::close(descriptors[0]));
    }
    if (write_handle) {
      static_cast<void>(write_handle->close());
    } else {
      static_cast<void>(::close(descriptors[1]));
    }
    return std::unexpected{!read_handle ? read_handle.error() : write_handle.error()};
  }
  return WakeupChannel{std::move(*read_handle), std::move(*write_handle),
                       WakeupMechanism::pipe};
}

bool laghu::os::WakeupChannel::begin_operation() const noexcept {
  if (closing_.load()) {
    return false;
  }
  active_operations_.fetch_add(1);
  if (!closing_.load()) {
    return true;
  }
  active_operations_.fetch_sub(1);
  return false;
}

void laghu::os::WakeupChannel::end_operation() const noexcept {
  active_operations_.fetch_sub(1);
}

int laghu::os::WakeupChannel::write_descriptor() const noexcept {
  return write_handle_.is_valid() ? write_handle_.native_handle()
                                  : read_handle_.native_handle();
}

Result<WakeupNotifyResult> laghu::os::WakeupChannel::notify() noexcept {
  if (!begin_operation()) {
    return std::unexpected{closed_error()};
  }
  OperationGuard guard{*this};

  if (pending_.exchange(true)) {
    return WakeupNotifyResult::coalesced;
  }

  const int descriptor = write_descriptor();
  const std::uint64_t counter = 1;
  const std::byte byte{1};
  const void* data = mechanism_ == WakeupMechanism::event_counter
                         ? static_cast<const void*>(&counter)
                         : static_cast<const void*>(&byte);
  const std::size_t size = mechanism_ == WakeupMechanism::event_counter
                               ? sizeof(counter)
                               : sizeof(byte);
  for (;;) {
    const ssize_t written = ::write(descriptor, data, size);
    if (written == static_cast<ssize_t>(size)) {
      return WakeupNotifyResult::signaled;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && would_block(errno)) {
      return WakeupNotifyResult::coalesced;
    }
    return std::unexpected{Error::from_errno(
        written < 0 ? errno : EIO, "worker wakeup notification failed")};
  }
}

Result<WakeupObservation> laghu::os::WakeupChannel::consume() noexcept {
  if (!begin_operation()) {
    return std::unexpected{closed_error()};
  }
  OperationGuard guard{*this};

  const bool was_pending = pending_.exchange(false);
  std::array<std::byte, 256> buffer{};
  bool drained = false;
  for (;;) {
    const std::size_t capacity = mechanism_ == WakeupMechanism::event_counter
                                     ? sizeof(std::uint64_t)
                                     : buffer.size();
    const ssize_t received =
        ::read(read_handle_.native_handle(), buffer.data(), capacity);
    if (received > 0) {
      drained = true;
      continue;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received < 0 && would_block(errno)) {
      break;
    }
    return std::unexpected{Error::from_errno(
        received < 0 ? errno : EIO, "worker wakeup consumption failed")};
  }
  return WakeupObservation{was_pending || drained, pending_.load()};
}

Result<int> laghu::os::WakeupChannel::notification_descriptor() const noexcept {
  if (!begin_operation()) {
    return std::unexpected{closed_error()};
  }
  OperationGuard guard{*this};
  if (!read_handle_.is_valid()) {
    return std::unexpected{closed_error()};
  }
  return read_handle_.native_handle();
}

Result<void> laghu::os::WakeupChannel::close() noexcept {
  bool expected = false;
  if (!closing_.compare_exchange_strong(expected, true)) {
    while (!close_completed_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    if (close_error_) {
      return std::unexpected{*close_error_};
    }
    return {};
  }
  while (active_operations_.load() != 0) {
    std::this_thread::yield();
  }
  pending_.store(false);
  const Result<void> write_closed = write_handle_.close();
  const Result<void> read_closed = read_handle_.close();
  if (!write_closed) {
    close_error_ = write_closed.error();
  } else if (!read_closed) {
    close_error_ = read_closed.error();
  }
  close_completed_.store(true, std::memory_order_release);
  if (close_error_) {
    return std::unexpected{*close_error_};
  }
  return {};
}
