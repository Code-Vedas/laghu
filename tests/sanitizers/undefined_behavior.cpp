// SPDX-License-Identifier: AGPL-3.0-only
#include <limits>

extern "C" {
volatile int laghu_sanitizer_fixture_undefined_behavior_marker = 0;
}

int main() {
  if (laghu_sanitizer_fixture_undefined_behavior_marker != 0) {
    return 1;
  }
  volatile int maximum = std::numeric_limits<int>::max();
  volatile int one = 1;
  return maximum + one;
}
