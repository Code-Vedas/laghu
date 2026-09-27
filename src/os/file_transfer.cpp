// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/os/file_transfer.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/stat.h>
#include <linux/tls.h>
#include <pthread.h>
#include <sys/sendfile.h>
#include <sys/syscall.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/uio.h>
#endif

#include <laghu/os/internal/file_transfer.hpp>

namespace laghu::os::internal {
namespace {

constexpr int operation_complete = 0;
constexpr int operation_error = -1;
constexpr int operation_budget_exhausted = 1;
#if defined(__linux__)
constexpr ssize_t transfer_budget_exhausted = -2;
#endif

[[nodiscard]] ssize_t system_pread(void*, int descriptor, void* output,
                                   std::size_t size, std::uint64_t offset) noexcept {
  return ::pread(descriptor, output, size, static_cast<off_t>(offset));
}

[[nodiscard]] ssize_t system_send(void*, int descriptor, const void* input,
                                  std::size_t size, int flags) noexcept {
  return ::send(descriptor, input, size, flags);
}

[[nodiscard]] int system_prepare_destination(void*, int descriptor,
                                             std::uint32_t maximum_calls,
                                             std::uint32_t* calls) noexcept {
  *calls = 0;
  if (maximum_calls == 0) {
    return operation_budget_exhausted;
  }
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  *calls = 1U;
  if (flags < 0 || (flags & O_NONBLOCK) == 0) {
    if (flags >= 0) {
      errno = EINVAL;
    }
    return operation_error;
  }
#if defined(SO_NOSIGPIPE)
  if (maximum_calls < 2U) {
    return operation_budget_exhausted;
  }
  const int enabled = 1;
  const int result = ::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                                  static_cast<socklen_t>(sizeof(enabled)));
  *calls = 2U;
  return result == 0 ? operation_complete : operation_error;
#else
  static_cast<void>(descriptor);
  return operation_complete;
#endif
}

#if defined(__linux__)
[[nodiscard]] ssize_t linux_sendfile_without_sigpipe(int output, int input,
                                                     off_t* offset,
                                                     std::size_t size,
                                                     std::uint32_t maximum_calls,
                                                     std::uint32_t* calls) noexcept {
  *calls = 0;
  if (maximum_calls < 5U) {
    errno = EAGAIN;
    return transfer_budget_exhausted;
  }
  sigset_t blocked{};
  sigset_t previous{};
  sigset_t pending{};
  if (::sigemptyset(&blocked) != 0 || ::sigaddset(&blocked, SIGPIPE) != 0) {
    return -1;
  }
  const int block_error = ::pthread_sigmask(SIG_BLOCK, &blocked, &previous);
  *calls = 1U;
  if (block_error != 0) {
    errno = block_error;
    return -1;
  }
  if (::sigpending(&pending) != 0) {
    const int pending_error = errno;
    static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous, nullptr));
    *calls = 3U;
    errno = pending_error;
    return -1;
  }
  *calls = 2U;
  const int pending_member = ::sigismember(&pending, SIGPIPE);
  if (pending_member < 0) {
    const int pending_error = errno;
    static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous, nullptr));
    *calls = 3U;
    errno = pending_error;
    return -1;
  }
  const bool already_pending = pending_member == 1;
  const ssize_t result = ::sendfile(output, input, offset, size);
  *calls = 3U;
  const int sendfile_error = errno;
  if (result < 0 && sendfile_error == EPIPE && !already_pending) {
    timespec timeout{};
    while (::sigtimedwait(&blocked, nullptr, &timeout) < 0 && errno == EINTR) {
    }
    *calls = 4U;
  }
  const int restore_error = ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  ++*calls;
  if (result < 0) {
    errno = sendfile_error;
  } else if (restore_error != 0) {
    errno = restore_error;
  }
  return result;
}
#endif

