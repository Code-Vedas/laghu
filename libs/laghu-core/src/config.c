// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  laghu_config_setting setting;
  const char *canonical;
  const char *apache;
} laghu_config_name;

static const laghu_config_name laghu_config_names[] = {
    {LAGHU_CONFIG_SETTING_PRESET, "preset", "Preset"},
    {LAGHU_CONFIG_SETTING_REWRITE_LEVEL, "rewrite_level", "RewriteLevel"},
    {LAGHU_CONFIG_SETTING_ENABLE_FILTER, "enable_filter", "EnableFilter"},
    {LAGHU_CONFIG_SETTING_DISABLE_FILTER, "disable_filter", "DisableFilter"},
    {LAGHU_CONFIG_SETTING_FORBID_FILTER, "forbid_filter", "ForbidFilter"},
    {LAGHU_CONFIG_SETTING_ALLOW_RESOURCES, "allow_resources", "AllowResources"},
    {LAGHU_CONFIG_SETTING_DISALLOW_RESOURCES, "disallow", "Disallow"},
    {LAGHU_CONFIG_SETTING_RESPECT_VARY, "respect_vary", "RespectVary"},
    {LAGHU_CONFIG_SETTING_RESPECT_FORWARDED_PROTO, "respect_x_forwarded_proto",
     "RespectXForwardedProto"},
    {LAGHU_CONFIG_SETTING_QUERY_FILTER_OVERRIDES, "query_filter_overrides",
     "QueryFilterOverrides"},
    {LAGHU_CONFIG_SETTING_ALLOW_API, "allow_api", "AllowApi"},
    {LAGHU_CONFIG_SETTING_IMAGE_QUALITY, "image_quality", "ImageQuality"},
    {LAGHU_CONFIG_SETTING_IMAGE_BEACON, "image_beacon", "ImageBeacon"},
    {LAGHU_CONFIG_SETTING_CRITICAL_CSS_BEACON, "critical_css_beacon",
     "CriticalCssBeacon"},
    {LAGHU_CONFIG_SETTING_INSTRUMENTATION_BEACON, "instrumentation_beacon",
     "InstrumentationBeacon"},
    {LAGHU_CONFIG_SETTING_INSTRUMENTATION_SAMPLE_RATE,
     "instrumentation_sample_rate", "InstrumentationSampleRate"},
    {LAGHU_CONFIG_SETTING_JAVASCRIPT_DEFER_SUGGESTIONS,
     "javascript_defer_suggestions", "JavaScriptDeferSuggestions"},
    {LAGHU_CONFIG_SETTING_INCLUDE_JS_SOURCE_MAPS, "include_js_source_maps",
     "IncludeJsSourceMaps"},
    {LAGHU_CONFIG_SETTING_IMAGE_INLINE_LIMIT, "image_inline_limit",
     "ImageInlineLimit"},
    {LAGHU_CONFIG_SETTING_IMAGE_METADATA_LIMIT, "image_metadata_limit",
     "ImageMetadataLimit"},
    {LAGHU_CONFIG_SETTING_IMAGE_METADATA_TTL, "image_metadata_ttl",
     "ImageMetadataTtl"},
    {LAGHU_CONFIG_SETTING_CSS_INLINE_LIMIT, "css_inline_limit",
     "CssInlineLimit"},
    {LAGHU_CONFIG_SETTING_CSS_OUTLINE_THRESHOLD, "css_outline_threshold",
     "CssOutlineThreshold"},
    {LAGHU_CONFIG_SETTING_JAVASCRIPT_INLINE_LIMIT, "javascript_inline_limit",
     "JavaScriptInlineLimit"},
    {LAGHU_CONFIG_SETTING_JAVASCRIPT_OUTLINE_THRESHOLD,
     "javascript_outline_threshold", "JavaScriptOutlineThreshold"},
    {LAGHU_CONFIG_SETTING_TRANSFORM_MEMORY_LIMIT, "transform_memory_limit",
     "TransformMemoryLimit"},
    {LAGHU_CONFIG_SETTING_TRANSFORM_DEADLINE_MS, "transform_deadline_ms",
     "TransformDeadlineMs"},
    {LAGHU_CONFIG_SETTING_VARIANTS_PER_SOURCE, "variants_per_source",
     "VariantsPerSource"},
    {LAGHU_CONFIG_SETTING_HTML_CACHE_ORIGIN, "html_cache_origin",
     "HtmlCacheOrigin"},
    {LAGHU_CONFIG_SETTING_HTML_CACHE_TTL, "html_cache_ttl", "HtmlCacheTtl"},
    {LAGHU_CONFIG_SETTING_HTML_CACHE_STALE_TTL, "html_cache_stale_ttl",
     "HtmlCacheStaleTtl"},
    {LAGHU_CONFIG_SETTING_CACHE_MIME_TYPES, "cache_mime_types",
     "CacheMimeTypes"},
    {LAGHU_CONFIG_SETTING_DOMAIN, "domain", "Domain"},
    {LAGHU_CONFIG_SETTING_MAP_REWRITE_DOMAIN, "map_rewrite_domain",
     "MapRewriteDomain"},
    {LAGHU_CONFIG_SETTING_SHARD_DOMAIN, "shard_domain", "ShardDomain"},
    {LAGHU_CONFIG_SETTING_MAP_PROXY_DOMAIN, "map_proxy_domain",
     "MapProxyDomain"}};

