// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_CORE_H
#define LAGHU_CORE_H

#include "laghu/base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_VERSION "0.1.0"
#define LAGHU_MIME_ALLOWLIST_SIZE 2048U
#define LAGHU_VARIANT_KEY_VERSION 11U
#define LAGHU_IMAGE_QUALITY_UNSET 0U
#define LAGHU_IMAGE_INLINE_LIMIT_UNSET UINT32_MAX
#define LAGHU_IMAGE_METADATA_LIMIT_UNSET 0U
#define LAGHU_IMAGE_METADATA_TTL_UNSET 0U
#define LAGHU_IMAGE_INLINE_LIMIT_DEFAULT 2048U
#define LAGHU_IMAGE_METADATA_LIMIT_DEFAULT 10000U
#define LAGHU_IMAGE_METADATA_TTL_DEFAULT 604800U
#define LAGHU_CSS_INLINE_LIMIT_UNSET UINT32_MAX
#define LAGHU_CSS_INLINE_LIMIT_DEFAULT 2048U
#define LAGHU_CSS_OUTLINE_THRESHOLD_UNSET 0U
#define LAGHU_CSS_OUTLINE_THRESHOLD_DEFAULT 8192U
#define LAGHU_JAVASCRIPT_INLINE_LIMIT_UNSET UINT32_MAX
#define LAGHU_JAVASCRIPT_INLINE_LIMIT_DEFAULT 2048U
#define LAGHU_JAVASCRIPT_OUTLINE_THRESHOLD_UNSET 0U
#define LAGHU_JAVASCRIPT_OUTLINE_THRESHOLD_DEFAULT 8192U
#define LAGHU_INSTRUMENTATION_SAMPLE_RATE_UNSET UINT32_MAX
#define LAGHU_INSTRUMENTATION_SAMPLE_RATE_DEFAULT 10U
#define LAGHU_RESOURCE_RULE_LIMIT 8U
#define LAGHU_RESOURCE_PATTERN_SIZE 128U
#define LAGHU_QUERY_OVERRIDE_SIZE 1024U
#define LAGHU_TRANSFORM_MEMORY_LIMIT_UNSET 0U
#define LAGHU_TRANSFORM_MEMORY_LIMIT_DEFAULT (32U * 1024U * 1024U)
#define LAGHU_TRANSFORM_MEMORY_LIMIT_MIN (4U * 1024U * 1024U)
#define LAGHU_TRANSFORM_MEMORY_LIMIT_MAX (256U * 1024U * 1024U)
#define LAGHU_TRANSFORM_DEADLINE_MS_UNSET 0U
#define LAGHU_TRANSFORM_DEADLINE_MS_DEFAULT 50U
#define LAGHU_TRANSFORM_DEADLINE_MS_MIN 5U
#define LAGHU_TRANSFORM_DEADLINE_MS_MAX 1000U
#define LAGHU_VARIANTS_PER_SOURCE_UNSET 0U
#define LAGHU_VARIANTS_PER_SOURCE_DEFAULT 16U
#define LAGHU_VARIANTS_PER_SOURCE_MIN 1U
#define LAGHU_VARIANTS_PER_SOURCE_MAX 64U
#define LAGHU_DOMAIN_POLICY_MAX_GROUPS 8U
#define LAGHU_DOMAIN_POLICY_MAX_SHARDS 8U
#define LAGHU_DOMAIN_POLICY_MAX_DOMAINS 8U
#define LAGHU_DOMAIN_POLICY_MAX_MAPPINGS 8U
#define LAGHU_DOMAIN_ORIGIN_SIZE 256U
#define LAGHU_HTML_CACHE_ORIGIN_SIZE LAGHU_DOMAIN_ORIGIN_SIZE
#define LAGHU_HTML_CACHE_TTL_UNSET 0U
#define LAGHU_HTML_CACHE_TTL_MIN 1U
#define LAGHU_HTML_CACHE_TTL_MAX 3600U
#define LAGHU_HTML_CACHE_STALE_TTL_UNSET 0U
#define LAGHU_HTML_CACHE_STALE_TTL_MAX 86400U

typedef enum {
  LAGHU_MODE_UNSET = -1,
  LAGHU_MODE_OFF = 0,
  LAGHU_MODE_ON = 1
} laghu_mode;

