// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/lcp.h"

#include <assert.h>

int main(void) {
  static const unsigned char html[] =
      "<main><img src=/hero.jpg width=800 height=600></main>";
  laghu_rum_instrumentation_record record = {0};
  char digest[LAGHU_RUNTIME_KEY_SIZE];
  assert(laghu_lcp_inventory_record((laghu_buffer){html, sizeof(html) - 1U},
                                    "/", "https://example.test", &record,
                                    digest));
  assert(record.media_count == 1U && digest[0] != '\0');
  return 0;
}
