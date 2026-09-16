// SPDX-License-Identifier: AGPL-3.0-only
#include <crypt.h>

int main() {
  crypt_data output{};
  return crypt_r("password", "$6$rounds=1000$salt$", &output) == nullptr ? 1 : 0;
}
