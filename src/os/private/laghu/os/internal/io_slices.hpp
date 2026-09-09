// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <span>

#include <sys/uio.h>

#include <laghu/core/bounded_buffer.hpp>

namespace laghu::os::internal {

[[nodiscard]] core::Result<std::size_t> translate_iovecs(const core::IoSliceList& slices,
                                                          std::span<iovec> output) noexcept;
[[nodiscard]] core::Result<void> consume_written(core::IoSliceList& slices,
                                                 std::size_t bytes) noexcept;

}  // namespace laghu::os::internal