static bool laghu_config_name_equal(const char *left, const char *right) {
  if (left[0] == '-' && left[1] == '-') left += 2;
  while (*left != '\0' && *right != '\0') {
    unsigned char left_value = (unsigned char)*left++;
    unsigned char right_value = (unsigned char)*right++;
    if (left_value == '-') left_value = '_';
    if (right_value == '-') right_value = '_';
    if (tolower(left_value) != tolower(right_value)) return false;
  }
  return *left == '\0' && *right == '\0';
}

static bool laghu_config_domain_present(const laghu_domain_policy *policy,
                                        const char *origin) {
  unsigned int index;
  for (index = 0U; index < policy->domain_count; ++index)
    if (strcmp(policy->domains[index], origin) == 0) return true;
  return false;
}

static bool laghu_config_html_cache_origin(const char *value) {
  laghu_domain_policy policy = {0};
  return laghu_domain_policy_add_domain(&policy, value);
}

static bool laghu_config_fail(char *error, size_t size, const char *message) {
  if (error != NULL && size != 0U) (void)snprintf(error, size, "%s", message);
  return false;
}

static bool laghu_config_mode(const char *value, laghu_mode *mode) {
  if (laghu_config_name_equal(value, "on"))
    *mode = LAGHU_MODE_ON;
  else if (laghu_config_name_equal(value, "off"))
    *mode = LAGHU_MODE_OFF;
  else
    return false;
  return true;
}

static bool laghu_config_unsigned(const char *value, uint64_t minimum,
                                  uint64_t maximum, unsigned int *output) {
  uint64_t parsed;
  if (!laghu_base_parse_u64(value, minimum, maximum, &parsed)) return false;
  *output = (unsigned int)parsed;
  return true;
}

static bool laghu_config_size(const char *value, uint64_t minimum,
                              uint64_t maximum, unsigned int *output) {
  char number[32U];
  size_t length = strlen(value), digits = length;
  uint64_t parsed, multiplier = 1U;
  if (length == 0U || length >= sizeof(number)) return false;
  if (!isdigit((unsigned char)value[length - 1U])) {
    char suffix = (char)tolower((unsigned char)value[length - 1U]);
    if (suffix == 'k')
      multiplier = 1024U;
    else if (suffix == 'm')
      multiplier = 1024U * 1024U;
    else if (suffix == 'g')
      multiplier = UINT64_C(1024) * 1024U * 1024U;
    else
      return false;
    digits = length - 1U;
  }
  if (digits == 0U) return false;
  memcpy(number, value, digits);
  number[digits] = '\0';
  if (!laghu_base_parse_u64(number, 0U, maximum / multiplier, &parsed))
    return false;
  parsed *= multiplier;
  if (parsed < minimum || parsed > maximum) return false;
  *output = (unsigned int)parsed;
  return true;
}

laghu_config_setting laghu_config_setting_find(const char *name) {
  size_t index;
  if (name == NULL) return LAGHU_CONFIG_SETTING_UNKNOWN;
  for (index = 0U;
       index < sizeof(laghu_config_names) / sizeof(laghu_config_names[0]);
       ++index)
    if (laghu_config_name_equal(name, laghu_config_names[index].canonical) ||
        laghu_config_name_equal(name, laghu_config_names[index].apache))
      return laghu_config_names[index].setting;
  return LAGHU_CONFIG_SETTING_UNKNOWN;
}

