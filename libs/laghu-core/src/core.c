// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/core.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define LAGHU_NORMALIZED_URL_SIZE 4096U

#define LAGHU_FILTER_SAFE                                      \
  (LAGHU_FILTER_IMAGE_LOSSLESS | LAGHU_FILTER_IMAGE_METADATA | \
   LAGHU_FILTER_IMAGE_DIMENSIONS)

#define LAGHU_FILTER_BALANCED                                     \
  (LAGHU_FILTER_SAFE | LAGHU_FILTER_IMAGE_MODERN |                \
   LAGHU_FILTER_IMAGE_RESPONSIVE | LAGHU_FILTER_IMAGE_LAZYLOAD |  \
   LAGHU_FILTER_HTML_MINIFY | LAGHU_FILTER_CSS_MINIFY |           \
   LAGHU_FILTER_JAVASCRIPT_MINIFY | LAGHU_FILTER_RESOURCE_HINTS | \
   LAGHU_FILTER_CACHE_EXTENSION)

#define LAGHU_FILTER_AGGRESSIVE                               \
  (LAGHU_FILTER_BALANCED | LAGHU_FILTER_RESOURCE_COMBINE |    \
   LAGHU_FILTER_RESOURCE_INLINE | LAGHU_FILTER_CRITICAL_CSS | \
   LAGHU_FILTER_JAVASCRIPT_DEFER)

#define LAGHU_FILTER_ECOMMERCE                                   \
  (LAGHU_FILTER_SAFE | LAGHU_FILTER_IMAGE_MODERN |               \
   LAGHU_FILTER_IMAGE_RESPONSIVE | LAGHU_FILTER_IMAGE_LAZYLOAD | \
   LAGHU_FILTER_CSS_MINIFY | LAGHU_FILTER_RESOURCE_HINTS |       \
   LAGHU_FILTER_CACHE_EXTENSION)

#define LAGHU_FILTER_BLOG                                 \
  (LAGHU_FILTER_BALANCED | LAGHU_FILTER_RESOURCE_INLINE | \
   LAGHU_FILTER_CRITICAL_CSS | LAGHU_FILTER_JAVASCRIPT_DEFER)

#define LAGHU_FILTER_BANDWIDTH                                 \
  (LAGHU_FILTER_IMAGE_LOSSLESS | LAGHU_FILTER_IMAGE_METADATA | \
   LAGHU_FILTER_IMAGE_MODERN | LAGHU_FILTER_HTML_MINIFY |      \
   LAGHU_FILTER_CSS_MINIFY | LAGHU_FILTER_JAVASCRIPT_MINIFY |  \
   LAGHU_FILTER_CACHE_EXTENSION)

#define LAGHU_FILTER_STATIC \
  (LAGHU_FILTER_AGGRESSIVE | LAGHU_FILTER_IMMUTABLE_CACHE)

#define LAGHU_FILTER_ALL LAGHU_FILTER_ALL_MASK

typedef struct {
  const char *name;
  uint32_t filter;
} laghu_filter_definition;

static const laghu_filter_definition laghu_filters[] = {
    {"image_lossless", LAGHU_FILTER_IMAGE_LOSSLESS},
    {"image_metadata", LAGHU_FILTER_IMAGE_METADATA},
    {"image_dimensions", LAGHU_FILTER_IMAGE_DIMENSIONS},
    {"image_modern", LAGHU_FILTER_IMAGE_MODERN},
    {"image_responsive", LAGHU_FILTER_IMAGE_RESPONSIVE},
    {"image_lazyload", LAGHU_FILTER_IMAGE_LAZYLOAD},
    {"html_minify", LAGHU_FILTER_HTML_MINIFY},
    {"css_minify", LAGHU_FILTER_CSS_MINIFY},
    {"javascript_minify", LAGHU_FILTER_JAVASCRIPT_MINIFY},
    {"resource_hints", LAGHU_FILTER_RESOURCE_HINTS},
    {"cache_extension", LAGHU_FILTER_CACHE_EXTENSION},
    {"resource_combine", LAGHU_FILTER_RESOURCE_COMBINE},
    {"resource_inline", LAGHU_FILTER_RESOURCE_INLINE},
    {"critical_css", LAGHU_FILTER_CRITICAL_CSS},
    {"javascript_defer", LAGHU_FILTER_JAVASCRIPT_DEFER},
    {"immutable_cache", LAGHU_FILTER_IMMUTABLE_CACHE},
    {"cache_media", LAGHU_FILTER_CACHE_MEDIA},
};

static bool laghu_buffer_is_valid(laghu_buffer buffer) {
  return buffer.data != NULL || buffer.length == 0U;
}

static void laghu_policy_init(laghu_policy *policy) {
  policy->preset = LAGHU_PRESET_UNSET;
  policy->rewrite_level = LAGHU_REWRITE_LEVEL_UNSET;
  policy->filter_families = 0U;
  policy->risk_level = LAGHU_RISK_CONSERVATIVE;
  policy->allow_lossy = false;
  policy->allow_structural_rewrite = false;
  policy->allow_resource_inlining = false;
  policy->allow_script_reordering = false;
  policy->allow_experimental = false;
  policy->include_js_source_maps = false;
  policy->image_quality = LAGHU_IMAGE_QUALITY_UNSET;
  policy->css_inline_limit = LAGHU_CSS_INLINE_LIMIT_DEFAULT;
  policy->css_outline_threshold = LAGHU_CSS_OUTLINE_THRESHOLD_DEFAULT;
  policy->javascript_inline_limit = LAGHU_JAVASCRIPT_INLINE_LIMIT_DEFAULT;
  policy->javascript_outline_threshold =
      LAGHU_JAVASCRIPT_OUTLINE_THRESHOLD_DEFAULT;
  policy->cache_mime_types[0] = '\0';
  memset(policy->resource_policy_hash, 0, sizeof(policy->resource_policy_hash));
}

static bool laghu_config_has_policy_selector(const laghu_config *config) {
  return config != NULL && (config->preset != LAGHU_PRESET_UNSET ||
                            config->rewrite_level != LAGHU_REWRITE_LEVEL_UNSET);
}

static bool laghu_starts_with(const char *value, const char *prefix) {
  size_t prefix_length;

  if (value == NULL || prefix == NULL) {
    return false;
  }

  prefix_length = strlen(prefix);
  return strncmp(value, prefix, prefix_length) == 0;
}

static bool laghu_path_segment_matches(const char *path, const char *segment) {
  size_t segment_length;

  if (path == NULL || segment == NULL) {
    return false;
  }

  segment_length = strlen(segment);
  return strncmp(path, segment, segment_length) == 0 &&
         (path[segment_length] == '\0' || path[segment_length] == '/');
}

static bool laghu_is_api_path(const char *path) {
  return laghu_path_segment_matches(path, "/api") ||
         laghu_path_segment_matches(path, "/graphql");
}

static bool laghu_contains_case_insensitive(const char *value,
                                            const char *needle) {
  const char *candidate;
  size_t index;
  size_t needle_length;

  if (value == NULL || needle == NULL) {
    return false;
  }

  needle_length = strlen(needle);
  if (needle_length == 0U) {
    return true;
  }

  for (candidate = value; *candidate != '\0'; ++candidate) {
    for (index = 0U; index < needle_length; ++index) {
      unsigned char left;
      unsigned char right;

      if (candidate[index] == '\0') {
        return false;
      }

      left = (unsigned char)candidate[index];
      right = (unsigned char)needle[index];
      if (tolower(left) != tolower(right)) {
        break;
      }
    }

    if (index == needle_length) {
      return true;
    }
  }

  return false;
}

static bool laghu_supported_content_type(const char *content_type) {
  return laghu_starts_with(content_type, "text/html") ||
         laghu_starts_with(content_type, "text/css") ||
         laghu_starts_with(content_type, "text/javascript") ||
         laghu_starts_with(content_type, "application/javascript") ||
         laghu_starts_with(content_type, "image/") ||
         laghu_starts_with(content_type, "font/") ||
         laghu_starts_with(content_type, "application/font-") ||
         laghu_starts_with(content_type, "application/pdf") ||
         laghu_starts_with(content_type, "audio/") ||
         laghu_starts_with(content_type, "video/");
}

