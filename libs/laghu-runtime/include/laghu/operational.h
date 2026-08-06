// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_OPERATIONAL_H
#define LAGHU_OPERATIONAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/budget.h"
#include "laghu/cache.h"
#include "laghu/core.h"
#include "laghu/image.h"
#include "laghu/lcp.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_OPERATIONAL_VERSION 4U
#define LAGHU_OPERATIONAL_MAX_SLOTS 64U
#define LAGHU_OPERATIONAL_LATENCY_BUCKETS 12U
#define LAGHU_OPERATIONAL_RENDER_SIZE 65536U
#define LAGHU_OPERATIONAL_STALE_SECONDS 45U
typedef enum {
  LAGHU_OPERATIONAL_SURFACE_NGINX = 0,
  LAGHU_OPERATIONAL_SURFACE_APACHE,
  LAGHU_OPERATIONAL_SURFACE_STANDALONE,
  LAGHU_OPERATIONAL_SURFACE_WORKER,
  LAGHU_OPERATIONAL_SURFACE_COUNT
} laghu_operational_surface;

typedef enum {
  LAGHU_OPERATIONAL_PROCESS_ADAPTER = 0,
  LAGHU_OPERATIONAL_PROCESS_LIBVIPS,
  LAGHU_OPERATIONAL_PROCESS_JAVASCRIPT,
  LAGHU_OPERATIONAL_PROCESS_RESOURCE_FETCH,
  LAGHU_OPERATIONAL_PROCESS_ASSET_UPLOAD,
  LAGHU_OPERATIONAL_PROCESS_COUNT
} laghu_operational_process_kind;

typedef enum {
  LAGHU_OPERATIONAL_DECISION_BYPASS = 0,
  LAGHU_OPERATIONAL_DECISION_ORIGINAL,
  LAGHU_OPERATIONAL_DECISION_OPTIMIZED,
  LAGHU_OPERATIONAL_DECISION_CACHED,
  LAGHU_OPERATIONAL_DECISION_QUEUED,
  LAGHU_OPERATIONAL_DECISION_COUNT
} laghu_operational_decision;

typedef enum {
  LAGHU_OPERATIONAL_FAILURE_RUNTIME = 0,
  LAGHU_OPERATIONAL_FAILURE_CACHE,
  LAGHU_OPERATIONAL_FAILURE_QUEUE,
  LAGHU_OPERATIONAL_FAILURE_WORKER,
  LAGHU_OPERATIONAL_FAILURE_TRANSFORM,
  LAGHU_OPERATIONAL_FAILURE_TRANSPORT,
  LAGHU_OPERATIONAL_FAILURE_COUNT
} laghu_operational_failure;

typedef struct {
  uint64_t active;
  uint64_t generation;
  uint64_t heartbeat;
  uint64_t surface;
  uint64_t process_kind;
  uint64_t required;
  uint64_t healthy;
  uint64_t queue_capacity;
  uint64_t queue_occupied;
  uint64_t requests;
  uint64_t decisions[LAGHU_OPERATIONAL_DECISION_COUNT];
  uint64_t cache_hits;
  uint64_t cache_misses;
  uint64_t cache_publications;
  uint64_t cache_evictions;
  uint64_t cache_bytes;
  uint64_t cache_files;
  uint64_t original_bytes;
  uint64_t selected_bytes;
  uint64_t saved_bytes;
  uint64_t failures[LAGHU_OPERATIONAL_FAILURE_COUNT];
  uint64_t budget_rejections[LAGHU_BUDGET_REJECTION_COUNT];
  uint64_t transform_memory_current;
  uint64_t transform_memory_peak;
  uint64_t transform_memory_limit;
  uint64_t transform_deadline_limit_ms;
  uint64_t cache_rejected_writes;
  uint64_t variant_occupancy;
  uint64_t variant_limit;
  uint64_t lcp_decisions[6];
  uint64_t lcp_applied;
  uint64_t lcp_profile_observations[4];
  uint64_t lcp_profile_ready[4];
  uint64_t latency_buckets[LAGHU_OPERATIONAL_LATENCY_BUCKETS];
  uint64_t latency_count;
  uint64_t latency_sum_microseconds;
} laghu_operational_slot_snapshot;

typedef struct {
  uint32_t version;
  uint32_t slot_count;
  uint64_t generation;
  laghu_operational_slot_snapshot slots[LAGHU_OPERATIONAL_MAX_SLOTS];
} laghu_operational_snapshot;

typedef struct {
  void *implementation;
} laghu_operational_registry;

typedef struct {
  bool runtime_ready;
  bool cache_ready;
  bool workers_ready;
  bool budgets_ready;
  bool degraded;
  unsigned int configured_workers;
  unsigned int healthy_workers;
} laghu_operational_readiness;
void laghu_operational_registry_init(laghu_operational_registry *registry);
bool laghu_operational_registry_open(laghu_operational_registry *registry,
                                     const char *cache_path,
                                     laghu_operational_surface surface,
                                     laghu_operational_process_kind kind,
                                     bool required, uint64_t now);
void laghu_operational_registry_close(laghu_operational_registry *registry);
bool laghu_operational_registry_heartbeat(laghu_operational_registry *registry,
                                          uint64_t now, bool healthy,
                                          uint64_t queue_capacity,
                                          uint64_t queue_occupied);
void laghu_operational_registry_record(laghu_operational_registry *registry,
                                       laghu_operational_decision decision,
                                       size_t original_bytes,
                                       size_t selected_bytes,
                                       uint64_t elapsed_microseconds);
void laghu_operational_registry_failure(laghu_operational_registry *registry,
                                        laghu_operational_failure failure);
void laghu_operational_registry_worker_job(laghu_operational_registry *registry,
                                           bool success,
                                           uint64_t elapsed_microseconds,
                                           laghu_operational_failure failure);
void laghu_operational_registry_cache(laghu_operational_registry *registry,
                                      const laghu_cache_stats *stats);
void laghu_operational_registry_budget(laghu_operational_registry *registry,
                                       const laghu_transform_budget *budget,
                                       unsigned int deadline_limit_ms);
void laghu_operational_registry_lcp(laghu_operational_registry *registry,
                                    laghu_lcp_decision decision, bool applied,
                                    const unsigned int observations[4],
                                    const bool ready[4]);
bool laghu_operational_registry_snapshot(laghu_operational_registry *registry,
                                         laghu_operational_snapshot *snapshot);
bool laghu_operational_render_prometheus(
    const laghu_operational_snapshot *snapshot, uint64_t now, char *output,
    size_t capacity, size_t *length);
bool laghu_operational_readiness_evaluate(
    const laghu_operational_snapshot *snapshot, uint64_t now,
    bool runtime_ready, bool cache_ready, bool strict_workers,
    laghu_operational_readiness *readiness);
bool laghu_operational_render_readiness(
    const laghu_operational_readiness *readiness, bool strict_workers,
    char *output, size_t capacity, size_t *length);

#ifdef __cplusplus
}
#endif

#endif
