// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_RUNTIME_H
#define LAGHU_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_QUEUE_VERSION 3U
#define LAGHU_QUEUE_DEFAULT_SLOTS 4U
#define LAGHU_RUNTIME_KEY_SIZE LAGHU_SHA256_HEX_SIZE
#define LAGHU_RUNTIME_PATH_SIZE 1024U
#define LAGHU_RUNTIME_TYPE_SIZE 64U
#define LAGHU_RUNTIME_VALIDATOR_SIZE 256U
#define LAGHU_RUNTIME_BACKEND_SIZE 128U

typedef struct {
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char request_path[LAGHU_RUNTIME_PATH_SIZE];
  char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  char content_type[LAGHU_RUNTIME_TYPE_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  uint64_t filters;
  unsigned int quality;
  unsigned int target_width;
  unsigned int target_height;
  uint64_t resize_filter;
  bool allow_lossy;
  bool accept_webp;
  laghu_buffer payload;
} laghu_runtime_job;

typedef struct {
  intptr_t platform_file;
  intptr_t platform_mapping;
  void *mapping;
  size_t mapping_length;
  unsigned int slot_count;
  size_t slot_payload_size;
  uint32_t capabilities;
  uint64_t worker_heartbeat;
  char backend_id[LAGHU_RUNTIME_BACKEND_SIZE];
} laghu_runtime_queue;

typedef struct {
  char variant_key[LAGHU_RUNTIME_KEY_SIZE];
  char payload_hash[LAGHU_RUNTIME_KEY_SIZE];
  char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  char content_type[LAGHU_RUNTIME_TYPE_SIZE];
  char backend_id[LAGHU_RUNTIME_BACKEND_SIZE];
  char variant_path[LAGHU_RUNTIME_PATH_SIZE];
  size_t length;
} laghu_runtime_cache_entry;

void laghu_runtime_queue_init(laghu_runtime_queue *queue);
bool laghu_runtime_queue_create(laghu_runtime_queue *queue, const char *path,
                                unsigned int slot_count,
                                size_t slot_payload_size);
bool laghu_runtime_queue_open(laghu_runtime_queue *queue, const char *path);
bool laghu_runtime_queue_refresh(laghu_runtime_queue *queue);
bool laghu_runtime_queue_set_backend(laghu_runtime_queue *queue,
                                     uint32_t capabilities,
                                     const char *backend_id);
bool laghu_runtime_queue_heartbeat(laghu_runtime_queue *queue,
                                   uint64_t epoch_seconds);
void laghu_runtime_queue_close(laghu_runtime_queue *queue);
bool laghu_runtime_queue_try_publish(laghu_runtime_queue *queue,
                                     const laghu_runtime_job *job);
bool laghu_runtime_queue_try_take(laghu_runtime_queue *queue,
                                  laghu_runtime_job *job,
                                  unsigned char *payload,
                                  size_t payload_capacity);

bool laghu_runtime_index_key(const char *request_path, const char *validator,
                             const char *policy_key, bool accept_webp,
                             char output[LAGHU_RUNTIME_KEY_SIZE]);
bool laghu_runtime_cache_publish(const char *cache_path, const char *index_key,
                                 const char *variant_key, const char *validator,
                                 const char *content_type,
                                 const char *backend_id, laghu_buffer payload,
                                 laghu_runtime_cache_entry *entry);
bool laghu_runtime_cache_lookup(const char *cache_path, const char *index_key,
                                const char *validator,
                                laghu_runtime_cache_entry *entry);
bool laghu_runtime_cache_read(const laghu_runtime_cache_entry *entry,
                              unsigned char *output, size_t output_capacity);

#ifdef __cplusplus
}
#endif

#endif
