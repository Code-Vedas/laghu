// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_QUEUE_H
#define LAGHU_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"
#include "laghu/image.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_QUEUE_DEFAULT_SLOTS 4U
typedef enum {
  LAGHU_RUNTIME_JOB_IMAGE = 0,
  LAGHU_RUNTIME_JOB_SPRITE = 1,
  LAGHU_RUNTIME_JOB_FONT_CSS = 2,
  LAGHU_RUNTIME_JOB_JAVASCRIPT = 3
} laghu_runtime_job_kind;

typedef struct {
  laghu_runtime_job_kind kind;
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char request_path[LAGHU_RUNTIME_PATH_SIZE];
  char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  char content_type[LAGHU_RUNTIME_TYPE_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  char provider_id[LAGHU_FONT_PROVIDER_ID_SIZE];
  char provider_digest[LAGHU_RUNTIME_KEY_SIZE];
  char javascript_target[LAGHU_JAVASCRIPT_TARGET_SIZE];
  uint64_t filters;
  unsigned int quality;
  unsigned int metadata_limit;
  unsigned int metadata_ttl;
  unsigned int target_count;
  unsigned int target_width[LAGHU_RUNTIME_MAX_TARGETS];
  unsigned int target_height[LAGHU_RUNTIME_MAX_TARGETS];
  uint64_t resize_filter[LAGHU_RUNTIME_MAX_TARGETS];
  unsigned int sprite_count;
  char sprite_variant_keys[LAGHU_RUNTIME_MAX_SPRITE_INPUTS]
                          [LAGHU_RUNTIME_KEY_SIZE];
  unsigned int sprite_width[LAGHU_RUNTIME_MAX_SPRITE_INPUTS];
  unsigned int sprite_height[LAGHU_RUNTIME_MAX_SPRITE_INPUTS];
  bool allow_lossy;
  bool accept_webp;
  laghu_buffer payload;
} laghu_runtime_job;
typedef struct {
  uint32_t capabilities;
  uint64_t worker_heartbeat;
  char backend_id[LAGHU_RUNTIME_BACKEND_SIZE];
  uint64_t capacity;
  uint64_t occupied;
  size_t payload_capacity;
} laghu_runtime_queue_snapshot;

/* Queue storage and its native mapping are private to the runtime. */
typedef struct {
  void *implementation;
} laghu_runtime_queue;
void laghu_runtime_queue_init(laghu_runtime_queue *queue);
bool laghu_runtime_queue_create(laghu_runtime_queue *queue, const char *path,
                                unsigned int slot_count,
                                size_t slot_payload_size);
bool laghu_runtime_queue_open(laghu_runtime_queue *queue, const char *path);
bool laghu_runtime_queue_move(laghu_runtime_queue *destination,
                              laghu_runtime_queue *source);
bool laghu_runtime_queue_refresh(laghu_runtime_queue *queue);
bool laghu_runtime_queue_set_backend(laghu_runtime_queue *queue,
                                     uint32_t capabilities,
                                     const char *backend_id);
bool laghu_runtime_queue_heartbeat(laghu_runtime_queue *queue,
                                   uint64_t epoch_seconds);
bool laghu_runtime_queue_snapshot_get(const laghu_runtime_queue *queue,
                                      laghu_runtime_queue_snapshot *snapshot);
bool laghu_runtime_queue_status(laghu_runtime_queue *queue, uint64_t *capacity,
                                uint64_t *occupied);
void laghu_runtime_queue_close(laghu_runtime_queue *queue);
bool laghu_runtime_queue_try_publish(laghu_runtime_queue *queue,
                                     const laghu_runtime_job *job);
bool laghu_runtime_queue_try_take(laghu_runtime_queue *queue,
                                  laghu_runtime_job *job,
                                  unsigned char *payload,
                                  size_t payload_capacity);

#ifdef __cplusplus
}
#endif

#endif
