// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>

#include <sys/types.h>

#include <laghu/os/file_transfer.hpp>

namespace laghu::os::internal {

using FilePreadFunction = ssize_t (*)(void*, int, void*, std::size_t,
                                      std::uint64_t) noexcept;
using FileSendFunction = ssize_t (*)(void*, int, const void*, std::size_t,
                                     int) noexcept;
using KernelTransferFunction = int (*)(void*, int, int, std::uint64_t,
                                       std::size_t, std::uint32_t,
                                       std::size_t*, std::uint32_t*) noexcept;
using FileTransferPrepareFunction = int (*)(void*, int, std::uint32_t,
                                            std::uint32_t*) noexcept;

struct FileTransferOperations final {
  void* context;
  FilePreadFunction pread;
  FileSendFunction send;
  KernelTransferFunction kernel_transfer;
  FileTransferPrepareFunction prepare_destination;
};

[[nodiscard]] const FileTransferOperations& default_file_transfer_operations() noexcept;

class FileTransferTestAccess final {
 public:
  [[nodiscard]] static core::Result<FileTransferResult> transfer(
      core::BorrowedFileHandle source, core::BorrowedSocketHandle destination,
      FileTransferRequest request, core::CancellationToken cancellation,
      const FileTransferOperations& operations) noexcept;
};

}  // namespace laghu::os::internal
