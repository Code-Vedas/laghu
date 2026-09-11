// SPDX-License-Identifier: AGPL-3.0-only
extern "C" {
volatile int laghu_sanitizer_fixture_heap_misuse_marker = 0;
}

int main() {
  if (laghu_sanitizer_fixture_heap_misuse_marker != 0) {
    return 1;
  }
  int* const values = new int[1];
  values[1] = 1;
  delete[] values;
  return 0;
}
