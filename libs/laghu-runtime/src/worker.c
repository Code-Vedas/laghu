// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/worker.h"

#include <string.h>
#include <time.h>

void laghu_worker_lifecycle_init(laghu_worker_lifecycle *lifecycle) {
  if (lifecycle == NULL) return;
  memset(lifecycle, 0, sizeof(*lifecycle));
  laghu_operational_registry_init(&lifecycle->registry);
}

bool laghu_worker_lifecycle_start(laghu_worker_lifecycle *lifecycle, const char *cache_path, laghu_operational_process_kind kind,
                                  laghu_runtime_queue *queue, bool required, uint64_t now) {
  if (lifecycle == NULL || cache_path == NULL || cache_path[0] == '\0') return false;
  lifecycle->queue = queue;
  lifecycle->started = laghu_operational_registry_open(&lifecycle->registry, cache_path, LAGHU_OPERATIONAL_SURFACE_WORKER, kind, required, now);
  if (lifecycle->started) laghu_worker_lifecycle_heartbeat(lifecycle, now, true);
  return lifecycle->started;
}

void laghu_worker_lifecycle_heartbeat(laghu_worker_lifecycle *lifecycle, uint64_t now, bool healthy) {
  uint64_t capacity = 0U;
  uint64_t occupied = 0U;
  if (lifecycle == NULL || !lifecycle->started) return;
  if (lifecycle->queue != NULL) {
    (void)laghu_runtime_queue_heartbeat(lifecycle->queue, now);
    (void)laghu_runtime_queue_status(lifecycle->queue, &capacity, &occupied);
  }
  (void)laghu_operational_registry_heartbeat(&lifecycle->registry, now, healthy, capacity, occupied);
}

uint64_t laghu_worker_lifecycle_clock(void) {
  struct timespec value;
  if (timespec_get(&value, TIME_UTC) != TIME_UTC) return 0U;
  return (uint64_t)value.tv_sec * UINT64_C(1000000) + (uint64_t)value.tv_nsec / UINT64_C(1000);
}

void laghu_worker_lifecycle_job(laghu_worker_lifecycle *lifecycle, bool success, uint64_t elapsed_microseconds, laghu_operational_failure failure) {
  if (lifecycle == NULL || !lifecycle->started) return;
  laghu_operational_registry_worker_job(&lifecycle->registry, success, elapsed_microseconds, failure);
}

void laghu_worker_lifecycle_stop(laghu_worker_lifecycle *lifecycle, uint64_t now) {
  if (lifecycle == NULL) return;
  if (lifecycle->started) (void)laghu_operational_registry_heartbeat(&lifecycle->registry, now, false, 0U, 0U);
  laghu_operational_registry_close(&lifecycle->registry);
  lifecycle->queue = NULL;
  lifecycle->started = false;
}