void laghu_config_init(laghu_config *config) {
  if (config == NULL) {
    return;
  }

  config->mode = LAGHU_MODE_UNSET;
  config->preset = LAGHU_PRESET_UNSET;
  config->rewrite_level = LAGHU_REWRITE_LEVEL_UNSET;
  config->enabled_filters = 0U;
  config->disabled_filters = 0U;
  config->forbidden_filters = 0U;
  config->allow_api = LAGHU_MODE_UNSET;
  config->image_beacon = LAGHU_MODE_UNSET;
  config->critical_css_beacon = LAGHU_MODE_UNSET;
  config->instrumentation_beacon = LAGHU_MODE_UNSET;
  config->javascript_defer_suggestions = LAGHU_MODE_UNSET;
  config->include_js_source_maps = LAGHU_MODE_UNSET;
  config->instrumentation_sample_rate = LAGHU_INSTRUMENTATION_SAMPLE_RATE_UNSET;
  config->image_quality = LAGHU_IMAGE_QUALITY_UNSET;
  config->image_inline_limit = LAGHU_IMAGE_INLINE_LIMIT_UNSET;
  config->image_metadata_limit = LAGHU_IMAGE_METADATA_LIMIT_UNSET;
  config->image_metadata_ttl = LAGHU_IMAGE_METADATA_TTL_UNSET;
  config->css_inline_limit = LAGHU_CSS_INLINE_LIMIT_UNSET;
  config->css_outline_threshold = LAGHU_CSS_OUTLINE_THRESHOLD_UNSET;
  config->javascript_inline_limit = LAGHU_JAVASCRIPT_INLINE_LIMIT_UNSET;
  config->javascript_outline_threshold =
      LAGHU_JAVASCRIPT_OUTLINE_THRESHOLD_UNSET;
  config->transform_memory_limit = LAGHU_TRANSFORM_MEMORY_LIMIT_UNSET;
  config->transform_deadline_ms = LAGHU_TRANSFORM_DEADLINE_MS_UNSET;
  config->variants_per_source = LAGHU_VARIANTS_PER_SOURCE_UNSET;
  config->cache_mime_types[0] = '\0';
  config->respect_vary = LAGHU_MODE_UNSET;
  config->respect_x_forwarded_proto = LAGHU_MODE_UNSET;
  config->query_filter_overrides = LAGHU_MODE_UNSET;
  config->allow_resource_count = 0U;
  config->disallow_resource_count = 0U;
  memset(&config->domain_policy, 0, sizeof(config->domain_policy));
}

