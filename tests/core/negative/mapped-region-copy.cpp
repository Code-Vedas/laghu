// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/mapped_regions.hpp>

int main() {
  laghu::core::MappedRegion first;
  [[maybe_unused]] laghu::core::MappedRegion second{first};
  return 0;
}
