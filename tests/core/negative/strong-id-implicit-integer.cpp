// SPDX-License-Identifier: AGPL-3.0-only
#include <cstdint>

#include <laghu/core/identifiers.hpp>

int main() {
  const std::uint64_t raw = 1;
  laghu::core::WorkerId worker = raw;
  return static_cast<int>(worker.value());
}
