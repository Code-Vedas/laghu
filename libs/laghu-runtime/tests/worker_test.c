// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/worker.h"

#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int main(void) {
  char directory[LAGHU_RUNTIME_PATH_SIZE];
  laghu_worker_lifecycle lifecycle;
  laghu_operational_snapshot snapshot;
  uint64_t now = (uint64_t)time(NULL);
  unsigned int slot;
  bool found = false;
  assert(snprintf(directory, sizeof(directory), "/tmp/laghu-worker-%ld-%llu", (long)getpid(), (unsigned long long)now) > 0);
  assert(mkdir(directory, 0700) == 0);
  laghu_worker_lifecycle_init(&lifecycle);
  assert(laghu_worker_lifecycle_start(&lifecycle, directory, LAGHU_OPERATIONAL_PROCESS_LIBVIPS, NULL, true, now));
  laghu_worker_lifecycle_heartbeat(&lifecycle, now, true);
  laghu_worker_lifecycle_job(&lifecycle, false, 2500U, LAGHU_OPERATIONAL_FAILURE_WORKER);
  assert(laghu_operational_registry_snapshot(&lifecycle.registry, &snapshot));
  for (slot = 0U; slot < snapshot.slot_count; ++slot) {
    if (snapshot.slots[slot].active != 0U && snapshot.slots[slot].process_kind == LAGHU_OPERATIONAL_PROCESS_LIBVIPS) {
      assert(snapshot.slots[slot].healthy == 1U);
      assert(snapshot.slots[slot].latency_count == 1U);
      assert(snapshot.slots[slot].failures[LAGHU_OPERATIONAL_FAILURE_WORKER] == 1U);
      found = true;
    }
  }
  assert(found);
  laghu_worker_lifecycle_stop(&lifecycle, now);
  return 0;
}
