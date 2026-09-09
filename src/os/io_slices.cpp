// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/os/internal/io_slices.hpp>

namespace laghu::os::internal {

core::Result<std::size_t> translate_iovecs(const core::IoSliceList& slices,
                                           std::span<iovec> output) noexcept {
  if (slices.size() > output.size()) {
    return std::unexpected{core::Error{core::ErrorDomain::core, core::ErrorCode::exhaustion, 0,
                                       "POSIX I/O vector storage is exhausted"}};
  }
  std::size_t index = 0;
  for (const core::IoSlice& slice : slices.slices()) {
    output[index].iov_base = const_cast<std::byte*>(slice.bytes().data());
    output[index].iov_len = slice.size();
    ++index;
  }
  return index;
}

core::Result<void> consume_written(core::IoSliceList& slices, std::size_t bytes) noexcept {
  return slices.consume(bytes);
}

}  // namespace laghu::os::internal
