// SPDX-License-Identifier: AGPL-3.0-only
#include <cerrno>

#include <unistd.h>

#include <laghu/os/internal/io_operations.hpp>

namespace laghu::os::internal {
namespace {

[[nodiscard]] ssize_t system_read(void*, int descriptor, void* output,
                                  std::size_t capacity) noexcept {
  return ::read(descriptor, output, capacity);
}

[[nodiscard]] ssize_t system_write(void*, int descriptor, const void* input,
                                   std::size_t size) noexcept {
  return ::write(descriptor, input, size);
}

constexpr IoOperations default_operations{nullptr, system_read, system_write};

[[nodiscard]] core::Result<std::size_t> transfer_result(ssize_t result,
                                                         const char* diagnostic) noexcept {
  if (result < 0) {
    return std::unexpected{core::Error::from_errno(errno, diagnostic)};
  }
  return static_cast<std::size_t>(result);
}

}  // namespace

const IoOperations& default_io_operations() noexcept { return default_operations; }

core::Result<std::size_t> read_once(int descriptor, core::MutableByteView output,
                                    const IoOperations& operations) noexcept {
  if (descriptor < 0 || operations.read == nullptr) {
    return std::unexpected{core::Error{core::ErrorDomain::core, core::ErrorCode::invalid_input, 0,
                                       "read operation is invalid"}};
  }
  return transfer_result(operations.read(operations.context, descriptor, output.data(), output.size()),
                         "read operation failed");
}

core::Result<std::size_t> write_once(int descriptor, core::ByteView input,
                                     const IoOperations& operations) noexcept {
  if (descriptor < 0 || operations.write == nullptr) {
    return std::unexpected{core::Error{core::ErrorDomain::core, core::ErrorCode::invalid_input, 0,
                                       "write operation is invalid"}};
  }
  return transfer_result(operations.write(operations.context, descriptor, input.data(), input.size()),
                         "write operation failed");
}

}  // namespace laghu::os::internal
