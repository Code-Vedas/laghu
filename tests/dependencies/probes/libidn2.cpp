// SPDX-License-Identifier: AGPL-3.0-only
#include <cstdint>

#include <idn2.h>

int main() {
  std::uint8_t* output{};
  constexpr std::uint8_t hostname[]{'e', 'x', 'a', 'm', 'p', 'l', 'e', '\0'};
  const int result = idn2_lookup_u8(
      hostname, &output, IDN2_NFC_INPUT | IDN2_NONTRANSITIONAL | IDN2_USE_STD3_ASCII_RULES);
  if (output != nullptr) {
    idn2_free(output);
  }
  return result == IDN2_OK ? 0 : 1;
}
