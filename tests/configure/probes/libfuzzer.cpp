// SPDX-License-Identifier: AGPL-3.0-only
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static_cast<void>(data);
  static_cast<void>(size);
  return 0;
}