void laghu_config_merge(laghu_config *result, const laghu_config *parent,
                        const laghu_config *child) {
  laghu_mode parent_mode = LAGHU_MODE_OFF;
  laghu_preset parent_preset = LAGHU_PRESET_BALANCED;
  laghu_rewrite_level parent_rewrite_level = LAGHU_REWRITE_LEVEL_UNSET;
  laghu_mode parent_allow_api = LAGHU_MODE_OFF;
  laghu_mode parent_image_beacon = LAGHU_MODE_OFF;
  laghu_mode parent_critical_css_beacon = LAGHU_MODE_OFF;
  laghu_mode parent_instrumentation_beacon = LAGHU_MODE_OFF;
  laghu_mode parent_javascript_defer_suggestions = LAGHU_MODE_ON;
  laghu_mode parent_include_js_source_maps = LAGHU_MODE_OFF;
  unsigned int parent_instrumentation_sample_rate =
      LAGHU_INSTRUMENTATION_SAMPLE_RATE_DEFAULT;
  unsigned int parent_image_quality = LAGHU_IMAGE_QUALITY_UNSET;
  unsigned int parent_image_inline_limit = LAGHU_IMAGE_INLINE_LIMIT_DEFAULT;
  unsigned int parent_image_metadata_limit = LAGHU_IMAGE_METADATA_LIMIT_DEFAULT;
  unsigned int parent_image_metadata_ttl = LAGHU_IMAGE_METADATA_TTL_DEFAULT;
  unsigned int parent_css_inline_limit = LAGHU_CSS_INLINE_LIMIT_DEFAULT;
  unsigned int parent_css_outline_threshold =
      LAGHU_CSS_OUTLINE_THRESHOLD_DEFAULT;
  unsigned int parent_javascript_inline_limit =
      LAGHU_JAVASCRIPT_INLINE_LIMIT_DEFAULT;
  unsigned int parent_javascript_outline_threshold =
      LAGHU_JAVASCRIPT_OUTLINE_THRESHOLD_DEFAULT;
  unsigned int parent_transform_memory_limit =
      LAGHU_TRANSFORM_MEMORY_LIMIT_DEFAULT;
  unsigned int parent_transform_deadline_ms =
      LAGHU_TRANSFORM_DEADLINE_MS_DEFAULT;
  unsigned int parent_variants_per_source = LAGHU_VARIANTS_PER_SOURCE_DEFAULT;
  laghu_mode parent_respect_vary = LAGHU_MODE_ON;
  laghu_mode parent_respect_x_forwarded_proto = LAGHU_MODE_OFF;
  laghu_mode parent_query_filter_overrides = LAGHU_MODE_OFF;

  if (result == NULL) {
    return;
  }

  if (parent != NULL) {
    if (parent->mode != LAGHU_MODE_UNSET) {
      parent_mode = parent->mode;
    }
    if (laghu_config_has_policy_selector(parent)) {
      parent_preset = parent->preset;
      parent_rewrite_level = parent->rewrite_level;
    }
    if (parent->allow_api != LAGHU_MODE_UNSET) {
      parent_allow_api = parent->allow_api;
    }
    if (parent->image_quality != LAGHU_IMAGE_QUALITY_UNSET) {
      parent_image_quality = parent->image_quality;
    }
    if (parent->image_beacon != LAGHU_MODE_UNSET) {
      parent_image_beacon = parent->image_beacon;
    }
    if (parent->critical_css_beacon != LAGHU_MODE_UNSET) {
      parent_critical_css_beacon = parent->critical_css_beacon;
    }
    if (parent->instrumentation_beacon != LAGHU_MODE_UNSET)
      parent_instrumentation_beacon = parent->instrumentation_beacon;
    if (parent->javascript_defer_suggestions != LAGHU_MODE_UNSET)
      parent_javascript_defer_suggestions =
          parent->javascript_defer_suggestions;
    if (parent->include_js_source_maps != LAGHU_MODE_UNSET)
      parent_include_js_source_maps = parent->include_js_source_maps;
    if (parent->instrumentation_sample_rate !=
        LAGHU_INSTRUMENTATION_SAMPLE_RATE_UNSET)
      parent_instrumentation_sample_rate = parent->instrumentation_sample_rate;
    if (parent->image_inline_limit != LAGHU_IMAGE_INLINE_LIMIT_UNSET) {
      parent_image_inline_limit = parent->image_inline_limit;
    }
    if (parent->image_metadata_limit != LAGHU_IMAGE_METADATA_LIMIT_UNSET) {
      parent_image_metadata_limit = parent->image_metadata_limit;
    }
    if (parent->image_metadata_ttl != LAGHU_IMAGE_METADATA_TTL_UNSET) {
      parent_image_metadata_ttl = parent->image_metadata_ttl;
    }
    if (parent->css_inline_limit != LAGHU_CSS_INLINE_LIMIT_UNSET) {
      parent_css_inline_limit = parent->css_inline_limit;
    }
    if (parent->css_outline_threshold != LAGHU_CSS_OUTLINE_THRESHOLD_UNSET) {
      parent_css_outline_threshold = parent->css_outline_threshold;
    }
    if (parent->javascript_inline_limit !=
        LAGHU_JAVASCRIPT_INLINE_LIMIT_UNSET) {
      parent_javascript_inline_limit = parent->javascript_inline_limit;
    }
    if (parent->javascript_outline_threshold !=
        LAGHU_JAVASCRIPT_OUTLINE_THRESHOLD_UNSET) {
      parent_javascript_outline_threshold =
          parent->javascript_outline_threshold;
    }
    if (parent->transform_memory_limit != LAGHU_TRANSFORM_MEMORY_LIMIT_UNSET)
      parent_transform_memory_limit = parent->transform_memory_limit;
    if (parent->transform_deadline_ms != LAGHU_TRANSFORM_DEADLINE_MS_UNSET)
      parent_transform_deadline_ms = parent->transform_deadline_ms;
    if (parent->variants_per_source != LAGHU_VARIANTS_PER_SOURCE_UNSET)
      parent_variants_per_source = parent->variants_per_source;
    if (parent->respect_vary != LAGHU_MODE_UNSET)
      parent_respect_vary = parent->respect_vary;
    if (parent->respect_x_forwarded_proto != LAGHU_MODE_UNSET)
      parent_respect_x_forwarded_proto = parent->respect_x_forwarded_proto;
    if (parent->query_filter_overrides != LAGHU_MODE_UNSET)
      parent_query_filter_overrides = parent->query_filter_overrides;
  }

  result->mode = child != NULL && child->mode != LAGHU_MODE_UNSET ? child->mode
                                                                  : parent_mode;
  result->enabled_filters = parent != NULL ? parent->enabled_filters : 0U;
  result->disabled_filters = parent != NULL ? parent->disabled_filters : 0U;
  result->forbidden_filters = parent != NULL ? parent->forbidden_filters : 0U;
  if (child != NULL) {
    result->enabled_filters &= ~child->disabled_filters;
    result->disabled_filters &= ~child->enabled_filters;
    result->enabled_filters |= child->enabled_filters;
    result->disabled_filters |= child->disabled_filters;
    result->forbidden_filters |= child->forbidden_filters;
  }
  if (laghu_config_has_policy_selector(child)) {
    result->preset = child->preset;
    result->rewrite_level = child->rewrite_level;
  } else {
    result->preset = parent_preset;
    result->rewrite_level = parent_rewrite_level;
  }
  result->allow_api = child != NULL && child->allow_api != LAGHU_MODE_UNSET
                          ? child->allow_api
                          : parent_allow_api;
  result->image_quality =
      child != NULL && child->image_quality != LAGHU_IMAGE_QUALITY_UNSET
          ? child->image_quality
          : parent_image_quality;
  result->image_beacon =
      child != NULL && child->image_beacon != LAGHU_MODE_UNSET
          ? child->image_beacon
          : parent_image_beacon;
  result->critical_css_beacon =
      child != NULL && child->critical_css_beacon != LAGHU_MODE_UNSET
          ? child->critical_css_beacon
          : parent_critical_css_beacon;
  result->instrumentation_beacon =
      child != NULL && child->instrumentation_beacon != LAGHU_MODE_UNSET
          ? child->instrumentation_beacon
          : parent_instrumentation_beacon;
  result->javascript_defer_suggestions =
      child != NULL && child->javascript_defer_suggestions != LAGHU_MODE_UNSET
          ? child->javascript_defer_suggestions
          : parent_javascript_defer_suggestions;
  result->include_js_source_maps =
      child != NULL && child->include_js_source_maps != LAGHU_MODE_UNSET
          ? child->include_js_source_maps
          : parent_include_js_source_maps;
  result->instrumentation_sample_rate =
      child != NULL && child->instrumentation_sample_rate !=
                           LAGHU_INSTRUMENTATION_SAMPLE_RATE_UNSET
          ? child->instrumentation_sample_rate
          : parent_instrumentation_sample_rate;
  result->image_inline_limit =
      child != NULL &&
              child->image_inline_limit != LAGHU_IMAGE_INLINE_LIMIT_UNSET
          ? child->image_inline_limit
          : parent_image_inline_limit;
  result->image_metadata_limit =
      child != NULL &&
              child->image_metadata_limit != LAGHU_IMAGE_METADATA_LIMIT_UNSET
          ? child->image_metadata_limit
          : parent_image_metadata_limit;
  result->image_metadata_ttl =
      child != NULL &&
              child->image_metadata_ttl != LAGHU_IMAGE_METADATA_TTL_UNSET
          ? child->image_metadata_ttl
          : parent_image_metadata_ttl;
  result->css_inline_limit =
      child != NULL && child->css_inline_limit != LAGHU_CSS_INLINE_LIMIT_UNSET
          ? child->css_inline_limit
          : parent_css_inline_limit;
  result->css_outline_threshold =
      child != NULL &&
              child->css_outline_threshold != LAGHU_CSS_OUTLINE_THRESHOLD_UNSET
          ? child->css_outline_threshold
          : parent_css_outline_threshold;
  result->javascript_inline_limit =
      child != NULL && child->javascript_inline_limit !=
                           LAGHU_JAVASCRIPT_INLINE_LIMIT_UNSET
          ? child->javascript_inline_limit
          : parent_javascript_inline_limit;
  result->javascript_outline_threshold =
      child != NULL && child->javascript_outline_threshold !=
                           LAGHU_JAVASCRIPT_OUTLINE_THRESHOLD_UNSET
          ? child->javascript_outline_threshold
          : parent_javascript_outline_threshold;
  result->transform_memory_limit =
      child != NULL && child->transform_memory_limit !=
                           LAGHU_TRANSFORM_MEMORY_LIMIT_UNSET
          ? child->transform_memory_limit
          : parent_transform_memory_limit;
  result->transform_deadline_ms =
      child != NULL &&
              child->transform_deadline_ms != LAGHU_TRANSFORM_DEADLINE_MS_UNSET
          ? child->transform_deadline_ms
          : parent_transform_deadline_ms;
  result->variants_per_source =
      child != NULL &&
              child->variants_per_source != LAGHU_VARIANTS_PER_SOURCE_UNSET
          ? child->variants_per_source
          : parent_variants_per_source;
  result->respect_vary =
      child != NULL && child->respect_vary != LAGHU_MODE_UNSET
          ? child->respect_vary
          : parent_respect_vary;
  result->respect_x_forwarded_proto =
      child != NULL && child->respect_x_forwarded_proto != LAGHU_MODE_UNSET
          ? child->respect_x_forwarded_proto
          : parent_respect_x_forwarded_proto;
  result->query_filter_overrides =
      child != NULL && child->query_filter_overrides != LAGHU_MODE_UNSET
          ? child->query_filter_overrides
          : parent_query_filter_overrides;
  if (child != NULL && child->cache_mime_types[0] != '\0') {
    (void)snprintf(result->cache_mime_types, sizeof(result->cache_mime_types),
                   "%s", child->cache_mime_types);
  } else if (parent != NULL) {
    (void)snprintf(result->cache_mime_types, sizeof(result->cache_mime_types),
                   "%s", parent->cache_mime_types);
  } else {
    result->cache_mime_types[0] = '\0';
  }
  result->allow_resource_count = 0U;
  result->disallow_resource_count = 0U;
  if (parent != NULL) {
    unsigned int index;
    for (index = 0U; index < parent->allow_resource_count; ++index)
      (void)laghu_resource_rule_add(result, true,
                                    parent->allow_resources[index]);
    for (index = 0U; index < parent->disallow_resource_count; ++index)
      (void)laghu_resource_rule_add(result, false,
                                    parent->disallow_resources[index]);
  }
  if (child != NULL) {
    unsigned int index;
    for (index = 0U; index < child->allow_resource_count; ++index)
      (void)laghu_resource_rule_add(result, true,
                                    child->allow_resources[index]);
    for (index = 0U; index < child->disallow_resource_count; ++index)
      (void)laghu_resource_rule_add(result, false,
                                    child->disallow_resources[index]);
  }
  memset(&result->domain_policy, 0, sizeof(result->domain_policy));
  if (parent != NULL) {
    unsigned int index;
    for (index = 0U; index < parent->domain_policy.domain_count; ++index)
      (void)laghu_domain_policy_add_domain(
          &result->domain_policy, parent->domain_policy.domains[index]);
    for (index = 0U; index < parent->domain_policy.mapping_count; ++index)
      (void)laghu_domain_policy_add_mapping(
          &result->domain_policy,
          parent->domain_policy.mappings[index].source_origin,
          parent->domain_policy.mappings[index].public_origin);
    for (index = 0U; index < parent->domain_policy.group_count; ++index) {
      unsigned int shard;
      for (shard = 0U; shard < parent->domain_policy.groups[index].shard_count;
           ++shard)
        (void)laghu_domain_policy_add_shard(
            &result->domain_policy,
            parent->domain_policy.groups[index].public_origin,
            parent->domain_policy.groups[index].shards[shard]);
    }
  }
  if (child != NULL) {
    unsigned int index;
    for (index = 0U; index < child->domain_policy.domain_count; ++index)
      (void)laghu_domain_policy_add_domain(&result->domain_policy,
                                           child->domain_policy.domains[index]);
    for (index = 0U; index < child->domain_policy.mapping_count; ++index)
      (void)laghu_domain_policy_add_mapping(
          &result->domain_policy,
          child->domain_policy.mappings[index].source_origin,
          child->domain_policy.mappings[index].public_origin);
    for (index = 0U; index < child->domain_policy.group_count; ++index) {
      unsigned int shard;
      for (shard = 0U; shard < child->domain_policy.groups[index].shard_count;
           ++shard)
        (void)laghu_domain_policy_add_shard(
            &result->domain_policy,
            child->domain_policy.groups[index].public_origin,
            child->domain_policy.groups[index].shards[shard]);
    }
  }
}

static bool laghu_glob_match(const char *pattern, const char *value) {
  const char *star = NULL, *retry = NULL;
  while (*value != '\0') {
    if (*pattern == '?' || *pattern == *value) {
      ++pattern;
      ++value;
    } else if (*pattern == '*') {
      star = pattern++;
      retry = value;
    } else if (star != NULL) {
      pattern = star + 1;
      value = ++retry;
    } else {
      return false;
    }
  }
  while (*pattern == '*') ++pattern;
  return *pattern == '\0';
}

bool laghu_resource_pattern_valid(const char *pattern) {
  size_t length;
  if (pattern == NULL || pattern[0] == '\0') return false;
  length = strlen(pattern);
  if (length >= LAGHU_RESOURCE_PATTERN_SIZE || strchr(pattern, '#') != NULL ||
      strchr(pattern, '\r') != NULL || strchr(pattern, '\n') != NULL)
    return false;
  if (pattern[0] == '/') return pattern[1] != '/';
  return laghu_starts_with(pattern, "http://") ||
         laghu_starts_with(pattern, "https://");
}

