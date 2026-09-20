// SPDX-License-Identifier: AGPL-3.0-only
#include <brotli/encode.h>
#include <brotli/decode.h>

int main() {
  return BrotliEncoderVersion() == 0U || BrotliDecoderVersion() == 0U;
}
