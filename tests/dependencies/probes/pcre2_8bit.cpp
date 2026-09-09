// SPDX-License-Identifier: AGPL-3.0-only
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

int main() {
  int value = 0;
  return pcre2_config(PCRE2_CONFIG_VERSION, &value);
}
