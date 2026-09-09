// SPDX-License-Identifier: AGPL-3.0-only
#include <memory_resource>

int main() {
  std::pmr::monotonic_buffer_resource resource;
  return resource.release(), 0;
}
