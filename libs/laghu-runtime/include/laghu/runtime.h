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

#define LAGHU_QUEUE_VERSION 8U
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
#define LAGHU_HTML_PLANNER_VERSION 4U
#define LAGHU_CRITICAL_CSS_VERSION 1U
#define LAGHU_CRITICAL_CSS_MAX_RULES 512U
#define LAGHU_CRITICAL_CSS_QUORUM 3U
#define LAGHU_HTML_MAX_TOKENS 4096U
#define LAGHU_HTML_MAX_HEADS 16U
#define LAGHU_HTML_MAX_PRELOADS 4U
#define LAGHU_HTML_MAX_DNS_PREFETCH 8U
#define LAGHU_HTML_MAX_LINK_HEADERS 12U
#define LAGHU_HTML_HEADER_VALUE_SIZE 1152U
#define LAGHU_HTML_LANGUAGE_SIZE 128U
#define LAGHU_FONT_PROVIDER_MAX 16U
#define LAGHU_FONT_PROVIDER_RULE_MAX 8U
#define LAGHU_FONT_PROVIDER_ID_SIZE 64U
#define LAGHU_FONT_PROVIDER_HOST_SIZE 256U
#define LAGHU_FONT_PROVIDER_PREFIX_SIZE 512U
#define LAGHU_FONT_CSS_MAX_BYTES 262144U
#define LAGHU_FONT_FETCH_PROFILE_VERSION 1U
#define LAGHU_JAVASCRIPT_MAX_BYTES 2097152U
#define LAGHU_JAVASCRIPT_TARGET_SIZE 512U
#define LAGHU_JAVASCRIPT_MAX_SCRIPTS 64U
#define LAGHU_JAVASCRIPT_DERIVATION_VERSION 2U
#define LAGHU_INSTRUMENTATION_VERSION 1U
#define LAGHU_INSTRUMENTATION_MAX_PROVIDERS 32U
#define LAGHU_INSTRUMENTATION_MAX_SCRIPTS 64U
#define LAGHU_JAVASCRIPT_DEFER_MAX_RULES 64U
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

typedef uint32_t laghu_html_planner_mask;

#define LAGHU_HTML_PLAN_ADD_COMBINE_HEAD (UINT32_C(1) << 0)
#define LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD (UINT32_C(1) << 1)
#define LAGHU_HTML_PLAN_MOVE_CSS_ABOVE_SCRIPTS (UINT32_C(1) << 2)
#define LAGHU_HTML_PLAN_COLLAPSE_WHITESPACE (UINT32_C(1) << 3)
#define LAGHU_HTML_PLAN_REMOVE_COMMENTS (UINT32_C(1) << 4)
#define LAGHU_HTML_PLAN_REMOVE_QUOTES (UINT32_C(1) << 5)
#define LAGHU_HTML_PLAN_ELIDE_ATTRIBUTES (UINT32_C(1) << 6)
#define LAGHU_HTML_PLAN_CONVERT_META_TAGS (UINT32_C(1) << 7)
#define LAGHU_HTML_PLAN_RESOURCE_HINTS (UINT32_C(1) << 8)
#define LAGHU_HTML_PLAN_TRIM_URLS (UINT32_C(1) << 9)
#define LAGHU_HTML_PLAN_LEXICAL                                            \
  (LAGHU_HTML_PLAN_COLLAPSE_WHITESPACE | LAGHU_HTML_PLAN_REMOVE_COMMENTS | \
   LAGHU_HTML_PLAN_REMOVE_QUOTES | LAGHU_HTML_PLAN_ELIDE_ATTRIBUTES |      \
   LAGHU_HTML_PLAN_TRIM_URLS)

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
  char host[LAGHU_FONT_PROVIDER_HOST_SIZE];
  char path_prefix[LAGHU_FONT_PROVIDER_PREFIX_SIZE];
} laghu_font_provider_rule;

typedef struct {
  char id[LAGHU_FONT_PROVIDER_ID_SIZE];
  laghu_font_provider_rule stylesheets[LAGHU_FONT_PROVIDER_RULE_MAX];
  unsigned int stylesheet_count;
  laghu_font_provider_rule redirects[LAGHU_FONT_PROVIDER_RULE_MAX];
  unsigned int redirect_count;
  laghu_font_provider_rule assets[LAGHU_FONT_PROVIDER_RULE_MAX];
  unsigned int asset_count;
  unsigned int max_css_bytes;
  unsigned int ttl_seconds;
  char digest[LAGHU_RUNTIME_KEY_SIZE];
} laghu_font_provider;

