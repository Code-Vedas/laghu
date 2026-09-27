// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/os/file_transfer.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/tls.h>
#include <pthread.h>
#include <sys/sendfile.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/uio.h>
#endif

#include <laghu/os/internal/file_transfer.hpp>
#include <laghu/core/checked_arithmetic.hpp>

namespace laghu::os::internal {
namespace {

[[nodiscard]] ssize_t system_pread(void*, int descriptor, void* output,
                                   std::size_t size, std::uint64_t offset) noexcept {
  return ::pread(descriptor, output, size, static_cast<off_t>(offset));
}

[[nodiscard]] ssize_t system_send(void*, int descriptor, const void* input,
                                  std::size_t size, int flags) noexcept {
  return ::send(descriptor, input, size, flags);
}

[[nodiscard]] int system_prepare_destination(void*, int descriptor) noexcept {
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0 || (flags & O_NONBLOCK) == 0) {
    if (flags >= 0) {
      errno = EINVAL;
    }
    return -1;
  }
#if defined(SO_NOSIGPIPE)
  const int enabled = 1;
  return ::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                      static_cast<socklen_t>(sizeof(enabled)));
#else
  static_cast<void>(descriptor);
  return 0;
#endif
}

#if defined(__linux__)
[[nodiscard]] ssize_t linux_sendfile_without_sigpipe(int output, int input,
                                                     off_t* offset,
                                                     std::size_t size) noexcept {
  sigset_t blocked{};
  sigset_t previous{};
  sigset_t pending{};
  if (::sigemptyset(&blocked) != 0 || ::sigaddset(&blocked, SIGPIPE) != 0) {
    return -1;
  }
  const int block_error = ::pthread_sigmask(SIG_BLOCK, &blocked, &previous);
  if (block_error != 0) {
    errno = block_error;
    return -1;
  }
  if (::sigpending(&pending) != 0) {
    const int pending_error = errno;
    static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous, nullptr));
    errno = pending_error;
    return -1;
  }
  const int pending_member = ::sigismember(&pending, SIGPIPE);
  if (pending_member < 0) {
    const int pending_error = errno;
    static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous, nullptr));
    errno = pending_error;
    return -1;
  }
  const bool already_pending = pending_member == 1;
  const ssize_t result = ::sendfile(output, input, offset, size);
  const int sendfile_error = errno;
  if (result < 0 && sendfile_error == EPIPE && !already_pending) {
    timespec timeout{};
    while (::sigtimedwait(&blocked, nullptr, &timeout) < 0 && errno == EINTR) {
    }
  }
  const int restore_error = ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
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
                                         std::size_t* transferred) noexcept {
#if defined(__linux__)
  off_t native_offset = static_cast<off_t>(offset);
  const ssize_t result =
      linux_sendfile_without_sigpipe(output, input, &native_offset, size);
  if (result >= 0) {
    *transferred = static_cast<std::size_t>(result);
    return 0;
  }
  *transferred = 0;
  return -1;
#elif defined(__APPLE__)
  off_t sent = static_cast<off_t>(size);
  const int result = ::sendfile(input, output, static_cast<off_t>(offset), &sent,
                                nullptr, 0);
  *transferred = sent > 0 ? static_cast<std::size_t>(sent) : 0U;
  return result;
#elif defined(__FreeBSD__)
  off_t sent{};
  const int result = ::sendfile(input, output, static_cast<off_t>(offset), size,
                                nullptr, &sent, 0);
  *transferred = sent > 0 ? static_cast<std::size_t>(sent) : 0U;
  return result;
#else
  static_cast<void>(output);
  static_cast<void>(input);
  static_cast<void>(offset);
  static_cast<void>(size);
  *transferred = 0;
  errno = ENOSYS;
  return -1;
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
constexpr std::size_t direct_io_alignment = 4096U;

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
    case FileTransferMode::mapped:
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
  alignas(direct_io_alignment) std::array<std::byte, generic_buffer_size> buffer{};
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

[[nodiscard]] core::Result<FileTransferResult> mapped_transfer(
    core::BorrowedFileHandle source, core::BorrowedSocketHandle destination,
    FileTransferRequest request, core::CancellationToken cancellation,
    const internal::FileTransferOperations& operations) noexcept {
  if (const auto active = cancellation.require_active(); !active) {
    return std::unexpected{active.error()};
  }
  if (request.maximum_syscalls < 4U) {
    return FileTransferResult{request.offset, 0, 0,
                              FileTransferState::budget_exhausted,
                              FileTransferPath::mapped};
  }
  struct stat status {};
  if (::fstat(source.native_handle(), &status) != 0) {
    return std::unexpected{core::Error::from_errno(
        errno, "mapped file transfer status failed")};
  }
  if (status.st_size < 0) {
    return std::unexpected{invalid("mapped file transfer size is invalid")};
  }
  if (!S_ISREG(status.st_mode) || status.st_size == 0) {
    return generic_transfer(source, destination, request, cancellation,
                            operations, 1U);
  }
  const std::uint64_t file_size = static_cast<std::uint64_t>(status.st_size);
  if (request.offset >= file_size) {
    return FileTransferResult{request.offset, 0, 1U,
                              FileTransferState::end_of_file,
                              FileTransferPath::mapped};
  }
  const std::uint64_t available = file_size - request.offset;
  std::size_t requested = request.maximum_bytes;
  if (available < request.maximum_bytes) {
    const auto narrowed = core::checked_narrow<std::size_t>(available);
    if (!narrowed) {
      return std::unexpected{narrowed.error()};
    }
    requested = *narrowed;
  }
  const long native_page_size = ::sysconf(_SC_PAGESIZE);
  if (native_page_size <= 0) {
    return generic_transfer(source, destination, request, cancellation,
                            operations, 1U);
  }
  const std::uint64_t page_size = static_cast<std::uint64_t>(native_page_size);
  const std::uint64_t aligned_offset = request.offset - (request.offset % page_size);
  const auto checked_prefix =
      core::checked_narrow<std::size_t>(request.offset - aligned_offset);
  if (!checked_prefix) {
    return std::unexpected{checked_prefix.error()};
  }
  const std::size_t prefix = *checked_prefix;
  if (requested > std::numeric_limits<std::size_t>::max() - prefix) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::overflow, 0,
                                       "mapped file transfer range overflows"}};
  }
  const std::size_t mapping_size = prefix + requested;
  void* const mapping = ::mmap(nullptr, mapping_size, PROT_READ, MAP_PRIVATE,
                               source.native_handle(),
                               static_cast<off_t>(aligned_offset));
  if (mapping == MAP_FAILED) {
    return generic_transfer(source, destination, request, cancellation,
                            operations, 2U);
  }
  const auto mapped_bytes = std::span<const std::byte>{
      static_cast<const std::byte*>(mapping), mapping_size};
  const auto* const bytes = mapped_bytes.subspan(prefix).data();
  std::uint32_t calls = 2U;
  ssize_t sent{-1};
  int native_error{};
  while (calls + 1U < request.maximum_syscalls) {
    if (const auto active = cancellation.require_active(); !active) {
      static_cast<void>(::munmap(mapping, mapping_size));
      return std::unexpected{active.error()};
    }
    ++calls;
    sent = operations.send(operations.context, destination.native_handle(),
                           bytes, requested, transfer_send_flags());
    native_error = errno;
    if (sent >= 0 || native_error != EINTR) {
      break;
    }
  }
  static_cast<void>(::munmap(mapping, mapping_size));
  ++calls;
  if (sent < 0) {
    if (native_error == EINTR) {
      return FileTransferResult{request.offset, 0, calls,
                                FileTransferState::budget_exhausted,
                                FileTransferPath::mapped};
    }
    if (would_block(native_error)) {
      return FileTransferResult{request.offset, 0, calls,
                                FileTransferState::would_block,
                                FileTransferPath::mapped};
    }
    return std::unexpected{core::Error::from_errno(
        native_error, "mapped file transfer write failed")};
  }
  const std::size_t transferred = static_cast<std::size_t>(sent);
  const auto offset = next_offset(request.offset, transferred);
  if (!offset) {
    return std::unexpected{offset.error()};
  }
  return FileTransferResult{*offset, transferred, calls,
                            FileTransferState::progress,
                            FileTransferPath::mapped};
}

