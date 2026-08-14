// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_CACHE_H
#define LAGHU_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"
#include "laghu/image.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_CACHE_DEFAULT_SIZE_BYTES (UINT64_C(10) * 1024U * 1024U * 1024U)
#define LAGHU_CACHE_DEFAULT_INODE_LIMIT 100000U
#define LAGHU_CACHE_DEFAULT_CLEAN_INTERVAL 60U
#define LAGHU_CACHE_DEFAULT_METADATA_BYTES (16U * 1024U * 1024U)
#define LAGHU_CACHE_LOW_WATER_PERCENT 90U
typedef struct {
  char variant_key[LAGHU_RUNTIME_KEY_SIZE];
  char payload_hash[LAGHU_RUNTIME_KEY_SIZE];
  char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  char content_type[LAGHU_RUNTIME_TYPE_SIZE];
  char backend_id[LAGHU_RUNTIME_BACKEND_SIZE];
  char variant_path[LAGHU_RUNTIME_PATH_SIZE];
  size_t length;
} laghu_runtime_cache_entry;

typedef struct {
  uint64_t size_limit;
  uint64_t inode_limit;
  size_t metadata_size;
  unsigned int clean_interval;
} laghu_cache_limits;

typedef struct {
  uint64_t bytes;
  uint64_t files;
  uint64_t hits;
  uint64_t misses;
  uint64_t rejected_publications;
  uint64_t variant_occupancy;
  uint64_t evictions;
  uint64_t corrupt_removals;
  uint64_t publications;
  uint64_t url_purges;
  uint64_t full_purges;
  uint64_t invalidated_artifacts;
  uint64_t invalidated_bytes;
  uint64_t cache_generation;
  uint64_t last_purge;
  uint64_t last_cleanup;
  bool cleaner_active;
  bool rebuilding;
} laghu_cache_stats;

typedef enum {
  LAGHU_CACHE_PURGE_ACCEPTED = 0,
  LAGHU_CACHE_PURGE_SATURATED,
  LAGHU_CACHE_PURGE_UNAVAILABLE,
  LAGHU_CACHE_PURGE_INVALID
} laghu_cache_purge_result;

typedef struct laghu_cache_backend laghu_cache_backend;

typedef struct {
  bool (*lookup)(laghu_cache_backend *backend, const char *index_key, const char *validator, laghu_runtime_cache_entry *entry);
  bool (*lookup_readonly)(laghu_cache_backend *backend, const char *index_key, const char *validator,
                          laghu_runtime_cache_entry *entry);
  bool (*lookup_variant)(laghu_cache_backend *backend, const char *variant_key, laghu_runtime_cache_entry *entry);
  bool (*read)(laghu_cache_backend *backend, const laghu_runtime_cache_entry *entry, unsigned char *output, size_t output_capacity);
  bool (*publish)(laghu_cache_backend *backend, const char *index_key, const char *variant_key, const char *validator, const char *content_type,
                  const char *backend_id, laghu_buffer payload, laghu_runtime_cache_entry *entry);
  bool (*touch)(laghu_cache_backend *backend, const char *variant_key, uint64_t now);
  bool (*remove)(laghu_cache_backend *backend, const char *variant_key);
  bool (*maintain)(laghu_cache_backend *backend, uint64_t now);
  bool (*health)(laghu_cache_backend *backend, laghu_cache_stats *stats);
  void (*close)(laghu_cache_backend *backend);
} laghu_cache_backend_contract;

struct laghu_cache_backend {
  const laghu_cache_backend_contract *contract;
  char uri[LAGHU_RUNTIME_PATH_SIZE];
  char path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_cache_limits limits;
  void *implementation;
};
bool laghu_runtime_index_key(const char *request_path, const char *validator, const char *policy_key, bool accept_webp, bool accept_avif,
                             unsigned int target_width, unsigned int target_height, char output[LAGHU_RUNTIME_KEY_SIZE]);
bool laghu_runtime_index_key_variant(const char *request_path, const char *validator, const char *policy_key, bool accept_webp,
                                     bool accept_avif, unsigned int target_width, unsigned int target_height, unsigned int policy_variant,
                                     char output[LAGHU_RUNTIME_KEY_SIZE]);
bool laghu_runtime_index_key_content_class(const char *base_key, laghu_image_content_class content,
                                           char output[LAGHU_RUNTIME_KEY_SIZE]);
bool laghu_runtime_cache_publish(const char *cache_path, const char *index_key, const char *variant_key, const char *validator,
                                 const char *content_type, const char *backend_id, laghu_buffer payload, laghu_runtime_cache_entry *entry);
