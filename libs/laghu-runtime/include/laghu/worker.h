// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_WORKER_H
#define LAGHU_WORKER_H

#include <stdbool.h>
#include <stdint.h>

#include "laghu/operational.h"
#include "laghu/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  laghu_operational_registry registry;
  laghu_runtime_queue *queue;
  bool started;
} laghu_worker_lifecycle;

void laghu_worker_lifecycle_init(laghu_worker_lifecycle *lifecycle);
bool laghu_worker_lifecycle_start(laghu_worker_lifecycle *lifecycle,
                                  const char *cache_path,
                                  laghu_operational_process_kind kind,
                                  laghu_runtime_queue *queue, bool required,
                                  uint64_t now);
void laghu_worker_lifecycle_heartbeat(laghu_worker_lifecycle *lifecycle,
                                      uint64_t now, bool healthy);
uint64_t laghu_worker_lifecycle_clock(void);
void laghu_worker_lifecycle_job(laghu_worker_lifecycle *lifecycle, bool success,
                                uint64_t elapsed_microseconds,
                                laghu_operational_failure failure);
void laghu_worker_lifecycle_stop(laghu_worker_lifecycle *lifecycle,
                                 uint64_t now);

#ifdef __cplusplus
}
#endif

#endif