[[nodiscard]] int system_kernel_transfer(void*, int output, int input,
                                         std::uint64_t offset, std::size_t size,
                                         std::uint32_t maximum_calls,
                                         std::size_t* transferred,
                                         std::uint32_t* calls) noexcept {
#if defined(__linux__)
  off_t native_offset = static_cast<off_t>(offset);
  const ssize_t result = linux_sendfile_without_sigpipe(
      output, input, &native_offset, size, maximum_calls, calls);
  if (result == transfer_budget_exhausted) {
    *transferred = 0;
    return operation_budget_exhausted;
  }
  if (result >= 0) {
    *transferred = static_cast<std::size_t>(result);
    return operation_complete;
  }
  *transferred = 0;
  return operation_error;
#elif defined(__APPLE__)
  *calls = 0;
  if (maximum_calls == 0) {
    *transferred = 0;
    return operation_budget_exhausted;
  }
  off_t sent = static_cast<off_t>(size);
  const int result = ::sendfile(input, output, static_cast<off_t>(offset), &sent,
                                nullptr, 0);
  *transferred = sent > 0 ? static_cast<std::size_t>(sent) : 0U;
  *calls = 1U;
  return result == 0 ? operation_complete : operation_error;
#elif defined(__FreeBSD__)
  *calls = 0;
  if (maximum_calls == 0) {
    *transferred = 0;
    return operation_budget_exhausted;
  }
  off_t sent{};
  const int result = ::sendfile(input, output, static_cast<off_t>(offset), size,
                                nullptr, &sent, 0);
  *transferred = sent > 0 ? static_cast<std::size_t>(sent) : 0U;
  *calls = 1U;
  return result == 0 ? operation_complete : operation_error;
#else
  static_cast<void>(output);
  static_cast<void>(input);
  static_cast<void>(offset);
  static_cast<void>(size);
  static_cast<void>(maximum_calls);
  *transferred = 0;
  *calls = 0;
  errno = ENOSYS;
  return operation_error;
#endif
}

constexpr FileTransferOperations default_operations{
    nullptr, system_pread, system_send, system_kernel_transfer,
    system_prepare_destination};

}  // namespace

const FileTransferOperations& default_file_transfer_operations() noexcept {
  return default_operations;
}

}  // namespace laghu::os::internal

namespace laghu::os {
namespace {

constexpr std::size_t generic_buffer_size = 16384U;
#if defined(O_DIRECT)
constexpr std::size_t direct_buffer_alignment = 65536U;
constexpr std::size_t direct_buffer_size = 65536U;
#endif

[[nodiscard]] constexpr int transfer_send_flags() noexcept {
  int flags{};
#if defined(MSG_DONTWAIT)
  flags |= MSG_DONTWAIT;
#endif
#if defined(MSG_NOSIGNAL)
  flags |= MSG_NOSIGNAL;
#endif
  return flags;
}

[[nodiscard]] core::Error invalid(const char* diagnostic) noexcept {
  return core::Error{core::ErrorDomain::core, core::ErrorCode::invalid_input, 0,
                     diagnostic};
}

[[nodiscard]] core::Error unavailable_capability(const char* diagnostic) noexcept {
  return core::Error{core::ErrorDomain::core,
                     core::ErrorCode::unavailable_capability, 0, diagnostic};
}

[[nodiscard]] bool would_block(int native_error) noexcept {
  return native_error == EAGAIN
#if EWOULDBLOCK != EAGAIN
         || native_error == EWOULDBLOCK
#endif
      ;
}

[[nodiscard]] bool unavailable(int native_error) noexcept {
  return native_error == ENOSYS || native_error == ENOTSUP ||
#if EOPNOTSUPP != ENOTSUP
         native_error == EOPNOTSUPP ||
#endif
         native_error == EXDEV ||
         native_error == EINVAL;
}

[[nodiscard]] constexpr FileTransferPath path_for_mode(
    FileTransferMode mode) noexcept {
  switch (mode) {
    case FileTransferMode::generic:
      return FileTransferPath::generic;
    case FileTransferMode::direct:
      return FileTransferPath::direct;
    case FileTransferMode::automatic:
    case FileTransferMode::kernel:
      return FileTransferPath::kernel;
  }
  return FileTransferPath::generic;
}

[[nodiscard]] core::Result<void> validate(
    core::BorrowedFileHandle source, core::BorrowedSocketHandle destination,
    FileTransferRequest request,
    const internal::FileTransferOperations& operations) noexcept {
  if (!source.is_valid() || !destination.is_valid() || request.maximum_bytes == 0 ||
      request.maximum_syscalls == 0 || operations.pread == nullptr ||
      operations.send == nullptr || operations.kernel_transfer == nullptr ||
      operations.prepare_destination == nullptr) {
    return std::unexpected{invalid("file transfer arguments are invalid")};
  }
  const std::uint64_t native_offset_max =
      static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
  if (request.offset > native_offset_max ||
      request.maximum_bytes > native_offset_max - request.offset) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::overflow, 0,
                                       "file transfer range overflows"}};
  }
  switch (request.mode) {
    case FileTransferMode::automatic:
    case FileTransferMode::generic:
    case FileTransferMode::kernel:
    case FileTransferMode::direct:
      return {};
  }
  return std::unexpected{invalid("file transfer mode is invalid")};
}

