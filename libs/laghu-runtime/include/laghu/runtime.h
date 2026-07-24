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
#include "laghu/image.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_QUEUE_VERSION 5U
#define LAGHU_QUEUE_DEFAULT_SLOTS 4U
#define LAGHU_RUNTIME_KEY_SIZE LAGHU_SHA256_HEX_SIZE
#define LAGHU_RUNTIME_PATH_SIZE 1024U
#define LAGHU_RUNTIME_TYPE_SIZE 64U
#define LAGHU_RUNTIME_VALIDATOR_SIZE 256U
#define LAGHU_RUNTIME_BACKEND_SIZE 128U
#define LAGHU_RUNTIME_MAX_TARGETS 2U
#define LAGHU_RUNTIME_MAX_SPRITE_INPUTS 32U
#define LAGHU_CATALOG_VERSION 2U
#define LAGHU_CATALOG_MAX_WIDTHS 8U
#define LAGHU_CATALOG_DEFAULT_LIMIT 10000U
#define LAGHU_CATALOG_DEFAULT_TTL 604800U
#define LAGHU_CSS_DERIVATION_VERSION 2U
#define LAGHU_STYLESHEET_CATALOG_VERSION 2U
#define LAGHU_CSS_IMPORT_MAX_DEPTH 8U
#define LAGHU_HTML_PLANNER_VERSION 2U
#define LAGHU_HTML_MAX_TOKENS 4096U
#define LAGHU_HTML_MAX_HEADS 16U

typedef uint32_t laghu_html_planner_mask;

#define LAGHU_HTML_PLAN_ADD_COMBINE_HEAD (UINT32_C(1) << 0)
#define LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD (UINT32_C(1) << 1)
#define LAGHU_HTML_PLAN_MOVE_CSS_ABOVE_SCRIPTS (UINT32_C(1) << 2)
#define LAGHU_HTML_PLAN_COLLAPSE_WHITESPACE (UINT32_C(1) << 3)
#define LAGHU_HTML_PLAN_REMOVE_COMMENTS (UINT32_C(1) << 4)
#define LAGHU_HTML_PLAN_REMOVE_QUOTES (UINT32_C(1) << 5)
#define LAGHU_HTML_PLAN_ELIDE_ATTRIBUTES (UINT32_C(1) << 6)
#define LAGHU_HTML_PLAN_LEXICAL                                            \
  (LAGHU_HTML_PLAN_COLLAPSE_WHITESPACE | LAGHU_HTML_PLAN_REMOVE_COMMENTS | \
   LAGHU_HTML_PLAN_REMOVE_QUOTES | LAGHU_HTML_PLAN_ELIDE_ATTRIBUTES)

typedef enum {
  LAGHU_RUNTIME_JOB_IMAGE = 0,
  LAGHU_RUNTIME_JOB_SPRITE = 1
} laghu_runtime_job_kind;

typedef struct {
  laghu_runtime_job_kind kind;
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char request_path[LAGHU_RUNTIME_PATH_SIZE];
  char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  char content_type[LAGHU_RUNTIME_TYPE_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
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
  uint32_t version;
  char normalized_url[LAGHU_RUNTIME_PATH_SIZE];
  char source_hash[LAGHU_RUNTIME_KEY_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  uint32_t capability_mask;
  unsigned int natural_width;
  unsigned int natural_height;
  char original_content_type[LAGHU_RUNTIME_TYPE_SIZE];
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
  unsigned char *data;
  size_t length;
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];
  bool rewritten;
  bool dependencies_pending;
} laghu_runtime_html_result;

typedef struct {
  unsigned char *data;
  size_t length;
  bool rewritten;
  bool added_head;
  bool combined_heads;
  bool moved_css;
  bool structural_changed;
  bool lexical_changed;
} laghu_runtime_head_result;

typedef struct {
  unsigned char *data;
  size_t length;
  size_t original_external_bytes;
  size_t combined_external_bytes;
  bool rewritten;
  bool dependencies_pending;
} laghu_runtime_css_combine_result;

typedef struct {
  unsigned char *data;
  size_t length;
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];
  bool rewritten;
  bool dependencies_pending;
  bool published;
  bool used_fallback;
} laghu_runtime_css_result;

typedef struct {
  unsigned char *data;
  size_t length;
  size_t original_external_bytes;
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];
  bool flattened;
  bool dependencies_pending;
  bool invalid;
} laghu_runtime_css_import_result;

typedef struct {
  uint32_t version;
  char normalized_url[LAGHU_RUNTIME_PATH_SIZE];
  char source_hash[LAGHU_RUNTIME_KEY_SIZE];
  char source_key[LAGHU_RUNTIME_KEY_SIZE];
  char derived_key[LAGHU_RUNTIME_KEY_SIZE];
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  uint32_t capability_mask;
  uint32_t parser_version;
  unsigned int inline_limit;
  unsigned int outline_threshold;
  size_t source_length;
  size_t derived_length;
  uint64_t updated_at;
  bool ready;
  bool terminally_excluded;
} laghu_stylesheet_record;

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
bool laghu_runtime_cache_lookup_variant(const char *cache_path,
                                        const char *variant_key,
                                        laghu_runtime_cache_entry *entry);
