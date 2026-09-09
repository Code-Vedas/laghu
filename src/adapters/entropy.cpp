// SPDX-License-Identifier: AGPL-3.0-only
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <sys/random.h>
#include <unistd.h>

#include <laghu/adapters/internal/entropy.hpp>

namespace laghu::adapters::internal {
namespace {

[[nodiscard]] int os_getentropy(core::MutableByteView output, void*) noexcept {
  return ::getentropy(output.data(), output.size());
}

}  // namespace

core::Result<void> fill_entropy_with(core::MutableByteView output,
                                     EntropyCall call,
                                     void* context) noexcept {
  if (call == nullptr) {
    return std::unexpected{core::Error{core::ErrorDomain::core, core::ErrorCode::invalid_input,
                                       0, "entropy call is required"}};
  }
  if (output.size() > entropy_request_limit) {
    return std::unexpected{core::Error{core::ErrorDomain::core, core::ErrorCode::invalid_range,
                                       0, "entropy request exceeds bounded limit"}};
  }

  std::size_t offset = 0;
  while (offset < output.size()) {
    const std::size_t request =
        std::min(entropy_call_limit, output.size() - offset);
    const auto chunk = output.slice(offset, request);
    if (!chunk.has_value()) {
      return std::unexpected{chunk.error()};
    }
    if (call(*chunk, context) != 0) {
      const int native_error = errno;
      return std::unexpected{core::Error::from_errno(native_error, "getentropy failed")};
    }
    offset += request;
  }
  return {};
}

core::Result<void> fill_entropy(core::MutableByteView output) noexcept {
  return fill_entropy_with(output, os_getentropy, nullptr);
}

}  // namespace laghu::adapters::internal