[[nodiscard]] core::Result<std::uint64_t> next_offset(std::uint64_t offset,
                                                       std::size_t bytes) noexcept {
  if (bytes > std::numeric_limits<std::uint64_t>::max() - offset) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::overflow, 0,
                                       "file transfer offset overflows"}};
  }
  const std::uint64_t byte_count = bytes;
  return offset + byte_count;
}

[[nodiscard]] core::Result<FileTransferResult> generic_transfer(
    core::BorrowedFileHandle source, core::BorrowedSocketHandle destination,
    FileTransferRequest request, core::CancellationToken cancellation,
    const internal::FileTransferOperations& operations,
    std::uint32_t prior_calls = 0,
    FileTransferPath path = FileTransferPath::generic) noexcept {
  std::array<std::byte, generic_buffer_size> buffer{};
  std::uint32_t calls = prior_calls;
  while (calls < request.maximum_syscalls) {
    if (const auto active = cancellation.require_active(); !active) {
      return std::unexpected{active.error()};
    }
    const std::size_t requested = std::min(buffer.size(), request.maximum_bytes);
    ++calls;
    const ssize_t read = operations.pread(operations.context, source.native_handle(),
                                          buffer.data(), requested, request.offset);
    if (read == 0) {
      return FileTransferResult{request.offset, 0, calls,
                                FileTransferState::end_of_file,
                                path};
    }
    if (read < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected{core::Error::from_errno(errno,
                                                      "file transfer read failed")};
    }
    if (calls == request.maximum_syscalls) {
      return FileTransferResult{request.offset, 0, calls,
                                FileTransferState::budget_exhausted,
                                path};
    }
    if (const auto active = cancellation.require_active(); !active) {
      return std::unexpected{active.error()};
    }
    ++calls;
    const ssize_t sent = operations.send(
        operations.context, destination.native_handle(), buffer.data(),
        static_cast<std::size_t>(read), transfer_send_flags());
    if (sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (would_block(errno)) {
        return FileTransferResult{request.offset, 0, calls,
                                  FileTransferState::would_block,
                                  path};
      }
      return std::unexpected{core::Error::from_errno(errno,
                                                      "file transfer write failed")};
    }
    const auto offset = next_offset(request.offset, static_cast<std::size_t>(sent));
    if (!offset) {
      return std::unexpected{offset.error()};
    }
    return FileTransferResult{*offset, static_cast<std::size_t>(sent), calls,
                              FileTransferState::progress,
                              path};
  }
  return FileTransferResult{request.offset, 0, calls,
                            FileTransferState::budget_exhausted,
                            path};
}