typedef enum {
  LAGHU_PRESET_UNSET = -1,
  LAGHU_PRESET_SAFE = 0,
  LAGHU_PRESET_BALANCED,
  LAGHU_PRESET_AGGRESSIVE,
  LAGHU_PRESET_ECOMMERCE,
  LAGHU_PRESET_BLOG,
  LAGHU_PRESET_STATIC
} laghu_preset;

typedef enum {
  LAGHU_REWRITE_LEVEL_UNSET = -1,
  LAGHU_REWRITE_LEVEL_PASSTHROUGH = 0,
  LAGHU_REWRITE_LEVEL_CORE,
  LAGHU_REWRITE_LEVEL_BANDWIDTH,
  LAGHU_REWRITE_LEVEL_ALL,
  LAGHU_REWRITE_LEVEL_EXPERIMENTAL
} laghu_rewrite_level;

typedef enum {
  LAGHU_RISK_CONSERVATIVE = 0,
  LAGHU_RISK_MODERATE,
  LAGHU_RISK_EXPANSIVE
} laghu_risk_level;

typedef enum {
  LAGHU_FILTER_IMAGE_LOSSLESS = UINT32_C(1) << 0,
  LAGHU_FILTER_IMAGE_METADATA = UINT32_C(1) << 1,
  LAGHU_FILTER_IMAGE_DIMENSIONS = UINT32_C(1) << 2,
  LAGHU_FILTER_IMAGE_MODERN = UINT32_C(1) << 3,
  LAGHU_FILTER_IMAGE_RESPONSIVE = UINT32_C(1) << 4,
  LAGHU_FILTER_IMAGE_LAZYLOAD = UINT32_C(1) << 5,
  LAGHU_FILTER_HTML_MINIFY = UINT32_C(1) << 6,
  LAGHU_FILTER_CSS_MINIFY = UINT32_C(1) << 7,
  LAGHU_FILTER_JAVASCRIPT_MINIFY = UINT32_C(1) << 8,
  LAGHU_FILTER_RESOURCE_HINTS = UINT32_C(1) << 9,
  LAGHU_FILTER_CACHE_EXTENSION = UINT32_C(1) << 10,
  LAGHU_FILTER_RESOURCE_COMBINE = UINT32_C(1) << 11,
  LAGHU_FILTER_RESOURCE_INLINE = UINT32_C(1) << 12,
  LAGHU_FILTER_CRITICAL_CSS = UINT32_C(1) << 13,
  LAGHU_FILTER_JAVASCRIPT_DEFER = UINT32_C(1) << 14,
  LAGHU_FILTER_IMMUTABLE_CACHE = UINT32_C(1) << 15,
  LAGHU_FILTER_CACHE_MEDIA = UINT32_C(1) << 16
} laghu_filter_family;

#define LAGHU_FILTER_ALL_MASK                                                \
  ((uint32_t)(LAGHU_FILTER_IMAGE_LOSSLESS | LAGHU_FILTER_IMAGE_METADATA |    \
              LAGHU_FILTER_IMAGE_DIMENSIONS | LAGHU_FILTER_IMAGE_MODERN |    \
              LAGHU_FILTER_IMAGE_RESPONSIVE | LAGHU_FILTER_IMAGE_LAZYLOAD |  \
              LAGHU_FILTER_HTML_MINIFY | LAGHU_FILTER_CSS_MINIFY |           \
              LAGHU_FILTER_JAVASCRIPT_MINIFY | LAGHU_FILTER_RESOURCE_HINTS | \
              LAGHU_FILTER_CACHE_EXTENSION | LAGHU_FILTER_RESOURCE_COMBINE | \
              LAGHU_FILTER_RESOURCE_INLINE | LAGHU_FILTER_CRITICAL_CSS |     \
              LAGHU_FILTER_JAVASCRIPT_DEFER | LAGHU_FILTER_IMMUTABLE_CACHE | \
              LAGHU_FILTER_CACHE_MEDIA))

typedef struct {
  laghu_preset preset;
  laghu_rewrite_level rewrite_level;
  uint32_t filter_families;
  laghu_risk_level risk_level;
  bool allow_lossy;
  bool allow_structural_rewrite;
  bool allow_resource_inlining;
  bool allow_script_reordering;
  bool allow_experimental;
  bool include_js_source_maps;
  unsigned int image_quality;
  unsigned int css_inline_limit;
  unsigned int css_outline_threshold;
  unsigned int javascript_inline_limit;
  unsigned int javascript_outline_threshold;
  char cache_mime_types[LAGHU_MIME_ALLOWLIST_SIZE];
  unsigned char resource_policy_hash[32U];
} laghu_policy;

