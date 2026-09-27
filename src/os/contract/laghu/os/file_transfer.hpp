// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>

#include <laghu/core/deadlines_cancellation.hpp>
#include <laghu/core/handles.hpp>

namespace laghu::os {

namespace internal {
struct FileTransferOperations;
class FileTransferTestAccess;
}  // namespace internal

// The destination socket must already be nonblocking. Direct mode additionally
// requires an O_DIRECT source and filesystem-aligned offsets and lengths.
enum class FileTransferMode : std::uint8_t {
  automatic,
  generic,
  kernel,
  direct,
};
enum class FileTransferPath : std::uint8_t { generic, kernel, direct };
enum class FileTransferState : std::uint8_t {
  progress,
  would_block,
  end_of_file,
  budget_exhausted,
};

struct FileTransferCapabilities final {
  bool kernel_transfer;
  bool direct_io;
  bool kernel_tls_hook;
};

struct FileTransferRequest final {
  std::uint64_t offset;
  std::size_t maximum_bytes;
  std::uint32_t maximum_syscalls;
  FileTransferMode mode;
};

struct FileTransferResult final {
  std::uint64_t next_offset;
  std::size_t bytes;
  std::uint32_t syscalls;
  FileTransferState state;
  FileTransferPath path;
};

[[nodiscard]] FileTransferCapabilities file_transfer_capabilities() noexcept;

[[nodiscard]] core::Result<FileTransferResult> transfer_file(
    core::BorrowedFileHandle source, core::BorrowedSocketHandle destination,
    FileTransferRequest request, core::CancellationToken cancellation) noexcept;

}  // namespace laghu::os