bool laghu_runtime_cache_read(const laghu_runtime_cache_entry *entry,
                              unsigned char *output, size_t output_capacity);
bool laghu_catalog_key(const char *normalized_url, const char *source_hash,
                       const char *policy_key, uint32_t capability_mask,
                       char output[LAGHU_RUNTIME_KEY_SIZE]);
bool laghu_catalog_publish(const char *cache_path, const char *catalog_key,
                           const laghu_catalog_record *record);
bool laghu_catalog_lookup(const char *cache_path, const char *catalog_key,
                          uint64_t now, unsigned int ttl_seconds,
                          laghu_catalog_record *record);
bool laghu_catalog_lookup_url(const char *cache_path,
                              const char *normalized_url,
                              const char *policy_key, uint32_t capability_mask,
                              uint64_t now, unsigned int ttl_seconds,
                              laghu_catalog_record *record);
bool laghu_catalog_publish_url(const char *cache_path,
                               const laghu_catalog_record *record);
bool laghu_runtime_rewrite_html(
    const char *cache_path, laghu_buffer html, const char *page_path,
    const char *page_origin, const char *policy_key, uint32_t capability_mask,
    uint64_t now, unsigned int ttl_seconds, laghu_image_filter_mask filters,
    bool allow_inline, bool allow_css_inline, bool allow_css_outline,
    bool allow_css_combine, laghu_html_planner_mask html_plan,
    bool csp_allows_data, bool csp_allows_inline_styles,
    bool csp_allows_self_styles, bool beacon_enabled, size_t inline_limit,
    unsigned int css_inline_limit, unsigned int css_outline_threshold,
    unsigned int viewport_width, unsigned int dpr_hundredths,
    laghu_runtime_html_result *result);
void laghu_runtime_html_result_release(laghu_runtime_html_result *result);
bool laghu_runtime_plan_html_document(laghu_buffer html,
                                      laghu_html_planner_mask plan,
                                      laghu_runtime_head_result *result);
void laghu_runtime_head_result_release(laghu_runtime_head_result *result);
bool laghu_runtime_rewrite_css_markup(
    const char *cache_path, laghu_buffer html, const char *page_path,
    const char *page_origin, const char *policy_key, uint32_t capability_mask,
    uint64_t now, unsigned int ttl_seconds, bool allow_inline,
    bool allow_outline, bool allow_combine, laghu_html_planner_mask html_plan,
    bool csp_allows_inline_styles, bool csp_allows_self_styles,
    unsigned int inline_limit, unsigned int outline_threshold,
    laghu_runtime_html_result *result);
bool laghu_runtime_combine_css_markup(
    const char *cache_path, laghu_buffer html, const char *page_path,
    const char *page_origin, const char *policy_key, uint32_t capability_mask,
    uint64_t now, unsigned int ttl_seconds, unsigned int inline_limit,
    unsigned int outline_threshold, laghu_runtime_css_combine_result *result);
void laghu_runtime_css_combine_result_release(
    laghu_runtime_css_combine_result *result);
bool laghu_runtime_csp_allows_self_styles(const char *csp,
                                          const char *page_origin);
bool laghu_runtime_rewrite_css(laghu_runtime_queue *queue,
                               const char *cache_path, laghu_buffer css,
                               const char *stylesheet_path,
                               const char *page_origin, const char *policy_key,
                               uint32_t capability_mask, uint64_t now,
                               unsigned int ttl_seconds, bool minify,
                               bool allow_sprites, unsigned int inline_limit,
                               unsigned int outline_threshold,
                               laghu_runtime_css_result *result);
void laghu_runtime_css_result_release(laghu_runtime_css_result *result);
bool laghu_runtime_flatten_css_imports(
    const char *cache_path, laghu_buffer css, const char *stylesheet_path,
    const char *page_origin, const char *policy_key, uint32_t capability_mask,
    uint64_t now, unsigned int ttl_seconds, unsigned int inline_limit,
    unsigned int outline_threshold, laghu_runtime_css_import_result *result);
void laghu_runtime_css_import_result_release(
    laghu_runtime_css_import_result *result);
bool laghu_stylesheet_publish(const char *cache_path,
                              const laghu_stylesheet_record *record);
bool laghu_stylesheet_lookup(const char *cache_path, const char *normalized_url,
                             const char *policy_key, uint32_t capability_mask,
                             unsigned int inline_limit,
                             unsigned int outline_threshold, uint64_t now,
                             unsigned int ttl_seconds,
                             laghu_stylesheet_record *record);
bool laghu_runtime_parse_image_beacon(laghu_buffer json,
                                      laghu_image_beacon_record *record);
bool laghu_catalog_apply_beacon(const char *cache_path, const char *policy_key,
                                uint32_t capability_mask, uint64_t now,
                                unsigned int ttl_seconds,
                                const laghu_image_beacon_record *beacon);
bool laghu_catalog_prune(const char *cache_path, uint64_t now,
                         unsigned int metadata_limit, unsigned int ttl_seconds);

#ifdef __cplusplus
}
#endif

#endif