[[nodiscard]] core::Result<FileTransferResult> direct_transfer(
    core::BorrowedFileHandle source, core::BorrowedSocketHandle destination,
    FileTransferRequest request, core::CancellationToken cancellation,
    const internal::FileTransferOperations& operations,
    std::uint32_t prior_calls) noexcept {
#if defined(O_DIRECT)
  std::uint32_t calls = prior_calls;
  if (calls == request.maximum_syscalls) {
    return FileTransferResult{request.offset, 0, calls,
                              FileTransferState::budget_exhausted,
                              FileTransferPath::direct};
  }
  const int flags = ::fcntl(source.native_handle(), F_GETFL, 0);
  ++calls;
  if (flags < 0) {
    return std::unexpected{core::Error::from_errno(
        errno, "direct file transfer descriptor inspection failed")};
  }
  if ((flags & O_DIRECT) == 0) {
    return std::unexpected{unavailable_capability(
        "direct file transfer requires an O_DIRECT source")};
  }
  std::size_t memory_alignment{};
  std::size_t offset_alignment{};
#if defined(__linux__) && defined(SYS_statx) && defined(STATX_DIOALIGN)
  if (calls == request.maximum_syscalls) {
    return FileTransferResult{request.offset, 0, calls,
                              FileTransferState::budget_exhausted,
                              FileTransferPath::direct};
  }
  struct statx status {};
  const int status_result = static_cast<int>(::syscall(
      SYS_statx, source.native_handle(), "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC,
      STATX_DIOALIGN, &status));
  ++calls;
  if (status_result == 0 && (status.stx_mask & STATX_DIOALIGN) != 0U) {
    memory_alignment = status.stx_dio_mem_align;
    offset_alignment = status.stx_dio_offset_align;
  }
#endif
#if defined(_PC_REC_XFER_ALIGN)
  if (memory_alignment == 0 || offset_alignment == 0) {
    if (calls == request.maximum_syscalls) {
      return FileTransferResult{request.offset, 0, calls,
                                FileTransferState::budget_exhausted,
                                FileTransferPath::direct};
    }
    errno = 0;
    const long alignment = ::fpathconf(source.native_handle(), _PC_REC_XFER_ALIGN);
    ++calls;
    if (alignment > 0) {
      memory_alignment = static_cast<std::size_t>(alignment);
      offset_alignment = memory_alignment;
    }
  }
#endif
  if (memory_alignment == 0 || offset_alignment == 0 ||
      memory_alignment > direct_buffer_alignment ||
      offset_alignment > direct_buffer_size ||
      direct_buffer_alignment % memory_alignment != 0 ||
      direct_buffer_size % offset_alignment != 0) {
    return std::unexpected{unavailable_capability(
        "direct file transfer alignment is unavailable")};
  }
  if (request.offset % offset_alignment != 0 ||
      request.maximum_bytes % offset_alignment != 0) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::invalid_range, 0,
                                       "direct file transfer range is unaligned"}};
  }
  alignas(direct_buffer_alignment)
      std::array<std::byte, direct_buffer_size> buffer{};
  const std::size_t requested = std::min(buffer.size(), request.maximum_bytes);
  while (calls < request.maximum_syscalls) {
    if (const auto active = cancellation.require_active(); !active) {
      return std::unexpected{active.error()};
    }
    ++calls;
    const ssize_t read = operations.pread(operations.context,
                                          source.native_handle(), buffer.data(),
                                          requested, request.offset);
    if (read == 0) {
      return FileTransferResult{request.offset, 0, calls,
                                FileTransferState::end_of_file,
                                FileTransferPath::direct};
    }
    if (read < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected{core::Error::from_errno(
          errno, "direct file transfer read failed")};
    }
    if (const auto active = cancellation.require_active(); !active) {
      return std::unexpected{active.error()};
    }
    if (calls == request.maximum_syscalls) {
      return FileTransferResult{request.offset, 0, calls,
                                FileTransferState::budget_exhausted,
                                FileTransferPath::direct};
    }
    ssize_t sent{-1};
    while (calls < request.maximum_syscalls) {
      if (const auto active = cancellation.require_active(); !active) {
        return std::unexpected{active.error()};
      }
      ++calls;
      sent = operations.send(operations.context, destination.native_handle(),
                             buffer.data(), static_cast<std::size_t>(read),
                             transfer_send_flags());
      if (sent >= 0 || errno != EINTR) {
        break;
      }
    }
    if (sent < 0) {
      if (errno == EINTR) {
        return FileTransferResult{request.offset, 0, calls,
                                  FileTransferState::budget_exhausted,
                                  FileTransferPath::direct};
      }
      if (would_block(errno)) {
        return FileTransferResult{request.offset, 0, calls,
                                  FileTransferState::would_block,
                                  FileTransferPath::direct};
      }
      return std::unexpected{core::Error::from_errno(
          errno, "direct file transfer write failed")};
    }
    if (sent == 0) {
      return std::unexpected{core::Error::from_errno(
          EIO, "direct file transfer made no progress")};
    }
    const auto offset = next_offset(request.offset, static_cast<std::size_t>(sent));
    if (!offset) {
      return std::unexpected{offset.error()};
    }
    return FileTransferResult{*offset, static_cast<std::size_t>(sent), calls,
                              FileTransferState::progress,
                              FileTransferPath::direct};
  }
  return FileTransferResult{request.offset, 0, calls,
                            FileTransferState::budget_exhausted,
                            FileTransferPath::direct};
#else
  static_cast<void>(source);
  static_cast<void>(destination);
  static_cast<void>(request);
  static_cast<void>(cancellation);
  static_cast<void>(operations);
  static_cast<void>(prior_calls);
  return std::unexpected{unavailable_capability(
      "direct file transfer is unavailable")};