bool laghu_resource_rule_add(laghu_config *config, bool allow,
                             const char *pattern) {
  unsigned int *count;
  char (*rules)[LAGHU_RESOURCE_PATTERN_SIZE];
  unsigned int index;
  if (config == NULL || !laghu_resource_pattern_valid(pattern)) return false;
  count =
      allow ? &config->allow_resource_count : &config->disallow_resource_count;
  rules = allow ? config->allow_resources : config->disallow_resources;
  if (*count >= LAGHU_RESOURCE_RULE_LIMIT) return false;
  for (index = 0U; index < config->allow_resource_count; ++index)
    if (strcmp(config->allow_resources[index], pattern) == 0) return false;
  for (index = 0U; index < config->disallow_resource_count; ++index)
    if (strcmp(config->disallow_resources[index], pattern) == 0) return false;
  (void)snprintf(rules[*count], LAGHU_RESOURCE_PATTERN_SIZE, "%s", pattern);
  ++*count;
  return true;
}

bool laghu_resource_rules_merge_valid(const laghu_config *parent,
                                      const laghu_config *child) {
  unsigned int left, right;
  if (parent == NULL || child == NULL) return true;
  if (parent->allow_resource_count + child->allow_resource_count >
          LAGHU_RESOURCE_RULE_LIMIT ||
      parent->disallow_resource_count + child->disallow_resource_count >
          LAGHU_RESOURCE_RULE_LIMIT)
    return false;
  for (left = 0U; left < parent->allow_resource_count; ++left) {
    for (right = 0U; right < child->allow_resource_count; ++right)
      if (strcmp(parent->allow_resources[left],
                 child->allow_resources[right]) == 0)
        return false;
    for (right = 0U; right < child->disallow_resource_count; ++right)
      if (strcmp(parent->allow_resources[left],
                 child->disallow_resources[right]) == 0)
        return false;
  }
  for (left = 0U; left < parent->disallow_resource_count; ++left) {
    for (right = 0U; right < child->allow_resource_count; ++right)
      if (strcmp(parent->disallow_resources[left],
                 child->allow_resources[right]) == 0)
        return false;
    for (right = 0U; right < child->disallow_resource_count; ++right)
      if (strcmp(parent->disallow_resources[left],
                 child->disallow_resources[right]) == 0)
        return false;
  }
  return true;
}

bool laghu_resource_allowed(const laghu_config *config, const char *url) {
  const char *target = url;
  const char *scheme;
  const char *path;
  unsigned int index;
  size_t length;
  char normalized[LAGHU_NORMALIZED_URL_SIZE];
  if (config == NULL || url == NULL || url[0] == '\0') return false;
  length = strcspn(url, "#");
  if (length >= sizeof(normalized)) return false;
  memcpy(normalized, url, length);
  normalized[length] = '\0';
  if (laghu_starts_with(normalized, "/.laghu") &&
      (normalized[7] == '\0' || normalized[7] == '/' || normalized[7] == '?'))
    return false;
  scheme = strstr(normalized, "://");
  if (scheme != NULL) {
    const char *authority = scheme + 3;
    bool has_path;
    if (!(laghu_starts_with(normalized, "http://") ||
          laghu_starts_with(normalized, "https://")) ||
        authority[0] == '\0')
      return false;
    path = strchr(authority, '/');
    has_path = path != NULL;
    if (!has_path) path = "/";
    if (strchr(authority, '@') != NULL &&
        (!has_path || strchr(authority, '@') < path))
      return false;
  } else if (normalized[0] == '/') {
    path = normalized;
  } else {
    return false;
  }
  if (laghu_starts_with(path, "/.laghu") &&
      (path[7] == '\0' || path[7] == '/' || path[7] == '?'))
    return false;
  for (index = 0U; index < config->disallow_resource_count; ++index) {
    target = config->disallow_resources[index][0] == '/' ? path : normalized;
    if (laghu_glob_match(config->disallow_resources[index], target))
      return false;
  }
  if (config->allow_resource_count == 0U) return true;
  for (index = 0U; index < config->allow_resource_count; ++index) {
    target = config->allow_resources[index][0] == '/' ? path : normalized;
    if (laghu_glob_match(config->allow_resources[index], target)) return true;
  }
  return false;
}

static bool laghu_domain_origin_valid(const char *origin) {
  const char *authority;
  const char *cursor;
  if (origin == NULL || strncmp(origin, "https://", 8U) != 0 ||
      origin[8] == '\0' || strlen(origin) >= LAGHU_DOMAIN_ORIGIN_SIZE)
    return false;
  authority = origin + 8U;
  if (!isalpha((unsigned char)authority[0])) return false;
  for (cursor = authority; *cursor != '\0'; ++cursor) {
    if (!(islower((unsigned char)*cursor) || isdigit((unsigned char)*cursor) ||
          *cursor == '.' || *cursor == '-' || *cursor == ':'))
      return false;
  }
  return strchr(authority, '.') != NULL && strstr(authority, "..") == NULL &&
         authority[strlen(authority) - 1U] != '.';
}

static bool laghu_domain_contains(
    const char domains[LAGHU_DOMAIN_POLICY_MAX_DOMAINS]
                      [LAGHU_DOMAIN_ORIGIN_SIZE],
    unsigned int count, const char *origin) {
  unsigned int index;
  for (index = 0U; index < count; ++index)
    if (strcmp(domains[index], origin) == 0) return true;
  return false;
}

bool laghu_domain_policy_add_domain(laghu_domain_policy *policy,
                                    const char *origin) {
  if (policy == NULL || !laghu_domain_origin_valid(origin) ||
      policy->domain_count >= LAGHU_DOMAIN_POLICY_MAX_DOMAINS ||
      laghu_domain_contains(policy->domains, policy->domain_count, origin))
    return false;
  (void)snprintf(policy->domains[policy->domain_count++],
                 LAGHU_DOMAIN_ORIGIN_SIZE, "%s", origin);
  return true;
}

bool laghu_domain_policy_add_mapping(laghu_domain_policy *policy,
                                     const char *source_origin,
                                     const char *public_origin) {
  unsigned int index;
  if (policy == NULL || !laghu_domain_origin_valid(source_origin) ||
      !laghu_domain_origin_valid(public_origin) ||
      policy->mapping_count >= LAGHU_DOMAIN_POLICY_MAX_MAPPINGS)
    return false;
  for (index = 0U; index < policy->mapping_count; ++index)
    if (strcmp(policy->mappings[index].source_origin, source_origin) == 0)
      return false;
  (void)snprintf(policy->mappings[policy->mapping_count].source_origin,
                 LAGHU_DOMAIN_ORIGIN_SIZE, "%s", source_origin);
  (void)snprintf(policy->mappings[policy->mapping_count].public_origin,
                 LAGHU_DOMAIN_ORIGIN_SIZE, "%s", public_origin);
  ++policy->mapping_count;
  return true;
}

bool laghu_domain_policy_add_shard(laghu_domain_policy *policy,
                                   const char *public_origin,
                                   const char *origin) {
  laghu_domain_shard_group *group = NULL;
  unsigned int index;
  if (policy == NULL || !laghu_domain_origin_valid(public_origin) ||
      !laghu_domain_origin_valid(origin))
    return false;
  {
    bool mapped = false;
    for (index = 0U; index < policy->mapping_count; ++index)
      if (strcmp(policy->mappings[index].public_origin, public_origin) == 0) {
        mapped = true;
        break;
      }
    if (!mapped) return false;
  }
  for (index = 0U; index < policy->group_count; ++index)
    if (strcmp(policy->groups[index].public_origin, public_origin) == 0) {
      group = &policy->groups[index];
      break;
    }
  if (group == NULL) {
    if (policy->group_count >= LAGHU_DOMAIN_POLICY_MAX_GROUPS) return false;
    group = &policy->groups[policy->group_count++];
    (void)snprintf(group->public_origin, sizeof(group->public_origin), "%s",
                   public_origin);
  }
  if (group->shard_count >= LAGHU_DOMAIN_POLICY_MAX_SHARDS ||
      laghu_domain_contains(group->shards, group->shard_count, origin))
    return false;
  (void)snprintf(group->shards[group->shard_count++], LAGHU_DOMAIN_ORIGIN_SIZE,
                 "%s", origin);
  return true;
}