/* A domain policy is deliberately origin-only. Paths, credentials, queries,
 * and fragments belong to the resource URL and are never configuration. */
typedef struct {
  char source_origin[LAGHU_DOMAIN_ORIGIN_SIZE];
  char public_origin[LAGHU_DOMAIN_ORIGIN_SIZE];
} laghu_domain_mapping;

typedef struct {
  char public_origin[LAGHU_DOMAIN_ORIGIN_SIZE];
  char shards[LAGHU_DOMAIN_POLICY_MAX_SHARDS][LAGHU_DOMAIN_ORIGIN_SIZE];
  unsigned int shard_count;
} laghu_domain_shard_group;

typedef struct {
  char domains[LAGHU_DOMAIN_POLICY_MAX_DOMAINS][LAGHU_DOMAIN_ORIGIN_SIZE];
  unsigned int domain_count;
  laghu_domain_mapping mappings[LAGHU_DOMAIN_POLICY_MAX_MAPPINGS];
  unsigned int mapping_count;
  laghu_domain_shard_group *groups;
  unsigned int group_count;
  unsigned int group_capacity;
} laghu_domain_policy;

typedef struct {
  laghu_mode mode;
  laghu_preset preset;
  laghu_rewrite_level rewrite_level;
  uint32_t enabled_filters;
  uint32_t disabled_filters;
  uint32_t forbidden_filters;
  laghu_mode allow_api;
  laghu_mode image_beacon;
  laghu_mode critical_css_beacon;
  laghu_mode instrumentation_beacon;
  /* Enables RUM-derived, DOM-template-scoped safe variants. */
  laghu_mode optimization_profiles;
  laghu_mode javascript_defer_suggestions;
  laghu_mode include_js_source_maps;
  unsigned int instrumentation_sample_rate;
  unsigned int image_quality;
  unsigned int image_inline_limit;
  unsigned int image_metadata_limit;
  unsigned int image_metadata_ttl;
  unsigned int css_inline_limit;
  unsigned int css_outline_threshold;
  unsigned int javascript_inline_limit;
  unsigned int javascript_outline_threshold;
  unsigned int transform_memory_limit;
  unsigned int transform_deadline_ms;
  unsigned int variants_per_source;
  /* HTML caching remains disabled until both an explicit HTTPS origin and a
   * positive TTL are configured.  Stale TTL zero disables stale serving. */
  char html_cache_origin[LAGHU_HTML_CACHE_ORIGIN_SIZE];
  unsigned int html_cache_ttl;
  unsigned int html_cache_stale_ttl;
  char cache_mime_types[LAGHU_MIME_ALLOWLIST_SIZE];
  laghu_mode respect_vary;
  laghu_mode respect_x_forwarded_proto;
  laghu_mode query_filter_overrides;
  unsigned int allow_resource_count;
  unsigned int disallow_resource_count;
  char allow_resources[LAGHU_RESOURCE_RULE_LIMIT][LAGHU_RESOURCE_PATTERN_SIZE];
  char disallow_resources[LAGHU_RESOURCE_RULE_LIMIT]
                         [LAGHU_RESOURCE_PATTERN_SIZE];
  laghu_domain_policy domain_policy;
} laghu_config;

typedef struct {
  unsigned int status;
  const char *request_path;
  const char *content_type;
  const char *cache_control;
  bool has_authorization;
} laghu_response;

typedef enum {
  LAGHU_DECISION_PASS = 0,
  LAGHU_DECISION_BYPASS_DISABLED,
  LAGHU_DECISION_BYPASS_PASSTHROUGH,
  LAGHU_DECISION_BYPASS_STATUS,
  LAGHU_DECISION_BYPASS_AUTHORIZED,
  LAGHU_DECISION_BYPASS_PRIVATE,
  LAGHU_DECISION_BYPASS_API,
  LAGHU_DECISION_BYPASS_CONTENT_TYPE,
  LAGHU_DECISION_BYPASS_ENCODED,
  LAGHU_DECISION_BYPASS_IMAGE_BACKEND,
  LAGHU_DECISION_BYPASS_RESOURCE_POLICY,
  LAGHU_DECISION_BYPASS_VARY,
  LAGHU_DECISION_BYPASS_QUERY_OVERRIDE,
  LAGHU_DECISION_BYPASS_QUERY_OFF,
  LAGHU_DECISION_BYPASS_QUERY_EXPLAIN,
  LAGHU_DECISION_BYPASS_FORWARDED_PROTO,
  LAGHU_DECISION_IMAGE_HIT,
  LAGHU_DECISION_BYPASS_ERROR
} laghu_decision;

