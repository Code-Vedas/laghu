// SPDX-License-Identifier: AGPL-3.0-only
#include <features.h>

#if !defined(__GLIBC__)
#error "Laghu requires a known active libc fortification marker"
#endif

#if !defined(__USE_FORTIFY_LEVEL) || __USE_FORTIFY_LEVEL < 1
#error "Laghu requires active glibc fortification"
#endif

#include <cstddef>
#include <cstring>

int main() {
  char destination[8]{};
  constexpr char source[] = "laghu";
  volatile std::size_t count = sizeof(source);
  std::memcpy(destination, source, count);
  return destination[0] == 'l' ? 0 : 1;
}