#endif
}

[[nodiscard]] core::Result<FileTransferResult> transfer_with_operations(
    core::BorrowedFileHandle source, core::BorrowedSocketHandle destination,
    FileTransferRequest request, core::CancellationToken cancellation,
    const internal::FileTransferOperations& operations) noexcept {
  if (const auto valid = validate(source, destination, request, operations); !valid) {
    return std::unexpected{valid.error()};
  }
  std::uint32_t calls{};
  const int preparation = operations.prepare_destination(
      operations.context, destination.native_handle(), request.maximum_syscalls,
      &calls);
  if (preparation == internal::operation_budget_exhausted) {
    return FileTransferResult{request.offset, 0, calls,
                              FileTransferState::budget_exhausted,
                              path_for_mode(request.mode)};
  }
  if (preparation != internal::operation_complete) {
    return std::unexpected{core::Error::from_errno(
        errno, "file transfer destination preparation failed")};
  }
  if (request.mode == FileTransferMode::generic) {
    return generic_transfer(source, destination, request, cancellation, operations,
                            calls);
  }
  if (request.mode == FileTransferMode::direct) {
    return direct_transfer(source, destination, request, cancellation, operations,
                           calls);
  }
  while (calls < request.maximum_syscalls) {
    if (const auto active = cancellation.require_active(); !active) {
      return std::unexpected{active.error()};
    }
    std::size_t transferred{};
    std::uint32_t kernel_calls{};
    const int result = operations.kernel_transfer(
        operations.context, destination.native_handle(), source.native_handle(),
        request.offset, request.maximum_bytes, request.maximum_syscalls - calls,
        &transferred, &kernel_calls);
    if (kernel_calls > request.maximum_syscalls - calls) {
      return std::unexpected{core::Error{core::ErrorDomain::core,
                                         core::ErrorCode::invalid_state, 0,
                                         "kernel transfer exceeded syscall budget"}};
    }
    calls += kernel_calls;
    const int native_error = errno;
    if (result == internal::operation_budget_exhausted) {
      return FileTransferResult{request.offset, 0, calls,
                                FileTransferState::budget_exhausted,
                                FileTransferPath::kernel};
    }
    if (transferred != 0) {
      const auto offset = next_offset(request.offset, transferred);
      if (!offset) {
        return std::unexpected{offset.error()};
      }
      return FileTransferResult{*offset, transferred, calls,
                                FileTransferState::progress,
                                FileTransferPath::kernel};
    }
    if (result == internal::operation_complete) {
      return FileTransferResult{request.offset, 0, calls,
                                FileTransferState::end_of_file,
                                FileTransferPath::kernel};
    }
    if (native_error == EINTR) {
      continue;
    }
    if (would_block(native_error)) {
      return FileTransferResult{request.offset, 0, calls,
                                FileTransferState::would_block,
                                FileTransferPath::kernel};
    }
    if (request.mode == FileTransferMode::automatic && unavailable(native_error) &&
        calls < request.maximum_syscalls) {
      return generic_transfer(source, destination, request, cancellation,
                              operations, calls);
    }
    return std::unexpected{core::Error::from_errno(
        native_error, "kernel file transfer failed")};
  }
  return FileTransferResult{request.offset, 0, calls,
                            FileTransferState::budget_exhausted,
                            FileTransferPath::kernel};
}

}  // namespace

FileTransferCapabilities file_transfer_capabilities() noexcept {
  return {
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
      true,
#else
      false,
#endif
#if defined(O_DIRECT)
      true,
#else
      false,
#endif
#if defined(__linux__) && defined(SOL_TLS)
      true,
#else
      false,
#endif
  };
}

core::Result<FileTransferResult> transfer_file(
    core::BorrowedFileHandle source, core::BorrowedSocketHandle destination,
    FileTransferRequest request, core::CancellationToken cancellation) noexcept {
  return transfer_with_operations(source, destination, request, cancellation,
                                  internal::default_file_transfer_operations());
}

core::Result<FileTransferResult> internal::FileTransferTestAccess::transfer(
    core::BorrowedFileHandle source, core::BorrowedSocketHandle destination,
    FileTransferRequest request, core::CancellationToken cancellation,
    const FileTransferOperations& operations) noexcept {
  return transfer_with_operations(source, destination, request, cancellation,
                                  operations);
}

}  // namespace laghu::os