bool laghu_domain_policy_validate(const laghu_domain_policy *policy) {
  unsigned int index;
  if (policy == NULL ||
      policy->domain_count > LAGHU_DOMAIN_POLICY_MAX_DOMAINS ||
      policy->mapping_count > LAGHU_DOMAIN_POLICY_MAX_MAPPINGS ||
      policy->group_count > LAGHU_DOMAIN_POLICY_MAX_GROUPS)
    return false;
  for (index = 0U; index < policy->domain_count; ++index)
    if (!laghu_domain_origin_valid(policy->domains[index]) ||
        laghu_domain_contains(policy->domains, index, policy->domains[index]))
      return false;
  for (index = 0U; index < policy->mapping_count; ++index) {
    unsigned int prior;
    if (!laghu_domain_origin_valid(policy->mappings[index].source_origin) ||
        !laghu_domain_origin_valid(policy->mappings[index].public_origin) ||
        !laghu_domain_contains(policy->domains, policy->domain_count,
                               policy->mappings[index].public_origin))
      return false;
    for (prior = 0U; prior < index; ++prior)
      if (strcmp(policy->mappings[prior].source_origin,
                 policy->mappings[index].source_origin) == 0)
        return false;
  }
  for (index = 0U; index < policy->group_count; ++index) {
    unsigned int shard, prior;
    const laghu_domain_shard_group *group = &policy->groups[index];
    bool mapped = false;
    if (!laghu_domain_origin_valid(group->public_origin) ||
        group->shard_count == 0U ||
        group->shard_count > LAGHU_DOMAIN_POLICY_MAX_SHARDS ||
        !laghu_domain_contains(policy->domains, policy->domain_count,
                               group->public_origin))
      return false;
    for (prior = 0U; prior < index; ++prior)
      if (strcmp(policy->groups[prior].public_origin, group->public_origin) ==
          0)
        return false;
    for (prior = 0U; prior < policy->mapping_count; ++prior)
      if (strcmp(policy->mappings[prior].public_origin, group->public_origin) ==
          0) {
        mapped = true;
        break;
      }
    if (!mapped) return false;
    for (shard = 0U; shard < group->shard_count; ++shard) {
      if (!laghu_domain_origin_valid(group->shards[shard]) ||
          !laghu_domain_contains(policy->domains, policy->domain_count,
                                 group->shards[shard]) ||
          laghu_domain_contains(group->shards, shard, group->shards[shard]))
        return false;
    }
  }
  return true;
}

bool laghu_domain_policy_merge_valid(const laghu_domain_policy *parent,
                                     const laghu_domain_policy *child) {
  laghu_domain_policy merged;
  unsigned int index;
  if (parent == NULL || child == NULL) return true;
  memset(&merged, 0, sizeof(merged));
  for (index = 0U; index < parent->domain_count; ++index)
    if (!laghu_domain_policy_add_domain(&merged, parent->domains[index]))
      return false;
  for (index = 0U; index < child->domain_count; ++index)
    if (!laghu_domain_policy_add_domain(&merged, child->domains[index]))
      return false;
  for (index = 0U; index < parent->mapping_count; ++index)
    if (!laghu_domain_policy_add_mapping(&merged,
                                         parent->mappings[index].source_origin,
                                         parent->mappings[index].public_origin))
      return false;
  for (index = 0U; index < child->mapping_count; ++index)
    if (!laghu_domain_policy_add_mapping(&merged,
                                         child->mappings[index].source_origin,
                                         child->mappings[index].public_origin))
      return false;
  for (index = 0U; index < parent->group_count; ++index) {
    unsigned int shard;
    for (shard = 0U; shard < parent->groups[index].shard_count; ++shard)
      if (!laghu_domain_policy_add_shard(
              &merged, parent->groups[index].public_origin,
              parent->groups[index].shards[shard]))
        return false;
  }
  for (index = 0U; index < child->group_count; ++index) {
    unsigned int shard;
    for (shard = 0U; shard < child->groups[index].shard_count; ++shard)
      if (!laghu_domain_policy_add_shard(
              &merged, child->groups[index].public_origin,
              child->groups[index].shards[shard]))
        return false;
  }
  return laghu_domain_policy_validate(&merged);
}

bool laghu_domain_url_rewrite(const laghu_domain_policy *policy,
                              const char *source_url, char *output,
                              size_t output_size) {
  const laghu_domain_mapping *mapping = NULL;
  const char *suffix;
  const char *public_origin;
  size_t index;
  size_t hash = 2166136261U;
  int written;
  if (output == NULL || output_size == 0U || source_url == NULL ||
      !laghu_domain_policy_validate(policy))
    return false;
  for (index = 0U; index < policy->mapping_count; ++index) {
    size_t length = strlen(policy->mappings[index].source_origin);
    if (strncmp(source_url, policy->mappings[index].source_origin, length) ==
            0 &&
        (source_url[length] == '\0' || source_url[length] == '/' ||
         source_url[length] == '?' || source_url[length] == '#')) {
      mapping = &policy->mappings[index];
      break;
    }
  }
  if (mapping == NULL) return false;
  suffix = source_url + strlen(mapping->source_origin);
  public_origin = mapping->public_origin;
  for (index = 0U; index < policy->group_count; ++index) {
    const laghu_domain_shard_group *group = &policy->groups[index];
    if (strcmp(group->public_origin, public_origin) != 0) continue;
    const unsigned char *cursor;
    for (cursor = (const unsigned char *)suffix;
         *cursor != '\0' && *cursor != '?' && *cursor != '#'; ++cursor)
      hash = (hash ^ *cursor) * 16777619U;
    public_origin = group->shards[hash % group->shard_count];
    break;
  }
  written = snprintf(output, output_size, "%s%s", public_origin, suffix);
  return written > 0 && (size_t)written < output_size;
}

bool laghu_vary_supported(const char *vary) {
  const char *cursor = vary;
  if (vary == NULL || *vary == '\0') return true;
  while (*cursor != '\0') {
    const char *end;
    size_t length;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') ++cursor;
    end = strchr(cursor, ',');
    length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
    while (length > 0U && isspace((unsigned char)cursor[length - 1U])) --length;
    if (length != 6U || tolower((unsigned char)cursor[0]) != 'a' ||
        tolower((unsigned char)cursor[1]) != 'c' ||
        tolower((unsigned char)cursor[2]) != 'c' ||
        tolower((unsigned char)cursor[3]) != 'e' ||
        tolower((unsigned char)cursor[4]) != 'p' ||
        tolower((unsigned char)cursor[5]) != 't')
      return false;
    if (end == NULL) break;
    cursor = end + 1;
  }
  return true;
}