[[nodiscard]] core::Result<FileTransferResult> direct_transfer(
    core::BorrowedFileHandle source, core::BorrowedSocketHandle destination,
    FileTransferRequest request, core::CancellationToken cancellation,
    const internal::FileTransferOperations& operations) noexcept {
#if defined(O_DIRECT)
  const int flags = ::fcntl(source.native_handle(), F_GETFL, 0);
  if (flags < 0) {
    return std::unexpected{core::Error::from_errno(
        errno, "direct file transfer descriptor inspection failed")};
  }
  if ((flags & O_DIRECT) == 0) {
    return std::unexpected{unavailable_capability(
        "direct file transfer requires an O_DIRECT source")};
  }
  if (request.offset % direct_io_alignment != 0 ||
      request.maximum_bytes % direct_io_alignment != 0) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::invalid_range, 0,
                                       "direct file transfer range is unaligned"}};
  }
  return generic_transfer(source, destination, request, cancellation,
                          operations, 0U, FileTransferPath::direct);
#else
  static_cast<void>(source);
  static_cast<void>(destination);
  static_cast<void>(request);
  static_cast<void>(cancellation);
  static_cast<void>(operations);
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
  if (operations.prepare_destination(operations.context,
                                     destination.native_handle()) != 0) {
    return std::unexpected{core::Error::from_errno(
        errno, "file transfer destination preparation failed")};
  }
  if (request.mode == FileTransferMode::generic) {
    return generic_transfer(source, destination, request, cancellation, operations);
  }
  if (request.mode == FileTransferMode::mapped) {
    return mapped_transfer(source, destination, request, cancellation, operations);
  }
  if (request.mode == FileTransferMode::direct) {
    return direct_transfer(source, destination, request, cancellation, operations);
  }
  for (std::uint32_t index = 0; index < request.maximum_syscalls; ++index) {
    const std::uint32_t calls = index + 1U;
    if (const auto active = cancellation.require_active(); !active) {
      return std::unexpected{active.error()};
    }
    std::size_t transferred{};
    const int result = operations.kernel_transfer(
        operations.context, destination.native_handle(), source.native_handle(),
        request.offset, request.maximum_bytes, &transferred);
    const int native_error = errno;
    if (transferred != 0) {
      const auto offset = next_offset(request.offset, transferred);
      if (!offset) {
        return std::unexpected{offset.error()};
      }
      return FileTransferResult{*offset, transferred, calls,
                                FileTransferState::progress,
                                FileTransferPath::kernel};
    }
    if (result == 0) {
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
  return FileTransferResult{request.offset, 0, request.maximum_syscalls,
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
      true,
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
