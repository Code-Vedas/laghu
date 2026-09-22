// SPDX-License-Identifier: AGPL-3.0-only
#include <protobuf-c/protobuf-c.h>

static_assert(PROTOBUF_C_VERSION_NUMBER >= 1004001);

int main() {
  return protobuf_c_version_number() < 1004001U;
}
