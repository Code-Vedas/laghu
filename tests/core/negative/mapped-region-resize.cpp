// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/mapped_regions.hpp>

int main() {
  laghu::core::MappedRegion region;
  region.resize(4096);
  return 0;
}