bool laghu_runtime_cache_lookup(const char *cache_path, const char *index_key, const char *validator, laghu_runtime_cache_entry *entry);
/* Reads a published artifact without mutating shared cache accounting. */
bool laghu_runtime_cache_lookup_readonly(const char *cache_path, const char *index_key, const char *validator,
                                        laghu_runtime_cache_entry *entry);
bool laghu_runtime_cache_lookup_variant(const char *cache_path, const char *variant_key, laghu_runtime_cache_entry *entry);
bool laghu_runtime_cache_read(const laghu_runtime_cache_entry *entry, unsigned char *output, size_t output_capacity);
bool laghu_runtime_file_cache_publish(const char *cache_path, const char *index_key, const char *variant_key, const char *validator,
                                      const char *content_type, const char *backend_id, laghu_buffer payload, laghu_runtime_cache_entry *entry);
bool laghu_runtime_file_cache_lookup(const char *cache_path, const char *index_key, const char *validator, laghu_runtime_cache_entry *entry);
bool laghu_runtime_file_cache_lookup_variant(const char *cache_path, const char *variant_key, laghu_runtime_cache_entry *entry);
/* Performs a full streaming integrity check; use from maintenance/recovery, not response paths. */
bool laghu_runtime_file_cache_verify(const laghu_runtime_cache_entry *entry);
bool laghu_runtime_file_cache_read(const laghu_runtime_cache_entry *entry, unsigned char *output, size_t output_capacity);
void laghu_cache_limits_init(laghu_cache_limits *limits);
bool laghu_cache_backend_uri_parse(const char *uri, char *path, size_t path_size);
bool laghu_cache_size_parse(const char *value, uint64_t minimum, uint64_t maximum, uint64_t *result);
bool laghu_cache_count_parse(const char *value, uint64_t minimum, uint64_t maximum, uint64_t *result);
bool laghu_cache_duration_parse(const char *value, unsigned int minimum, unsigned int maximum, unsigned int *result);
bool laghu_cache_backend_open(laghu_cache_backend *backend, const char *uri, const laghu_cache_limits *limits);
bool laghu_cache_backend_open_path(laghu_cache_backend *backend, const char *path, const laghu_cache_limits *limits);
bool laghu_cache_backend_register_path(const char *path, const laghu_cache_limits *limits);
bool laghu_cache_backend_maintain_path(const char *path, uint64_t now);
void laghu_cache_backend_close(laghu_cache_backend *backend);
bool laghu_cache_backend_lookup(laghu_cache_backend *backend, const char *index_key, const char *validator, laghu_runtime_cache_entry *entry);
bool laghu_cache_backend_lookup_readonly(laghu_cache_backend *backend, const char *index_key, const char *validator,
                                        laghu_runtime_cache_entry *entry);
bool laghu_cache_backend_lookup_variant(laghu_cache_backend *backend, const char *variant_key, laghu_runtime_cache_entry *entry);
bool laghu_cache_backend_read(laghu_cache_backend *backend, const laghu_runtime_cache_entry *entry, unsigned char *output, size_t output_capacity);
bool laghu_cache_backend_publish(laghu_cache_backend *backend, const char *index_key, const char *variant_key, const char *validator,
                                 const char *content_type, const char *backend_id, laghu_buffer payload, laghu_runtime_cache_entry *entry);
bool laghu_cache_backend_maintain(laghu_cache_backend *backend, uint64_t now);
bool laghu_cache_backend_health(laghu_cache_backend *backend, laghu_cache_stats *stats);
bool laghu_cache_backend_health_path(const char *path, laghu_cache_stats *stats);
bool laghu_cache_source_normalize(const char *target, char *normalized, size_t normalized_size, bool *purge_control);
bool laghu_cache_source_hash(const char *target, char output[LAGHU_RUNTIME_KEY_SIZE]);
bool laghu_cache_backend_associate(laghu_cache_backend *backend, const char *index_key, const char *source_hash);
bool laghu_cache_backend_associate_path(const char *path, const char *index_key, const char *source_target);
void laghu_cache_source_scope(const char *path, const char *source_target);
void laghu_cache_variant_limit_scope(const char *path, unsigned int limit);
laghu_cache_purge_result laghu_cache_backend_purge_url(laghu_cache_backend *backend, const char *source_target, uint64_t now,
                                                       uint64_t *matched_artifacts);
laghu_cache_purge_result laghu_cache_backend_purge_url_path(const char *path, const char *source_target, uint64_t now, uint64_t *matched_artifacts);
bool laghu_cache_backend_flush(laghu_cache_backend *backend, uint64_t requested_generation, uint64_t now, uint64_t *generation);
bool laghu_cache_backend_flush_path(const char *path, uint64_t requested_generation, uint64_t now, uint64_t *generation);
bool laghu_cache_flush_file_poll(const char *path, const char *flush_file, uint64_t now, uint64_t *generation);

#ifdef __cplusplus
}
#endif

#endif