static int laghu_hex_value(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool laghu_apply_query_filter_overrides(const laghu_config *config,
                                        const char *query, laghu_policy *policy,
                                        uint32_t *enabled, uint32_t *disabled) {
  const char *cursor;
  char decoded[LAGHU_QUERY_OVERRIDE_SIZE];
  size_t used = 0U;
  bool found = false;
  uint32_t enable_mask = 0U, disable_mask = 0U;
  laghu_config effective;
  if (config == NULL || policy == NULL) return false;
  if (enabled != NULL) *enabled = 0U;
  if (disabled != NULL) *disabled = 0U;
  if (config->query_filter_overrides != LAGHU_MODE_ON || query == NULL)
    return laghu_resolve_config_policy(config, policy);
  cursor = query[0] == '?' ? query + 1 : query;
  while (*cursor != '\0') {
    const char *end = strchr(cursor, '&');
    size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
    if (length >= 13U && strncmp(cursor, "laghuFilters=", 13U) == 0) {
      size_t index;
      if (found) return false;
      found = true;
      for (index = 13U; index < length; ++index) {
        unsigned char value = (unsigned char)cursor[index];
        if (value == '%' && index + 2U < length) {
          int high = laghu_hex_value(cursor[index + 1U]);
          int low = laghu_hex_value(cursor[index + 2U]);
          if (high < 0 || low < 0) return false;
          value = (unsigned char)((high << 4) | low);
          index += 2U;
        } else if (value == '%' || value <= 0x20U || value >= 0x7fU) {
          return false;
        }
        if (used + 1U >= sizeof(decoded)) return false;
        decoded[used++] = (char)value;
      }
    }
    if (end == NULL) break;
    cursor = end + 1U;
  }
  if (!found) return laghu_resolve_config_policy(config, policy);
  if (used == 0U) return false;
  decoded[used] = '\0';
  cursor = decoded;
  while (*cursor != '\0') {
    const char *end = strchr(cursor, ',');
    size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
    char name[64U];
    uint32_t filter;
    bool turn_on;
    if (length < 2U || length >= sizeof(name) ||
        (cursor[0] != '+' && cursor[0] != '-'))
      return false;
    turn_on = cursor[0] == '+';
    memcpy(name, cursor + 1U, length - 1U);
    name[length - 1U] = '\0';
    if (!laghu_parse_filter(name, &filter) ||
        ((enable_mask | disable_mask) & filter) != 0U ||
        (turn_on && (config->forbidden_filters & filter) != 0U))
      return false;
    if (turn_on)
      enable_mask |= filter;
    else
      disable_mask |= filter;
    if (end == NULL) break;
    cursor = end + 1U;
  }
  effective = *config;
  effective.enabled_filters &= ~disable_mask;
  effective.disabled_filters &= ~enable_mask;
  effective.enabled_filters |= enable_mask;
  effective.disabled_filters |= disable_mask;
  if (!laghu_resolve_config_policy(&effective, policy)) return false;
  if (enabled != NULL) *enabled = enable_mask;
  if (disabled != NULL) *disabled = disable_mask;
  return true;
}

bool laghu_mime_type_allowed(const char *allowlist, const char *content_type) {
  const char *cursor;
  size_t type_length;
  if (allowlist == NULL || content_type == NULL || content_type[0] == '\0')
    return false;
  type_length = strcspn(content_type, "; \t\r\n");
  if (type_length == 0U || type_length >= 256U) return false;
  cursor = allowlist;
  while (*cursor != '\0') {
    const char *end = strchr(cursor, ',');
    size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
    while (length > 0U && isspace((unsigned char)cursor[0])) {
      ++cursor;
      --length;
    }
    while (length > 0U && isspace((unsigned char)cursor[length - 1U])) --length;
    if (length == type_length) {
      size_t index;
      for (index = 0U; index < type_length; ++index) {
        if (tolower((unsigned char)cursor[index]) !=
            tolower((unsigned char)content_type[index]))
          break;
      }
      if (index == type_length) return true;
    }
    if (end == NULL) break;
    cursor = end + 1U;
  }
  return false;
}

bool laghu_parse_preset(const char *value, laghu_preset *preset) {
  static const struct {
    const char *name;
    laghu_preset value;
  } presets[] = {
      {"safe", LAGHU_PRESET_SAFE},
      {"balanced", LAGHU_PRESET_BALANCED},
      {"aggressive", LAGHU_PRESET_AGGRESSIVE},
      {"ecommerce", LAGHU_PRESET_ECOMMERCE},
      {"blog", LAGHU_PRESET_BLOG},
      {"static", LAGHU_PRESET_STATIC},
  };
  size_t index;

  if (value == NULL || preset == NULL) {
    return false;
  }

  for (index = 0U; index < sizeof(presets) / sizeof(presets[0]); ++index) {
    if (strcmp(value, presets[index].name) == 0) {
      *preset = presets[index].value;
      return true;
    }
  }

  return false;
}

const char *laghu_preset_name(laghu_preset preset) {
  switch (preset) {
    case LAGHU_PRESET_SAFE:
      return "safe";
    case LAGHU_PRESET_BALANCED:
      return "balanced";
    case LAGHU_PRESET_AGGRESSIVE:
      return "aggressive";
    case LAGHU_PRESET_ECOMMERCE:
      return "ecommerce";
    case LAGHU_PRESET_BLOG:
      return "blog";
    case LAGHU_PRESET_STATIC:
      return "static";
    case LAGHU_PRESET_UNSET:
    default:
      return "unset";
  }
}

bool laghu_resolve_policy(laghu_preset preset, laghu_policy *policy) {
  if (policy == NULL) {
    return false;
  }

  laghu_policy_init(policy);
  policy->preset = preset;

  switch (preset) {
    case LAGHU_PRESET_SAFE:
      policy->filter_families = LAGHU_FILTER_SAFE;
      policy->risk_level = LAGHU_RISK_CONSERVATIVE;
      policy->image_quality = LAGHU_IMAGE_QUALITY_UNSET;
      return true;
    case LAGHU_PRESET_BALANCED:
      policy->filter_families = LAGHU_FILTER_BALANCED;
      policy->risk_level = LAGHU_RISK_MODERATE;
      policy->allow_lossy = true;
      policy->allow_structural_rewrite = true;
      policy->image_quality = 82U;
      return true;
    case LAGHU_PRESET_AGGRESSIVE:
      policy->filter_families = LAGHU_FILTER_AGGRESSIVE;
      policy->risk_level = LAGHU_RISK_EXPANSIVE;
      policy->allow_lossy = true;
      policy->allow_structural_rewrite = true;
      policy->allow_resource_inlining = true;
      policy->allow_script_reordering = true;
      policy->image_quality = 75U;
      return true;
    case LAGHU_PRESET_ECOMMERCE:
      policy->filter_families = LAGHU_FILTER_ECOMMERCE;
      policy->risk_level = LAGHU_RISK_CONSERVATIVE;
      policy->allow_lossy = true;
      policy->allow_structural_rewrite = true;
      policy->image_quality = 85U;
      return true;
    case LAGHU_PRESET_BLOG:
      policy->filter_families = LAGHU_FILTER_BLOG;
      policy->risk_level = LAGHU_RISK_MODERATE;
      policy->allow_lossy = true;
      policy->allow_structural_rewrite = true;
      policy->allow_resource_inlining = true;
      policy->allow_script_reordering = true;
      policy->image_quality = 82U;
      return true;
    case LAGHU_PRESET_STATIC:
      policy->filter_families = LAGHU_FILTER_STATIC;
      policy->risk_level = LAGHU_RISK_EXPANSIVE;
      policy->allow_lossy = true;
      policy->allow_structural_rewrite = true;
      policy->allow_resource_inlining = true;
      policy->allow_script_reordering = true;
      policy->image_quality = 75U;
      return true;
    case LAGHU_PRESET_UNSET:
    default:
      return false;
  }
}

bool laghu_parse_rewrite_level(const char *value,
                               laghu_rewrite_level *rewrite_level) {
  static const struct {
    const char *name;
    laghu_rewrite_level value;
  } rewrite_levels[] = {
      {"passthrough", LAGHU_REWRITE_LEVEL_PASSTHROUGH},
      {"core", LAGHU_REWRITE_LEVEL_CORE},
      {"bandwidth", LAGHU_REWRITE_LEVEL_BANDWIDTH},
      {"all", LAGHU_REWRITE_LEVEL_ALL},
      {"experimental", LAGHU_REWRITE_LEVEL_EXPERIMENTAL},
  };
  size_t index;

  if (value == NULL || rewrite_level == NULL) {
    return false;
  }

  for (index = 0U; index < sizeof(rewrite_levels) / sizeof(rewrite_levels[0]);
       ++index) {
    if (strcmp(value, rewrite_levels[index].name) == 0) {
      *rewrite_level = rewrite_levels[index].value;
      return true;
    }
  }

  return false;
}

const char *laghu_rewrite_level_name(laghu_rewrite_level rewrite_level) {
  switch (rewrite_level) {
    case LAGHU_REWRITE_LEVEL_PASSTHROUGH:
      return "passthrough";
    case LAGHU_REWRITE_LEVEL_CORE:
      return "core";
    case LAGHU_REWRITE_LEVEL_BANDWIDTH:
      return "bandwidth";
    case LAGHU_REWRITE_LEVEL_ALL:
      return "all";
    case LAGHU_REWRITE_LEVEL_EXPERIMENTAL:
      return "experimental";
    case LAGHU_REWRITE_LEVEL_UNSET:
    default:
      return "unset";
  }
}

bool laghu_parse_filter(const char *value, uint32_t *filter) {
  size_t index;

  if (value == NULL || filter == NULL) return false;
  for (index = 0U; index < sizeof(laghu_filters) / sizeof(laghu_filters[0]);
       ++index) {
    if (strcmp(value, laghu_filters[index].name) == 0) {
      *filter = laghu_filters[index].filter;
      return true;
    }
  }
  return false;
}

const char *laghu_filter_name(uint32_t filter) {
  size_t index;

  for (index = 0U; index < sizeof(laghu_filters) / sizeof(laghu_filters[0]);
       ++index) {
    if (filter == laghu_filters[index].filter) return laghu_filters[index].name;
  }
  return NULL;
}

bool laghu_resolve_rewrite_level(laghu_rewrite_level rewrite_level,
                                 laghu_policy *policy) {
  if (policy == NULL) {
    return false;
  }

  laghu_policy_init(policy);
  policy->rewrite_level = rewrite_level;

  switch (rewrite_level) {
    case LAGHU_REWRITE_LEVEL_PASSTHROUGH:
      return true;
    case LAGHU_REWRITE_LEVEL_CORE:
      if (!laghu_resolve_policy(LAGHU_PRESET_BALANCED, policy)) {
        return false;
      }
      policy->preset = LAGHU_PRESET_UNSET;
      policy->rewrite_level = rewrite_level;
      return true;
    case LAGHU_REWRITE_LEVEL_BANDWIDTH:
      policy->filter_families = LAGHU_FILTER_BANDWIDTH;
      policy->risk_level = LAGHU_RISK_MODERATE;
      policy->allow_lossy = true;
      policy->image_quality = 82U;
      return true;
    case LAGHU_REWRITE_LEVEL_ALL:
    case LAGHU_REWRITE_LEVEL_EXPERIMENTAL:
      policy->filter_families = LAGHU_FILTER_ALL;
      policy->risk_level = LAGHU_RISK_EXPANSIVE;
      policy->allow_lossy = true;
      policy->allow_structural_rewrite = true;
      policy->allow_resource_inlining = true;
      policy->allow_script_reordering = true;
      policy->allow_experimental =
          rewrite_level == LAGHU_REWRITE_LEVEL_EXPERIMENTAL;
      policy->image_quality = 75U;
      return true;
    case LAGHU_REWRITE_LEVEL_UNSET:
    default:
      return false;
  }
}

bool laghu_resolve_config_policy(const laghu_config *config,
                                 laghu_policy *policy) {
  bool has_preset;
  bool has_rewrite_level;
  bool resolved;
  laghu_sha256_context resource_context;
  unsigned int index;

  if (config == NULL || policy == NULL) {
    return false;
  }
  if (!laghu_domain_policy_validate(&config->domain_policy)) return false;
  if (((config->enabled_filters | config->disabled_filters |
        config->forbidden_filters) &
       ~LAGHU_FILTER_ALL_MASK) != 0U ||
      (config->enabled_filters & config->disabled_filters) != 0U ||
      (config->enabled_filters & config->forbidden_filters) != 0U) {
    return false;
  }
  if (config->instrumentation_sample_rate !=
          LAGHU_INSTRUMENTATION_SAMPLE_RATE_UNSET &&
      config->instrumentation_sample_rate > 100U)
    return false;
  if ((config->transform_memory_limit != LAGHU_TRANSFORM_MEMORY_LIMIT_UNSET &&
       (config->transform_memory_limit < LAGHU_TRANSFORM_MEMORY_LIMIT_MIN ||
        config->transform_memory_limit > LAGHU_TRANSFORM_MEMORY_LIMIT_MAX)) ||
      (config->transform_deadline_ms != LAGHU_TRANSFORM_DEADLINE_MS_UNSET &&
       (config->transform_deadline_ms < LAGHU_TRANSFORM_DEADLINE_MS_MIN ||
        config->transform_deadline_ms > LAGHU_TRANSFORM_DEADLINE_MS_MAX)) ||
      (config->variants_per_source != LAGHU_VARIANTS_PER_SOURCE_UNSET &&
       (config->variants_per_source < LAGHU_VARIANTS_PER_SOURCE_MIN ||
        config->variants_per_source > LAGHU_VARIANTS_PER_SOURCE_MAX)))
    return false;
  if (!laghu_domain_policy_validate(&config->domain_policy)) return false;

  has_preset = config->preset != LAGHU_PRESET_UNSET;
  has_rewrite_level = config->rewrite_level != LAGHU_REWRITE_LEVEL_UNSET;
  if (has_preset == has_rewrite_level) {
    return false;
  }

  resolved = has_preset
                 ? laghu_resolve_policy(config->preset, policy)
                 : laghu_resolve_rewrite_level(config->rewrite_level, policy);
  if (resolved && config->rewrite_level == LAGHU_REWRITE_LEVEL_PASSTHROUGH &&
      config->enabled_filters != 0U) {
    return false;
  }
  if (resolved) {
    policy->filter_families |= config->enabled_filters;
    policy->filter_families &=
        ~(config->disabled_filters | config->forbidden_filters);
    if ((config->enabled_filters &
         (LAGHU_FILTER_IMAGE_MODERN | LAGHU_FILTER_IMAGE_RESPONSIVE)) != 0U) {
      policy->allow_lossy = true;
      if (policy->image_quality == LAGHU_IMAGE_QUALITY_UNSET)
        policy->image_quality = 82U;
    }
    if ((config->enabled_filters &
         (LAGHU_FILTER_HTML_MINIFY | LAGHU_FILTER_CSS_MINIFY |
          LAGHU_FILTER_RESOURCE_HINTS | LAGHU_FILTER_RESOURCE_COMBINE |
          LAGHU_FILTER_RESOURCE_INLINE | LAGHU_FILTER_CRITICAL_CSS |
          LAGHU_FILTER_JAVASCRIPT_DEFER)) != 0U)
      policy->allow_structural_rewrite = true;
    if ((config->enabled_filters &
         (LAGHU_FILTER_RESOURCE_INLINE | LAGHU_FILTER_CRITICAL_CSS)) != 0U)
      policy->allow_resource_inlining = true;
    if ((config->enabled_filters & LAGHU_FILTER_JAVASCRIPT_DEFER) != 0U)
      policy->allow_script_reordering = true;
    if ((config->enabled_filters &
         (LAGHU_FILTER_RESOURCE_COMBINE | LAGHU_FILTER_RESOURCE_INLINE |
          LAGHU_FILTER_CRITICAL_CSS | LAGHU_FILTER_JAVASCRIPT_DEFER)) != 0U)
      policy->risk_level = LAGHU_RISK_EXPANSIVE;
    else if (config->enabled_filters != 0U &&
             policy->risk_level < LAGHU_RISK_MODERATE)
      policy->risk_level = LAGHU_RISK_MODERATE;
  }
  if (resolved && config->image_quality != LAGHU_IMAGE_QUALITY_UNSET) {
    if (config->image_quality > 100U) {
      return false;
    }
    if (policy->allow_lossy) {
      policy->image_quality = config->image_quality;
    }
  }
  if (resolved) {
    policy->include_js_source_maps =
        config->include_js_source_maps == LAGHU_MODE_ON;
    policy->css_inline_limit =
        config->css_inline_limit == LAGHU_CSS_INLINE_LIMIT_UNSET
            ? LAGHU_CSS_INLINE_LIMIT_DEFAULT
            : config->css_inline_limit;
    policy->css_outline_threshold =
        config->css_outline_threshold == LAGHU_CSS_OUTLINE_THRESHOLD_UNSET
            ? LAGHU_CSS_OUTLINE_THRESHOLD_DEFAULT
            : config->css_outline_threshold;
    policy->javascript_inline_limit =
        config->javascript_inline_limit == LAGHU_JAVASCRIPT_INLINE_LIMIT_UNSET
            ? LAGHU_JAVASCRIPT_INLINE_LIMIT_DEFAULT
            : config->javascript_inline_limit;
    policy->javascript_outline_threshold =
        config->javascript_outline_threshold ==
                LAGHU_JAVASCRIPT_OUTLINE_THRESHOLD_UNSET
            ? LAGHU_JAVASCRIPT_OUTLINE_THRESHOLD_DEFAULT
            : config->javascript_outline_threshold;
    (void)snprintf(policy->cache_mime_types, sizeof(policy->cache_mime_types),
                   "%s", config->cache_mime_types);
    laghu_sha256_init(&resource_context);
    for (index = 0U; index < config->allow_resource_count; ++index) {
      static const unsigned char allow_marker = 'A';
      laghu_sha256_update(&resource_context, &allow_marker, 1U);
      laghu_sha256_update(&resource_context,
                          (const unsigned char *)config->allow_resources[index],
                          strlen(config->allow_resources[index]) + 1U);
    }
    for (index = 0U; index < config->disallow_resource_count; ++index) {
      static const unsigned char deny_marker = 'D';
      laghu_sha256_update(&resource_context, &deny_marker, 1U);
      laghu_sha256_update(
          &resource_context,
          (const unsigned char *)config->disallow_resources[index],
          strlen(config->disallow_resources[index]) + 1U);
    }
    for (index = 0U; index < config->domain_policy.domain_count; ++index) {
      static const unsigned char domain_marker = 'D';
      laghu_sha256_update(&resource_context, &domain_marker, 1U);
      laghu_sha256_update(
          &resource_context,
          (const unsigned char *)config->domain_policy.domains[index],
          strlen(config->domain_policy.domains[index]) + 1U);
    }
    for (index = 0U; index < config->domain_policy.mapping_count; ++index) {
      static const unsigned char mapping_marker = 'M';
      laghu_sha256_update(&resource_context, &mapping_marker, 1U);
      laghu_sha256_update(
          &resource_context,
          (const unsigned char *)config->domain_policy.mappings[index]
              .source_origin,
          strlen(config->domain_policy.mappings[index].source_origin) + 1U);
      laghu_sha256_update(
          &resource_context,
          (const unsigned char *)config->domain_policy.mappings[index]
              .public_origin,
          strlen(config->domain_policy.mappings[index].public_origin) + 1U);
    }
    for (index = 0U; index < config->domain_policy.group_count; ++index) {
      static const unsigned char group_marker = 'G';
      unsigned int shard;
      laghu_sha256_update(&resource_context, &group_marker, 1U);
      laghu_sha256_update(
          &resource_context,
          (const unsigned char *)config->domain_policy.groups[index]
              .public_origin,
          strlen(config->domain_policy.groups[index].public_origin) + 1U);
      for (shard = 0U;
           shard < config->domain_policy.groups[index].shard_count; ++shard) {
        static const unsigned char shard_marker = 'S';
        laghu_sha256_update(&resource_context, &shard_marker, 1U);
        laghu_sha256_update(
            &resource_context,
            (const unsigned char *)config->domain_policy.groups[index]
                .shards[shard],
            strlen(config->domain_policy.groups[index].shards[shard]) + 1U);
      }
    }
    laghu_sha256_final(&resource_context, policy->resource_policy_hash);
  }
  return resolved;
}

laghu_decision laghu_decide(const laghu_config *config,
                            const laghu_response *response) {
  laghu_policy policy;

  if (config == NULL || config->mode != LAGHU_MODE_ON) {
    return LAGHU_DECISION_BYPASS_DISABLED;
  }

  if (!laghu_resolve_config_policy(config, &policy)) {
    return LAGHU_DECISION_BYPASS_ERROR;
  }

  if (policy.rewrite_level == LAGHU_REWRITE_LEVEL_PASSTHROUGH) {
    return LAGHU_DECISION_BYPASS_PASSTHROUGH;
  }

  if (response == NULL) {
    return LAGHU_DECISION_BYPASS_ERROR;
  }

  if (response->status != 200U) {
    return LAGHU_DECISION_BYPASS_STATUS;
  }

  if (response->has_authorization) {
    return LAGHU_DECISION_BYPASS_AUTHORIZED;
  }

  if (laghu_contains_case_insensitive(response->cache_control, "no-store") ||
      laghu_contains_case_insensitive(response->cache_control, "private")) {
    return LAGHU_DECISION_BYPASS_PRIVATE;
  }

  if (config->allow_api != LAGHU_MODE_ON &&
      laghu_is_api_path(response->request_path)) {
    return LAGHU_DECISION_BYPASS_API;
  }

  if (!laghu_supported_content_type(response->content_type)) {
    return LAGHU_DECISION_BYPASS_CONTENT_TYPE;
  }

  return LAGHU_DECISION_PASS;
}

const char *laghu_decision_name(laghu_decision decision) {
  switch (decision) {
    case LAGHU_DECISION_PASS:
      return "pass";
    case LAGHU_DECISION_BYPASS_DISABLED:
      return "bypass-disabled";
    case LAGHU_DECISION_BYPASS_PASSTHROUGH:
      return "bypass-passthrough";
    case LAGHU_DECISION_BYPASS_STATUS:
      return "bypass-status";
    case LAGHU_DECISION_BYPASS_AUTHORIZED:
      return "bypass-authorized";
    case LAGHU_DECISION_BYPASS_PRIVATE:
      return "bypass-private";
    case LAGHU_DECISION_BYPASS_API:
      return "bypass-api";
    case LAGHU_DECISION_BYPASS_CONTENT_TYPE:
      return "bypass-content-type";
    case LAGHU_DECISION_BYPASS_ENCODED:
      return "bypass-encoded";
    case LAGHU_DECISION_BYPASS_IMAGE_BACKEND:
      return "bypass-image-backend";
    case LAGHU_DECISION_BYPASS_RESOURCE_POLICY:
      return "bypass-resource-policy";
    case LAGHU_DECISION_BYPASS_VARY:
      return "bypass-vary";
    case LAGHU_DECISION_BYPASS_QUERY_OVERRIDE:
      return "bypass-query-override";
    case LAGHU_DECISION_BYPASS_FORWARDED_PROTO:
      return "bypass-forwarded-proto";
    case LAGHU_DECISION_IMAGE_HIT:
      return "image-hit";
    case LAGHU_DECISION_BYPASS_ERROR:
    default:
      return "bypass-error";
  }
}

laghu_candidate_result laghu_finalize_candidate(laghu_buffer original,
                                                laghu_buffer candidate,
                                                bool candidate_valid) {
  laghu_candidate_result result = {
      .original = original,
      .selected = original,
      .decision = LAGHU_CANDIDATE_REJECTED_FAILED,
  };

  if (!candidate_valid) {
    return result;
  }

  if (!laghu_buffer_is_valid(original) || !laghu_buffer_is_valid(candidate)) {
    result.decision = LAGHU_CANDIDATE_REJECTED_INVALID;
    return result;
  }

  if (candidate.length == original.length &&
      (candidate.length == 0U ||
       memcmp(candidate.data, original.data, candidate.length) == 0)) {
    result.decision = LAGHU_CANDIDATE_REJECTED_IDENTICAL;
    return result;
  }

  if (candidate.length >= original.length) {
    result.decision = LAGHU_CANDIDATE_REJECTED_NOT_SMALLER;
    return result;
  }

  result.selected = candidate;
  result.decision = LAGHU_CANDIDATE_ACCEPTED;
  return result;
}

bool laghu_variant_key(laghu_buffer original, const laghu_policy *policy,
                       char output[LAGHU_SHA256_HEX_SIZE]) {
  static const unsigned char namespace_value[] = "laghu-variant";
  laghu_sha256_context context;
  unsigned char fields[31];
  bool has_preset;
  bool has_rewrite_level;
  bool preset_is_valid;
  bool rewrite_level_is_valid;

  if (output == NULL) {
    return false;
  }
  if (!laghu_buffer_is_valid(original) || policy == NULL) {
    output[0] = '\0';
    return false;
  }

  has_preset = policy->preset >= LAGHU_PRESET_SAFE &&
               policy->preset <= LAGHU_PRESET_STATIC;
  has_rewrite_level =
      policy->rewrite_level >= LAGHU_REWRITE_LEVEL_PASSTHROUGH &&
      policy->rewrite_level <= LAGHU_REWRITE_LEVEL_EXPERIMENTAL;
  preset_is_valid = policy->preset == LAGHU_PRESET_UNSET || has_preset;
  rewrite_level_is_valid =
      policy->rewrite_level == LAGHU_REWRITE_LEVEL_UNSET || has_rewrite_level;
  if (!preset_is_valid || !rewrite_level_is_valid ||
      has_preset == has_rewrite_level ||
      policy->risk_level < LAGHU_RISK_CONSERVATIVE ||
      policy->risk_level > LAGHU_RISK_EXPANSIVE ||
      policy->image_quality > 100U || policy->css_inline_limit > 65536U ||
      policy->css_outline_threshold < 1024U ||
      policy->css_outline_threshold > 1048576U ||
      policy->javascript_inline_limit > 65536U ||
      policy->javascript_outline_threshold < 1024U ||
      policy->javascript_outline_threshold > 1048576U ||
      (policy->filter_families & ~((uint32_t)LAGHU_FILTER_ALL)) != 0U) {
    output[0] = '\0';
    return false;
  }

  fields[0] = (unsigned char)LAGHU_VARIANT_KEY_VERSION;
  fields[1] = has_preset ? (unsigned char)(policy->preset + 1) : 0U;
  fields[2] =
      has_rewrite_level ? (unsigned char)(policy->rewrite_level + 1) : 0U;
  fields[3] = (unsigned char)(policy->filter_families >> 24U);
  fields[4] = (unsigned char)(policy->filter_families >> 16U);
  fields[5] = (unsigned char)(policy->filter_families >> 8U);
  fields[6] = (unsigned char)policy->filter_families;
  fields[7] = (unsigned char)policy->risk_level;
  fields[8] = policy->allow_lossy ? 1U : 0U;
  fields[9] = policy->allow_structural_rewrite ? 1U : 0U;
  fields[10] = policy->allow_resource_inlining ? 1U : 0U;
  fields[11] = policy->allow_script_reordering ? 1U : 0U;
  fields[12] = policy->allow_experimental ? 1U : 0U;
  fields[13] = (unsigned char)policy->image_quality;
  fields[14] = (unsigned char)(policy->css_inline_limit >> 24U);
  fields[15] = (unsigned char)(policy->css_inline_limit >> 16U);
  fields[16] = (unsigned char)(policy->css_inline_limit >> 8U);
  fields[17] = (unsigned char)policy->css_inline_limit;
  fields[18] = (unsigned char)(policy->css_outline_threshold >> 24U);
  fields[19] = (unsigned char)(policy->css_outline_threshold >> 16U);
  fields[20] = (unsigned char)(policy->css_outline_threshold >> 8U);
  fields[21] = (unsigned char)policy->css_outline_threshold;
  fields[22] = (unsigned char)(policy->javascript_inline_limit >> 24U);
  fields[23] = (unsigned char)(policy->javascript_inline_limit >> 16U);
  fields[24] = (unsigned char)(policy->javascript_inline_limit >> 8U);
  fields[25] = (unsigned char)policy->javascript_inline_limit;
  fields[26] = (unsigned char)(policy->javascript_outline_threshold >> 24U);
  fields[27] = (unsigned char)(policy->javascript_outline_threshold >> 16U);
  fields[28] = (unsigned char)(policy->javascript_outline_threshold >> 8U);
  fields[29] = (unsigned char)policy->javascript_outline_threshold;
  fields[30] = policy->include_js_source_maps ? 1U : 0U;

  laghu_sha256_init(&context);
  laghu_sha256_update(&context, namespace_value, sizeof(namespace_value) - 1U);
  laghu_sha256_update(&context, fields, sizeof(fields));
  laghu_sha256_update(&context, policy->resource_policy_hash,
                      sizeof(policy->resource_policy_hash));
  laghu_sha256_update(&context, original.data, original.length);
  laghu_sha256_final_hex(&context, output);
  return true;
}
