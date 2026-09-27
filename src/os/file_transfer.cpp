// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/os/file_transfer.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/tls.h>
#include <sys/sendfile.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/uio.h>
#endif

#include <laghu/os/internal/file_transfer.hpp>

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
#if !defined(MSG_DONTWAIT)
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0 || (flags & O_NONBLOCK) == 0) {
    if (flags >= 0) {
      errno = EINVAL;
    }
    return -1;
  }
#endif
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
  const int enabled = 1;
  return ::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                      static_cast<socklen_t>(sizeof(enabled)));
#else
  static_cast<void>(descriptor);
  return 0;
#endif
}

[[nodiscard]] int system_kernel_transfer(void*, int output, int input,
                                         std::uint64_t offset, std::size_t size,
                                         std::size_t* transferred) noexcept {
#if defined(__linux__)
  off_t native_offset = static_cast<off_t>(offset);
  const ssize_t result = ::sendfile(output, input, &native_offset, size);
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
    std::uint32_t prior_calls = 0) noexcept {
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
                                FileTransferPath::generic};
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
                                FileTransferPath::generic};
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
                                  FileTransferPath::generic};
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
                              FileTransferPath::generic};
  }
  return FileTransferResult{request.offset, 0, calls,
                            FileTransferState::budget_exhausted,
                            FileTransferPath::generic};
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
