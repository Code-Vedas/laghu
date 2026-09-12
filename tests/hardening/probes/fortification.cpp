// SPDX-License-Identifier: AGPL-3.0-only
#include <cstddef>
#include <cstring>

int main() {
  char destination[8]{};
  constexpr char source[] = "laghu";
  volatile std::size_t count = sizeof(source);
  std::memcpy(destination, source, count);
  return destination[0] == 'l' ? 0 : 1;
}