typedef struct {
  laghu_font_provider providers[LAGHU_FONT_PROVIDER_MAX];
  unsigned int count;
  char digest[LAGHU_RUNTIME_KEY_SIZE];
} laghu_font_provider_set;

typedef struct {
  char provider_id[LAGHU_FONT_PROVIDER_ID_SIZE];
  char provider_digest[LAGHU_RUNTIME_KEY_SIZE];
  char normalized_url[LAGHU_RUNTIME_PATH_SIZE];
  char variant_key[LAGHU_RUNTIME_KEY_SIZE];
  size_t css_length;
  uint64_t fetched_at;
  uint64_t retry_after;
  unsigned int ttl_seconds;
  bool ready;
  bool terminally_excluded;
} laghu_font_stylesheet_record;

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
  unsigned char *data;
  size_t length;
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];
  bool rewritten;
  bool published;
} laghu_runtime_javascript_result;

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
  uint32_t version;
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

typedef struct {
  uint32_t version;
  char template_key[LAGHU_RUNTIME_KEY_SIZE];
  char provider_digest[LAGHU_RUNTIME_KEY_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  uint64_t updated_at;
  unsigned int script_count;
  char script_keys[LAGHU_INSTRUMENTATION_MAX_SCRIPTS][LAGHU_RUNTIME_KEY_SIZE];
  uint64_t observations[2];
  uint64_t metric_sums[2][5];
  unsigned int metric_maxima[2][5];
  uint64_t histograms[2][LAGHU_RUM_HISTOGRAMS][LAGHU_RUM_BUCKETS];
  uint64_t errors[2];
  uint64_t rejections[2];
  uint64_t script_observations[2][LAGHU_INSTRUMENTATION_MAX_SCRIPTS];
  uint64_t script_before_dcl[2][LAGHU_INSTRUMENTATION_MAX_SCRIPTS];
  uint64_t script_long_tasks[2][LAGHU_INSTRUMENTATION_MAX_SCRIPTS];
} laghu_rum_instrumentation_record;

typedef struct {
  unsigned char *data;
  size_t length;
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];
  char content_language[LAGHU_HTML_LANGUAGE_SIZE];
  char link_headers[LAGHU_HTML_MAX_LINK_HEADERS][LAGHU_HTML_HEADER_VALUE_SIZE];
  unsigned int link_header_count;
  bool set_content_language;
  bool invalid;
  bool rewritten;
  bool dependencies_pending;
  bool javascript_defer_recommended;
  char javascript_defer_path[LAGHU_RUNTIME_PATH_SIZE];
  char javascript_defer_template[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int javascript_defer_bucket;
  uint64_t javascript_defer_observations;
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

typedef struct {
  uint32_t version;
  char template_key[LAGHU_RUNTIME_KEY_SIZE];
  char stylesheet_url[LAGHU_RUNTIME_PATH_SIZE];
  char stylesheet_key[LAGHU_RUNTIME_KEY_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  uint64_t updated_at;
  uint32_t generation;
  uint16_t observation_count[2];
  unsigned char critical_rules[2][LAGHU_CRITICAL_CSS_MAX_RULES / 8U];
} laghu_critical_css_record;

typedef struct {
  char template_key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int viewport_bucket;
  unsigned int rule_count;
  uint16_t rules[LAGHU_CRITICAL_CSS_MAX_RULES];
} laghu_critical_css_beacon;

typedef struct {
  char host[LAGHU_FONT_PROVIDER_HOST_SIZE];
  char path_prefix[LAGHU_FONT_PROVIDER_PREFIX_SIZE];
} laghu_javascript_observation_rule;

typedef struct {
  laghu_javascript_observation_rule rules[LAGHU_INSTRUMENTATION_MAX_PROVIDERS];
  unsigned int count;
  char digest[LAGHU_RUNTIME_KEY_SIZE];
} laghu_javascript_observation_set;

typedef struct {
  char script_path[LAGHU_RUNTIME_PATH_SIZE];
  char template_path[LAGHU_RUNTIME_PATH_SIZE];
} laghu_javascript_defer_rule;

typedef struct {
  laghu_javascript_defer_rule rules[LAGHU_JAVASCRIPT_DEFER_MAX_RULES];
  unsigned int count;
  char digest[LAGHU_RUNTIME_KEY_SIZE];
} laghu_javascript_defer_set;

typedef struct {
  char key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int before_dcl;
  unsigned int long_tasks;
} laghu_instrumentation_candidate;

typedef struct {
  char template_key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int bucket;
  unsigned int lcp_ms;
  unsigned int inp_ms;
  unsigned int cls_milli;
  unsigned int dcl_ms;
  unsigned int load_ms;
  unsigned int errors;
  unsigned int rejections;
  laghu_instrumentation_candidate candidates[LAGHU_INSTRUMENTATION_MAX_SCRIPTS];
  unsigned int candidate_count;
} laghu_instrumentation_beacon;

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

bool laghu_font_providers_load(const char *path, laghu_font_provider_set *set,
                               char *error, size_t error_size);
const laghu_font_provider *laghu_font_provider_match(
    const laghu_font_provider_set *set, const char *url);
const laghu_font_provider *laghu_font_provider_by_id(
    const laghu_font_provider_set *set, const char *id);
bool laghu_font_provider_url_allowed(const laghu_font_provider *provider,
                                     const char *url, bool redirect,
                                     bool asset);
bool laghu_font_css_validate(const laghu_font_provider *provider,
                             laghu_buffer css);
bool laghu_font_stylesheet_key(const char *url, const char *provider_digest,
                               char output[LAGHU_RUNTIME_KEY_SIZE]);
bool laghu_font_stylesheet_publish(const char *cache_path,
                                   const laghu_font_stylesheet_record *record);
bool laghu_font_stylesheet_lookup(const char *cache_path, const char *url,
                                  const laghu_font_provider *provider,
                                  uint64_t now,
                                  laghu_font_stylesheet_record *record);

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
bool laghu_javascript_target_normalize(
    const char *target, char output[LAGHU_JAVASCRIPT_TARGET_SIZE]);
bool laghu_runtime_rewrite_javascript(
    laghu_runtime_queue *queue, const char *cache_path, laghu_buffer source,
    const char *normalized_path, const char *policy_key, const char *target,
    bool module, bool include_source_map,
    laghu_runtime_javascript_result *result);
bool laghu_runtime_rewrite_javascript_html(
    laghu_runtime_queue *queue, const char *cache_path, laghu_buffer html,
    const char *page_path, const char *policy_key, const char *target,
    const char *content_security_policy, uint64_t now, unsigned int ttl_seconds,
    laghu_rum_engine *rum, const char *template_key, const char *page_origin,
    unsigned int viewport_bucket, const laghu_javascript_defer_set *defer_set,
    bool allow_defer, bool allow_defer_suggestions, bool allow_combine,
    bool allow_inline, bool allow_outline, bool include_source_maps,
    unsigned int inline_limit, unsigned int outline_threshold,
    laghu_runtime_html_result *result);
void laghu_runtime_javascript_result_release(
    laghu_runtime_javascript_result *result);
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
bool laghu_catalog_url_identity(const char *normalized_url,
                                const char *policy_key,
                                uint32_t capability_mask,
                                char output[LAGHU_RUNTIME_KEY_SIZE]);
bool laghu_runtime_rewrite_html(
    laghu_rum_engine *rum, const char *cache_path, laghu_buffer html,
    const char *page_path, const char *page_origin, const char *policy_key,
    uint32_t capability_mask, uint64_t now, unsigned int ttl_seconds,
    laghu_image_filter_mask filters, bool allow_inline, bool allow_css_inline,
    bool allow_css_outline, bool allow_css_combine,
    laghu_html_planner_mask html_plan, bool csp_allows_data,
    bool csp_allows_inline_styles, bool csp_allows_self_styles,
    bool beacon_enabled, size_t inline_limit, unsigned int css_inline_limit,
    unsigned int css_outline_threshold, unsigned int viewport_width,
    unsigned int dpr_hundredths, laghu_runtime_html_result *result);
void laghu_runtime_html_result_release(laghu_runtime_html_result *result);
bool laghu_runtime_finalize_html_headers(
    const char *cache_path, laghu_buffer html, const char *page_path,
    const char *page_origin, const char *policy_key, uint32_t capability_mask,
    uint64_t now, unsigned int ttl_seconds, laghu_html_planner_mask plan,
    const char *existing_content_language, const char *existing_link_headers,
    unsigned int css_inline_limit, unsigned int css_outline_threshold,
    bool already_warm, laghu_runtime_html_result *result);
bool laghu_runtime_plan_html_document(laghu_buffer html,
                                      laghu_html_planner_mask plan,
                                      laghu_runtime_head_result *result);
bool laghu_runtime_plan_html_document_at(laghu_buffer html,
                                         const char *page_path,
                                         const char *page_origin,
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
bool laghu_runtime_rewrite_font_css(
    laghu_runtime_queue *fetch_queue, const char *cache_path,
    const laghu_font_provider_set *providers, laghu_buffer html, uint64_t now,
    bool allow_inline, bool csp_allows_inline_styles, unsigned int inline_limit,
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
bool laghu_runtime_csp_allows_self_scripts(const char *csp,
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
bool laghu_catalog_apply_beacon(laghu_rum_engine *rum, const char *cache_path,
                                const char *policy_key,
                                uint32_t capability_mask, uint64_t now,
                                unsigned int ttl_seconds,
                                const laghu_image_beacon_record *beacon);
bool laghu_catalog_prune(const char *cache_path, uint64_t now,
                         unsigned int metadata_limit, unsigned int ttl_seconds);
bool laghu_runtime_prioritize_critical_css(
    laghu_rum_engine *rum, const char *cache_path, laghu_buffer html,
    const char *page_path, const char *page_origin, const char *policy_key,
    uint32_t capability_mask, uint64_t now, unsigned int ttl_seconds,
    unsigned int inline_limit, unsigned int outline_threshold,
    unsigned int viewport_width, bool beacon_enabled,
    bool csp_allows_inline_styles, bool csp_allows_self_styles,
    bool csp_allows_self_scripts, laghu_runtime_html_result *result);
bool laghu_runtime_parse_critical_css_beacon(laghu_buffer json,
                                             laghu_critical_css_beacon *record);
bool laghu_critical_css_apply_beacon(laghu_rum_engine *rum,
                                     const char *cache_path,
                                     const char *policy_key, uint64_t now,
                                     unsigned int ttl_seconds,
                                     const laghu_critical_css_beacon *beacon);
const char *laghu_runtime_critical_css_beacon_script(void);
bool laghu_javascript_observations_load(const char *path,
                                        laghu_javascript_observation_set *set,
                                        char *error, size_t error_size);
bool laghu_javascript_defer_load(const char *path,
                                 laghu_javascript_defer_set *set, char *error,
                                 size_t error_size);
bool laghu_javascript_defer_approved(const laghu_javascript_defer_set *set,
                                     const char *script_path,
                                     const char *template_path);
bool laghu_javascript_defer_recommended(
    const laghu_rum_instrumentation_record *record, unsigned int bucket,
    unsigned int script_index);
bool laghu_javascript_defer_rollback_recommended(
    const laghu_rum_instrumentation_record *baseline,
    const laghu_rum_instrumentation_record *current, unsigned int bucket);
bool laghu_runtime_add_instrumentation(
    laghu_rum_engine *rum, const char *cache_path,
    const laghu_javascript_observation_set *providers, laghu_buffer html,
    const char *page_path, const char *page_origin, const char *policy_key,
    uint64_t now, unsigned int ttl_seconds, unsigned int sample_rate,
    bool csp_allows_self_scripts, laghu_runtime_html_result *result);
bool laghu_runtime_instrumentation_template_key(
    laghu_rum_engine *rum, const char *cache_path,
    const laghu_javascript_observation_set *providers, laghu_buffer html,
    const char *page_path, const char *page_origin, const char *policy_key,
    uint64_t now, unsigned int ttl_seconds, unsigned int sample_rate,
    char output[LAGHU_RUNTIME_KEY_SIZE]);
const char *laghu_runtime_instrumentation_script(void);
bool laghu_runtime_parse_instrumentation_beacon(
    laghu_buffer json, laghu_instrumentation_beacon *record);
bool laghu_instrumentation_apply_beacon(
    laghu_rum_engine *rum, const char *cache_path, uint64_t now,
    unsigned int ttl_seconds, const laghu_instrumentation_beacon *beacon);

#ifdef __cplusplus
}
#endif

#endif
