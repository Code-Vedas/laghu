// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_RUM_H
#define LAGHU_RUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"
#include "laghu/image.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_RUM_MAX_RECORDS 4096U
#define LAGHU_RUM_MAX_RECORD_BYTES 16384U
#define LAGHU_RUM_DEFAULT_MEMORY_BYTES (64U * 1024U * 1024U)
#define LAGHU_RUM_DEFAULT_PENDING_BYTES (16U * 1024U * 1024U)
#define LAGHU_RUM_DEFAULT_SYNC_SECONDS 5U
#define LAGHU_RUM_DEFAULT_TIMEOUT_MS 100U
#define LAGHU_RUM_DEFAULT_RETRY_LIMIT 3U
#define LAGHU_RUM_HISTOGRAMS 3U
#define LAGHU_RUM_BUCKETS 8U
typedef enum {
  LAGHU_RUM_RECORD_INSTRUMENTATION = 1,
  LAGHU_RUM_RECORD_CRITICAL_CSS = 2,
  LAGHU_RUM_RECORD_IMAGE = 3,
  LAGHU_RUM_RECORD_DECISION = 4
} laghu_rum_record_type;

typedef enum {
  LAGHU_RUM_HEALTH_READY = 0,
  LAGHU_RUM_HEALTH_DEGRADED,
  LAGHU_RUM_HEALTH_UNAVAILABLE
} laghu_rum_health;

typedef struct laghu_rum_engine laghu_rum_engine;

typedef struct {
  const char *store_uri;
  const char *snapshot_path;
  const char *client_library;
  size_t memory_limit;
  size_t pending_limit;
  unsigned int ttl_seconds;
  unsigned int sync_interval_seconds;
  unsigned int timeout_ms;
  unsigned int retry_limit;
  bool required;
} laghu_rum_options;

typedef struct {
  uint64_t generation;
  uint64_t updated_at;
  size_t length;
  laghu_rum_record_type type;
} laghu_rum_value;

typedef bool (*laghu_rum_mutator)(void *data, size_t length, void *context);

void laghu_rum_options_init(laghu_rum_options *options);
bool laghu_rum_store_validate(const char *uri, char *error, size_t error_size);
laghu_rum_engine *laghu_rum_engine_create(const laghu_rum_options *options,
                                          char *error, size_t error_size);
void laghu_rum_engine_destroy(laghu_rum_engine *engine);
bool laghu_rum_engine_read(laghu_rum_engine *engine, laghu_rum_record_type type,
                           const char *key, uint64_t now, void *data,
                           size_t capacity, laghu_rum_value *value);
bool laghu_rum_engine_publish(laghu_rum_engine *engine,
                              laghu_rum_record_type type, const char *key,
                              uint64_t updated_at, const void *data,
                              size_t length, uint64_t *generation);
bool laghu_rum_engine_update(laghu_rum_engine *engine,
                             laghu_rum_record_type type, const char *key,
                             uint64_t updated_at, laghu_rum_mutator mutator,
                             void *context, uint64_t *generation);
laghu_rum_health laghu_rum_engine_health(laghu_rum_engine *engine);
size_t laghu_rum_engine_memory_used(laghu_rum_engine *engine);
bool laghu_rum_record_merge(laghu_rum_record_type type, void *target,
                            const void *delta, size_t length);

#ifdef __cplusplus
}
#endif

#endif