typedef enum {
  LAGHU_QUERY_CONTROL_NONE = 0,
  LAGHU_QUERY_CONTROL_OFF,
  LAGHU_QUERY_CONTROL_EXPLAIN
} laghu_query_control;

typedef enum {
  LAGHU_CANDIDATE_ACCEPTED = 0,
  LAGHU_CANDIDATE_REJECTED_FAILED,
  LAGHU_CANDIDATE_REJECTED_INVALID,
  LAGHU_CANDIDATE_REJECTED_IDENTICAL,
  LAGHU_CANDIDATE_REJECTED_NOT_SMALLER
} laghu_candidate_decision;

typedef struct {
  /* Both fields are borrowed views; this result does not own either buffer. */
  laghu_buffer original;
  laghu_buffer selected;
  laghu_candidate_decision decision;
} laghu_candidate_result;

void laghu_config_init(laghu_config *config);
void laghu_config_merge(laghu_config *result, const laghu_config *parent,
                        const laghu_config *child);

bool laghu_parse_preset(const char *value, laghu_preset *preset);
const char *laghu_preset_name(laghu_preset preset);
bool laghu_resolve_policy(laghu_preset preset, laghu_policy *policy);

bool laghu_parse_rewrite_level(const char *value,
                               laghu_rewrite_level *rewrite_level);
const char *laghu_rewrite_level_name(laghu_rewrite_level rewrite_level);
bool laghu_resolve_rewrite_level(laghu_rewrite_level rewrite_level,
                                 laghu_policy *policy);
bool laghu_resolve_config_policy(const laghu_config *config,
                                 laghu_policy *policy);
bool laghu_parse_filter(const char *value, uint32_t *filter);
const char *laghu_filter_name(uint32_t filter);
bool laghu_mime_type_allowed(const char *allowlist, const char *content_type);
bool laghu_resource_pattern_valid(const char *pattern);
bool laghu_resource_rule_add(laghu_config *config, bool allow,
                             const char *pattern);
bool laghu_resource_rules_merge_valid(const laghu_config *parent,
                                      const laghu_config *child);
bool laghu_resource_allowed(const laghu_config *config, const char *url);
bool laghu_domain_policy_add_domain(laghu_domain_policy *policy,
                                    const char *origin);
bool laghu_domain_policy_add_mapping(laghu_domain_policy *policy,
                                     const char *source_origin,
                                     const char *public_origin);
bool laghu_domain_policy_add_shard(laghu_domain_policy *policy,
                                   const char *public_origin,
                                   const char *origin);
bool laghu_domain_policy_validate(const laghu_domain_policy *policy);
bool laghu_domain_policy_merge_valid(const laghu_domain_policy *parent,
                                     const laghu_domain_policy *child);
/* A pure, allocation-free rewrite. The caller supplies storage for the exact
 * original path, query, fragment, and percent-encoding to be retained. */
bool laghu_domain_url_rewrite(const laghu_domain_policy *policy,
                              const char *source_url, char *output,
                              size_t output_size);
bool laghu_vary_supported(const char *vary);
bool laghu_apply_query_control(const char *query, laghu_query_control *control);
bool laghu_apply_query_filter_overrides(const laghu_config *config,
                                        const char *query, laghu_policy *policy,
                                        uint32_t *enabled, uint32_t *disabled);

laghu_decision laghu_decide(const laghu_config *config,
                            const laghu_response *response);
const char *laghu_decision_name(laghu_decision decision);

laghu_candidate_result laghu_finalize_candidate(laghu_buffer original,
                                                laghu_buffer candidate,
                                                bool candidate_valid);

bool laghu_variant_key(laghu_buffer original, const laghu_policy *policy,
                       char output[LAGHU_SHA256_HEX_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
