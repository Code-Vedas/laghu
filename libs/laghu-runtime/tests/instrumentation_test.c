// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/instrumentation.h"

#include <assert.h>
#include <string.h>

int main(void) {
  laghu_instrumentation_beacon beacon;
  const char *script = laghu_runtime_instrumentation_script();
  assert(script != NULL && script[0] != '\0');
  assert(!laghu_runtime_parse_instrumentation_beacon(
      (laghu_buffer){(const unsigned char *)"{}", 2U}, &beacon));
  return 0;
}
