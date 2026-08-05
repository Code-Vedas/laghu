// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/operational.h"

#include <assert.h>
#include <string.h>

#include "test_fixture.h"

int main(void) {
  char root[LAGHU_RUNTIME_PATH_SIZE];
  char output[LAGHU_OPERATIONAL_RENDER_SIZE];
  size_t length = 0U;
  laghu_operational_registry registry;
  laghu_operational_snapshot snapshot;
  assert(laghu_test_directory(root, sizeof(root)));
  laghu_operational_registry_init(&registry);
  assert(laghu_operational_registry_open(
      &registry, root, LAGHU_OPERATIONAL_SURFACE_STANDALONE,
      LAGHU_OPERATIONAL_PROCESS_ADAPTER, false, 100U));
  laghu_operational_registry_record(
      &registry, LAGHU_OPERATIONAL_DECISION_ORIGINAL, 100U, 80U, 5000U);
  assert(laghu_operational_registry_snapshot(&registry, &snapshot));
  assert(laghu_operational_render_prometheus(&snapshot, 100U, output,
                                             sizeof(output), &length));
  assert(length != 0U && strstr(output, "laghu_requests_total") != NULL);
  laghu_operational_registry_close(&registry);
  return 0;
}
