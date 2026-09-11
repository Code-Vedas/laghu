// SPDX-License-Identifier: AGPL-3.0-only
int main() {
  int* const values = new int[1];
  values[1] = 1;
  delete[] values;
  return 0;
}
