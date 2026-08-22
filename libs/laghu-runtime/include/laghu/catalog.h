// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_CATALOG_H
#define LAGHU_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"
#include "laghu/image.h"
#include "laghu/rum.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_CATALOG_MAX_WIDTHS 8U
#define LAGHU_CATALOG_DEFAULT_LIMIT 10000U
#define LAGHU_CATALOG_DEFAULT_TTL 604800U
typedef struct {
  unsigned int width;
  unsigned int height;
  char variant_key[LAGHU_RUNTIME_KEY_SIZE];
  char content_type[LAGHU_RUNTIME_TYPE_SIZE];
  size_t original_length;
  size_t variant_length;
  bool ready;
  bool terminally_excluded;
} laghu_catalog_variant;

typedef struct {
  char normalized_url[LAGHU_RUNTIME_PATH_SIZE];
  char source_hash[LAGHU_RUNTIME_KEY_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  uint32_t capability_mask;
  unsigned int natural_width;
  unsigned int natural_height;
  char original_content_type[LAGHU_RUNTIME_TYPE_SIZE];
  laghu_image_content_class content_class;
  laghu_catalog_variant variants[LAGHU_CATALOG_MAX_WIDTHS];
  unsigned int variant_count;
  uint64_t updated_at;
  uint64_t last_accessed_at;
  char preview_data_uri[4096U];
  unsigned int learned_width;
  unsigned int learned_height;
  unsigned int learned_mobile_width;
  unsigned int learned_mobile_height;
  unsigned int learned_viewport_width;
  unsigned int learned_dpr_hundredths;
  uint64_t learned_at;
  bool learned_above_fold;
  /* Both encodes must be present before HTML becomes video markup. */
  char gif_video_mp4_key[LAGHU_RUNTIME_KEY_SIZE];
  char gif_video_webm_key[LAGHU_RUNTIME_KEY_SIZE];
  size_t gif_video_mp4_length;
  size_t gif_video_webm_length;
  bool gif_video_ready;
  bool gif_video_excluded;
} laghu_catalog_record;

typedef struct {
  char normalized_url[LAGHU_RUNTIME_PATH_SIZE];
  unsigned int width;
  unsigned int height;
  unsigned int viewport_width;
  unsigned int dpr_hundredths;
  bool above_fold;
  bool mobile;
} laghu_image_beacon_record;

typedef struct {
  char identity[LAGHU_RUNTIME_KEY_SIZE];
  uint64_t updated_at;
  unsigned int width;
  unsigned int height;
  unsigned int mobile_width;
  unsigned int mobile_height;
  unsigned int viewport_width;
  unsigned int dpr_hundredths;
  bool above_fold;
} laghu_rum_image_record;
bool laghu_catalog_key(const char *normalized_url, const char *source_hash, const char *policy_key, uint32_t capability_mask,
                       char output[LAGHU_RUNTIME_KEY_SIZE]);
bool laghu_catalog_publish(const char *cache_path, const char *catalog_key, const laghu_catalog_record *record);
bool laghu_catalog_lookup(const char *cache_path, const char *catalog_key, uint64_t now, unsigned int ttl_seconds, laghu_catalog_record *record);
bool laghu_catalog_lookup_url(const char *cache_path, const char *normalized_url, const char *policy_key, uint32_t capability_mask, uint64_t now,
                              unsigned int ttl_seconds, laghu_catalog_record *record);
bool laghu_catalog_publish_url(const char *cache_path, const laghu_catalog_record *record);
bool laghu_catalog_url_identity(const char *normalized_url, const char *policy_key, uint32_t capability_mask, char output[LAGHU_RUNTIME_KEY_SIZE]);
bool laghu_runtime_parse_image_beacon(laghu_buffer json, laghu_image_beacon_record *record);
const char *laghu_runtime_image_beacon_script(void);
bool laghu_catalog_apply_beacon(laghu_rum_engine *rum, const char *cache_path, const char *policy_key, uint32_t capability_mask, uint64_t now,
                                unsigned int ttl_seconds, const laghu_image_beacon_record *beacon);
bool laghu_catalog_prune(const char *cache_path, uint64_t now, unsigned int metadata_limit, unsigned int ttl_seconds);

#ifdef __cplusplus
}
#endif

#endif