bool laghu_config_setting_apply(laghu_config *config,
                                laghu_config_setting setting, const char *value,
                                char *error, size_t error_size) {
  laghu_mode *mode = NULL;
  unsigned int *number = NULL;
  unsigned int minimum = 0U, maximum = 0U;
  uint32_t filter, declared;
  if (config == NULL || value == NULL)
    return laghu_config_fail(error, error_size, "missing configuration value");
  switch (setting) {
    case LAGHU_CONFIG_SETTING_PRESET:
      if (config->preset != LAGHU_PRESET_UNSET ||
          config->rewrite_level != LAGHU_REWRITE_LEVEL_UNSET ||
          !laghu_parse_preset(value, &config->preset))
        return laghu_config_fail(error, error_size,
                                 "invalid or conflicting preset");
      return true;
    case LAGHU_CONFIG_SETTING_REWRITE_LEVEL:
      if (config->rewrite_level != LAGHU_REWRITE_LEVEL_UNSET ||
          config->preset != LAGHU_PRESET_UNSET ||
          !laghu_parse_rewrite_level(value, &config->rewrite_level) ||
          (config->rewrite_level == LAGHU_REWRITE_LEVEL_PASSTHROUGH &&
           config->enabled_filters != 0U))
        return laghu_config_fail(error, error_size,
                                 "invalid or conflicting rewrite level");
      return true;
    case LAGHU_CONFIG_SETTING_ENABLE_FILTER:
    case LAGHU_CONFIG_SETTING_DISABLE_FILTER:
    case LAGHU_CONFIG_SETTING_FORBID_FILTER:
      declared = config->enabled_filters | config->disabled_filters |
                 config->forbidden_filters;
      if (!laghu_parse_filter(value, &filter) || (declared & filter) != 0U)
        return laghu_config_fail(error, error_size,
                                 "unknown or conflicting filter");
      if (setting == LAGHU_CONFIG_SETTING_ENABLE_FILTER)
        config->enabled_filters |= filter;
      else if (setting == LAGHU_CONFIG_SETTING_DISABLE_FILTER)
        config->disabled_filters |= filter;
      else
        config->forbidden_filters |= filter;
      if (config->rewrite_level == LAGHU_REWRITE_LEVEL_PASSTHROUGH &&
          config->enabled_filters != 0U)
        return laghu_config_fail(error, error_size,
                                 "filter conflicts with passthrough");
      return true;
    case LAGHU_CONFIG_SETTING_ALLOW_RESOURCES:
    case LAGHU_CONFIG_SETTING_DISALLOW_RESOURCES:
      if (!laghu_resource_rule_add(
              config, setting == LAGHU_CONFIG_SETTING_ALLOW_RESOURCES, value))
        return laghu_config_fail(error, error_size,
                                 "invalid or conflicting resource rule");
      return true;
    case LAGHU_CONFIG_SETTING_RESPECT_VARY:
      mode = &config->respect_vary;
      break;
    case LAGHU_CONFIG_SETTING_RESPECT_FORWARDED_PROTO:
      mode = &config->respect_x_forwarded_proto;
      break;
    case LAGHU_CONFIG_SETTING_QUERY_FILTER_OVERRIDES:
      mode = &config->query_filter_overrides;
      break;
    case LAGHU_CONFIG_SETTING_ALLOW_API:
      mode = &config->allow_api;
      break;
    case LAGHU_CONFIG_SETTING_IMAGE_BEACON:
      mode = &config->image_beacon;
      break;
    case LAGHU_CONFIG_SETTING_CRITICAL_CSS_BEACON:
      mode = &config->critical_css_beacon;
      break;
    case LAGHU_CONFIG_SETTING_INSTRUMENTATION_BEACON:
      mode = &config->instrumentation_beacon;
      break;
    case LAGHU_CONFIG_SETTING_JAVASCRIPT_DEFER_SUGGESTIONS:
      mode = &config->javascript_defer_suggestions;
      break;
    case LAGHU_CONFIG_SETTING_INCLUDE_JS_SOURCE_MAPS:
      mode = &config->include_js_source_maps;
      break;
    case LAGHU_CONFIG_SETTING_IMAGE_QUALITY:
      number = &config->image_quality;
      minimum = 1U;
      maximum = 100U;
      break;
    case LAGHU_CONFIG_SETTING_INSTRUMENTATION_SAMPLE_RATE:
      number = &config->instrumentation_sample_rate;
      maximum = 100U;
      break;
    case LAGHU_CONFIG_SETTING_IMAGE_INLINE_LIMIT:
      number = &config->image_inline_limit;
      maximum = 16384U;
      break;
    case LAGHU_CONFIG_SETTING_IMAGE_METADATA_LIMIT:
      number = &config->image_metadata_limit;
      minimum = 1U;
      maximum = 100000U;
      break;
    case LAGHU_CONFIG_SETTING_CSS_INLINE_LIMIT:
      number = &config->css_inline_limit;
      maximum = 65536U;
      break;
    case LAGHU_CONFIG_SETTING_CSS_OUTLINE_THRESHOLD:
      number = &config->css_outline_threshold;
      minimum = 1024U;
      maximum = 1048576U;
      break;
    case LAGHU_CONFIG_SETTING_JAVASCRIPT_INLINE_LIMIT:
      number = &config->javascript_inline_limit;
      maximum = 65536U;
      break;
    case LAGHU_CONFIG_SETTING_JAVASCRIPT_OUTLINE_THRESHOLD:
      number = &config->javascript_outline_threshold;
      minimum = 1024U;
      maximum = 1048576U;
      break;
    case LAGHU_CONFIG_SETTING_TRANSFORM_DEADLINE_MS:
      number = &config->transform_deadline_ms;
      minimum = LAGHU_TRANSFORM_DEADLINE_MS_MIN;
      maximum = LAGHU_TRANSFORM_DEADLINE_MS_MAX;
      break;
    case LAGHU_CONFIG_SETTING_VARIANTS_PER_SOURCE:
      number = &config->variants_per_source;
      minimum = LAGHU_VARIANTS_PER_SOURCE_MIN;
      maximum = LAGHU_VARIANTS_PER_SOURCE_MAX;
      break;
    case LAGHU_CONFIG_SETTING_HTML_CACHE_ORIGIN:
      if (config->html_cache_origin[0] != '\0' ||
          !laghu_config_html_cache_origin(value) ||
          !laghu_base_string_copy(config->html_cache_origin,
                                  sizeof(config->html_cache_origin), value))
        return laghu_config_fail(error, error_size,
                                 "invalid or duplicate HTML cache origin");
      return true;
    case LAGHU_CONFIG_SETTING_HTML_CACHE_TTL:
      number = &config->html_cache_ttl;
      minimum = LAGHU_HTML_CACHE_TTL_MIN;
      maximum = LAGHU_HTML_CACHE_TTL_MAX;
      break;
    case LAGHU_CONFIG_SETTING_HTML_CACHE_STALE_TTL:
      number = &config->html_cache_stale_ttl;
      maximum = LAGHU_HTML_CACHE_STALE_TTL_MAX;
      break;
    case LAGHU_CONFIG_SETTING_TRANSFORM_MEMORY_LIMIT:
      if (config->transform_memory_limit !=
              LAGHU_TRANSFORM_MEMORY_LIMIT_UNSET ||
          !laghu_config_size(value, LAGHU_TRANSFORM_MEMORY_LIMIT_MIN,
                             LAGHU_TRANSFORM_MEMORY_LIMIT_MAX,
                             &config->transform_memory_limit))
        return laghu_config_fail(error, error_size,
                                 "invalid or duplicate memory limit");
      return true;
    case LAGHU_CONFIG_SETTING_IMAGE_METADATA_TTL: {
      char number_value[32U];
      size_t length = strlen(value);
      uint64_t amount, multiplier;
      if (config->image_metadata_ttl != LAGHU_IMAGE_METADATA_TTL_UNSET ||
          length < 2U || length >= sizeof(number_value))
        return laghu_config_fail(error, error_size,
                                 "invalid or duplicate metadata ttl");
      multiplier =
          tolower((unsigned char)value[length - 1U]) == 'h'
              ? 3600U
              : (tolower((unsigned char)value[length - 1U]) == 'd' ? 86400U
                                                                   : 0U);
      memcpy(number_value, value, length - 1U);
      number_value[length - 1U] = '\0';
      if (multiplier == 0U ||
          !laghu_base_parse_u64(number_value, 1U, 2592000U / multiplier,
                                &amount) ||
          amount * multiplier < 3600U)
        return laghu_config_fail(error, error_size,
                                 "metadata ttl must be 1h through 30d");
      config->image_metadata_ttl = (unsigned int)(amount * multiplier);
      return true;
    }
    case LAGHU_CONFIG_SETTING_CACHE_MIME_TYPES:
      if (config->cache_mime_types[0] != '\0' ||
          strlen(value) >= sizeof(config->cache_mime_types))
        return laghu_config_fail(error, error_size,
                                 "invalid or duplicate mime list");
      (void)laghu_base_string_copy(config->cache_mime_types,
                                   sizeof(config->cache_mime_types), value);
      return true;
    case LAGHU_CONFIG_SETTING_DOMAIN:
      if (!laghu_domain_policy_add_domain(&config->domain_policy, value))
        return laghu_config_fail(error, error_size,
                                 "invalid or duplicate domain");
      return true;
    case LAGHU_CONFIG_SETTING_MAP_REWRITE_DOMAIN:
    case LAGHU_CONFIG_SETTING_MAP_PROXY_DOMAIN:
    case LAGHU_CONFIG_SETTING_SHARD_DOMAIN:
      return laghu_config_fail(error, error_size,
                               "domain mapping requires two values");
    default:
      return laghu_config_fail(error, error_size,
                               "unknown configuration setting");
  }
  if (mode != NULL) {
    if (*mode != LAGHU_MODE_UNSET || !laghu_config_mode(value, mode))
      return laghu_config_fail(error, error_size,
                               "invalid or duplicate on/off setting");
    return true;
  }
  if (number != NULL) {
    unsigned int unset =
        number == &config->image_quality ? LAGHU_IMAGE_QUALITY_UNSET
        : number == &config->instrumentation_sample_rate
            ? LAGHU_INSTRUMENTATION_SAMPLE_RATE_UNSET
        : number == &config->image_inline_limit ? LAGHU_IMAGE_INLINE_LIMIT_UNSET
        : number == &config->css_inline_limit   ? LAGHU_CSS_INLINE_LIMIT_UNSET
        : number == &config->javascript_inline_limit
            ? LAGHU_JAVASCRIPT_INLINE_LIMIT_UNSET
            : 0U;
    if (*number != unset ||
        !laghu_config_unsigned(value, minimum, maximum, number))
      return laghu_config_fail(error, error_size,
                               "invalid or duplicate numeric setting");
    return true;
  }
  return false;
}

