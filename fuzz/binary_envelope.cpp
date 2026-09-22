// SPDX-License-Identifier: AGPL-3.0-only
#include <cstddef>
#include <cstdint>
#include <span>

#include <laghu/core/binary_envelope.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto input = laghu::core::ByteView::from(
      std::span<const std::byte>{reinterpret_cast<const std::byte*>(data), size});
  if (!input.has_value()) {
    return 0;
  }
  laghu::core::BinaryEnvelope decoded;
  static_cast<void>(laghu::core::decode_binary_envelope(*input, UINT32_MAX, decoded));
  return 0;
}
