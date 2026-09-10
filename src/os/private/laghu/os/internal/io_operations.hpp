// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>

#include <sys/types.h>

#include <laghu/core/bounded_buffer.hpp>

namespace laghu::os::internal {

using IoReadFunction = ssize_t (*)(void* context, int descriptor, void* output,
                                   std::size_t capacity) noexcept;
using IoWriteFunction = ssize_t (*)(void* context, int descriptor, const void* input,
                                    std::size_t size) noexcept;

struct IoOperations final {
  void* context;
  IoReadFunction read;
  IoWriteFunction write;
};

[[nodiscard]] const IoOperations& default_io_operations() noexcept;
[[nodiscard]] core::Result<std::size_t> read_once(int descriptor, core::MutableByteView output,
                                                   const IoOperations& operations) noexcept;
[[nodiscard]] core::Result<std::size_t> write_once(int descriptor, core::ByteView input,
                                                    const IoOperations& operations) noexcept;

}  // namespace laghu::os::internal