bool laghu_config_setting_apply_pair(laghu_config *config,
                                     laghu_config_setting setting,
                                     const char *first, const char *second,
                                     char *error, size_t error_size) {
  const char *source;
  const char *public_origin;
  if (config == NULL || first == NULL || second == NULL || first[0] == '\0' ||
      second[0] == '\0')
    return laghu_config_fail(error, error_size, "missing domain mapping");
  if (setting == LAGHU_CONFIG_SETTING_SHARD_DOMAIN) {
    char shards[LAGHU_DOMAIN_ORIGIN_SIZE * LAGHU_DOMAIN_POLICY_MAX_DOMAINS];
    char *cursor;
    bool mapped = false;
    unsigned int mapping;
    for (mapping = 0U; mapping < config->domain_policy.mapping_count; ++mapping)
      if (strcmp(config->domain_policy.mappings[mapping].public_origin,
                 first) == 0) {
        mapped = true;
        break;
      }
    if (!mapped || strlen(second) >= sizeof(shards) ||
        (!laghu_config_domain_present(&config->domain_policy, first) &&
         !laghu_domain_policy_add_domain(&config->domain_policy, first)))
      return laghu_config_fail(error, error_size,
                               "invalid, duplicate, or conflicting shard");
    (void)snprintf(shards, sizeof(shards), "%s", second);
    cursor = shards;
    while (cursor != NULL) {
      char *next = strchr(cursor, ',');
      if (next != NULL) *next++ = '\0';
      if (cursor[0] == '\0' ||
          !laghu_domain_policy_add_shard(&config->domain_policy, first, cursor))
        return laghu_config_fail(error, error_size,
                                 "invalid, duplicate, or conflicting shard");
      cursor = next;
    }
    return true;
  }
  if (setting != LAGHU_CONFIG_SETTING_MAP_REWRITE_DOMAIN &&
      setting != LAGHU_CONFIG_SETTING_MAP_PROXY_DOMAIN)
    return laghu_config_fail(error, error_size, "unknown domain mapping");
  /* Migration syntax is target then source.  The shared policy deliberately
   * stores the operational direction: source URL to public URL. */
  public_origin = first;
  source = second;
  if ((!laghu_config_domain_present(&config->domain_policy, public_origin) &&
       !laghu_domain_policy_add_domain(&config->domain_policy,
                                       public_origin)) ||
      !laghu_domain_policy_add_mapping(&config->domain_policy, source,
                                       public_origin))
    return laghu_config_fail(error, error_size,
                             "invalid, duplicate, or conflicting domain map");
  return true;
}
