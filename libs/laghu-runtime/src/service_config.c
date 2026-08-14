// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/service_config.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/base.h"

#define LAGHU_SERVICE_BIT(setting) (UINT64_C(1) << (unsigned int)(setting))
#define LAGHU_SERVICE_IPV6_TEXT_SIZE 46U

typedef struct {
  laghu_service_setting_descriptor descriptor;
  const char *aliases[3];
} laghu_service_descriptor_entry;

static const laghu_service_descriptor_entry laghu_service_descriptors[] = {
    {{LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND, "file_cache_backend", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"filecachebackend", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_IMAGE_CACHE, "image_cache", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U, LAGHU_RUNTIME_PATH_SIZE - 1U,
      false},
     {"imagecache", "cache", NULL}},
    {{LAGHU_SERVICE_SETTING_FILE_CACHE_SIZE, "file_cache_size", LAGHU_SERVICE_VALUE_SIZE, LAGHU_SERVICE_INHERIT_SCALAR, 1024U * 1024U,
      UINT64_C(1) << 60U, false},
     {"filecachesize", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_FILE_CACHE_INODE_LIMIT, "file_cache_inode_limit", LAGHU_SERVICE_VALUE_UNSIGNED, LAGHU_SERVICE_INHERIT_SCALAR, 16U,
      100000000U, false},
     {"filecacheinodelimit", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_FILE_CACHE_CLEAN_INTERVAL, "file_cache_clean_interval", LAGHU_SERVICE_VALUE_DURATION, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      86400U, false},
     {"filecachecleaninterval", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_FILE_CACHE_METADATA_SIZE, "file_cache_metadata_size", LAGHU_SERVICE_VALUE_SIZE, LAGHU_SERVICE_INHERIT_SCALAR, 16384U,
      1024U * 1024U * 1024U, false},
     {"filecachemetadatasize", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_WORKER_QUEUE, "worker_queue", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U, LAGHU_RUNTIME_PATH_SIZE - 1U,
      false},
     {"workerqueue", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_HTML_REFRESH_QUEUE, "html_refresh_queue", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"htmlrefreshqueue", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_QUEUE, "chrome_analysis_queue", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"chromeanalysisqueue", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_OUTPUT, "chrome_analysis_output", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"chromeanalysisoutput", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_TIMEOUT, "chrome_analysis_timeout", LAGHU_SERVICE_VALUE_UNSIGNED, LAGHU_SERVICE_INHERIT_SCALAR, 100U,
      10000U, false},
     {"chromeanalysistimeout", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_FONT_FETCH_QUEUE, "font_fetch_queue", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"fontfetchqueue", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_FONT_PROVIDER_CONFIG, "font_provider_config", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"fontproviderconfig", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_JAVASCRIPT_QUEUE, "javascript_queue", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"javascriptqueue", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_JAVASCRIPT_TARGET, "javascript_target", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_JAVASCRIPT_TARGET_SIZE - 1U, false},
     {"javascripttarget", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_JAVASCRIPT_OBSERVATION_CONFIG, "javascript_observation_config", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR,
      1U, LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"javascriptobservationconfig", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_JAVASCRIPT_DEFER_CONFIG, "javascript_defer_config", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"javascriptdeferconfig", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_LAYOUT_RESERVATION_CONFIG, "layout_reservation_config", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"layoutreservationconfig", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_ASSET_OFFLOAD_CONFIG, "asset_offload_config", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"assetoffloadconfig", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_ASSET_UPLOAD_QUEUE, "asset_upload_queue", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"assetuploadqueue", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_RUM_STORE, "rum_store", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U, LAGHU_RUNTIME_PATH_SIZE - 1U,
      false},
     {"rumstore", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_RUM_STORE_LOCAL_SNAPSHOT, "rum_store_local_snapshot", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"rumstorelocalsnapshot", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_RUM_STORE_CLIENT_LIBRARY, "rum_store_client_library", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"rumstoreclientlibrary", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_RUM_STORE_REQUIRED, "rum_store_required", LAGHU_SERVICE_VALUE_BOOLEAN, LAGHU_SERVICE_INHERIT_SCALAR, 0U, 1U, false},
     {"rumstorerequired", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_RUM_STORE_TIMEOUT, "rum_store_timeout", LAGHU_SERVICE_VALUE_UNSIGNED, LAGHU_SERVICE_INHERIT_SCALAR, 10U, 10000U, false},
     {"rumstoretimeout", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_RUM_STORE_TTL, "rum_store_ttl", LAGHU_SERVICE_VALUE_DURATION, LAGHU_SERVICE_INHERIT_SCALAR, 3600U, 2592000U, false},
     {"rumstorettl", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_RUM_STORE_RETRY_LIMIT, "rum_store_retry_limit", LAGHU_SERVICE_VALUE_UNSIGNED, LAGHU_SERVICE_INHERIT_SCALAR, 0U, 10U,
      false},
     {"rumstoreretrylimit", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_RUM_STORE_SYNC_INTERVAL, "rum_store_sync_interval", LAGHU_SERVICE_VALUE_DURATION, LAGHU_SERVICE_INHERIT_SCALAR, 1U, 300U,
      false},
     {"rumstoresyncinterval", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_RUM_STORE_MEMORY_LIMIT, "rum_store_memory_limit", LAGHU_SERVICE_VALUE_SIZE, LAGHU_SERVICE_INHERIT_SCALAR, 16384U,
      1073741824U, false},
     {"rumstorememorylimit", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_RUM_STORE_PENDING_LIMIT, "rum_store_pending_limit", LAGHU_SERVICE_VALUE_SIZE, LAGHU_SERVICE_INHERIT_SCALAR, 16384U,
      1073741824U, false},
     {"rumstorependinglimit", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_LOAD_FROM_FILE, "load_from_file", LAGHU_SERVICE_VALUE_ENUM, LAGHU_SERVICE_INHERIT_SCALAR, 0U, 0U, false},
     {"loadfromfile", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP, "file_source_map", LAGHU_SERVICE_VALUE_PAIR, LAGHU_SERVICE_INHERIT_APPEND, 0U, LAGHU_SOURCE_MAX_MAPPINGS,
      true},
     {"filesourcemap", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_PURGE_METHOD, "purge_method", LAGHU_SERVICE_VALUE_BOOLEAN, LAGHU_SERVICE_INHERIT_SCALAR, 0U, 1U, false},
     {"purgemethod", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_PURGE_QUERY, "purge_query", LAGHU_SERVICE_VALUE_BOOLEAN, LAGHU_SERVICE_INHERIT_SCALAR, 0U, 1U, false},
     {"purgequery", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_STATISTICS, "statistics", LAGHU_SERVICE_VALUE_BOOLEAN, LAGHU_SERVICE_INHERIT_SCALAR, 0U, 1U, false}, {NULL, NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_METRICS, "metrics", LAGHU_SERVICE_VALUE_BOOLEAN, LAGHU_SERVICE_INHERIT_SCALAR, 0U, 1U, false}, {NULL, NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_READINESS, "readiness", LAGHU_SERVICE_VALUE_BOOLEAN, LAGHU_SERVICE_INHERIT_SCALAR, 0U, 1U, false}, {NULL, NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_READINESS_POLICY, "readiness_policy", LAGHU_SERVICE_VALUE_ENUM, LAGHU_SERVICE_INHERIT_SCALAR, 0U, 0U, false},
     {"readinesspolicy", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_PURGE_TOKEN_FILE, "purge_token_file", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"purgetokenfile", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_CACHE_FLUSH_FILE, "cache_flush_file", LAGHU_SERVICE_VALUE_STRING, LAGHU_SERVICE_INHERIT_SCALAR, 1U,
      LAGHU_RUNTIME_PATH_SIZE - 1U, false},
     {"cacheflushfile", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_PURGE_ALLOW, "purge_allow", LAGHU_SERVICE_VALUE_CIDR, LAGHU_SERVICE_INHERIT_REPLACE, 0U, LAGHU_SERVICE_CONFIG_MAX_CIDRS,
      true},
     {"purgeallow", NULL, NULL}},
    {{LAGHU_SERVICE_SETTING_TRUSTED_PROXY, "trusted_proxy", LAGHU_SERVICE_VALUE_CIDR, LAGHU_SERVICE_INHERIT_REPLACE, 0U,
      LAGHU_SERVICE_CONFIG_MAX_CIDRS, true},
     {"trustedproxy", NULL, NULL}},
};

static bool laghu_service_name_equal(const char *left, const char *right) {
  size_t left_index = 0U, right_index = 0U;
  if (left == NULL || right == NULL) return false;
  while (left[left_index] == '-') ++left_index;
  while (right[right_index] != '\0' || left[left_index] != '\0') {
    while (left[left_index] == '_' || left[left_index] == '-') ++left_index;
    while (right[right_index] == '_' || right[right_index] == '-') ++right_index;
    if (laghu_base_ascii_lower((unsigned char)left[left_index]) != laghu_base_ascii_lower((unsigned char)right[right_index])) return false;
    if (left[left_index] != '\0') ++left_index;
    if (right[right_index] != '\0') ++right_index;
  }
  return true;
}

static void laghu_service_diagnostic_set(laghu_service_diagnostic *diagnostic, laghu_service_diagnostic_code code, laghu_service_setting setting,
                                         const char *message) {
  if (diagnostic == NULL) return;
  diagnostic->code = code;
  diagnostic->setting = setting;
  (void)laghu_base_string_copy(diagnostic->message, sizeof(diagnostic->message), message);
}

static bool laghu_service_fail(laghu_service_diagnostic *diagnostic, laghu_service_diagnostic_code code, laghu_service_setting setting,
                               const char *message) {
  laghu_service_diagnostic_set(diagnostic, code, setting, message);
  return false;
}

static bool laghu_service_copy(char *output, size_t capacity, const char *value) {
  return value != NULL && value[0] != '\0' && laghu_base_string_copy(output, capacity, value);
}

static bool laghu_service_ipv4_parse(const char *text, unsigned char output[4]) {
  size_t index;
  if (text == NULL || output == NULL) return false;
  for (index = 0U; index < 4U; ++index) {
    unsigned int value = 0U, digits = 0U;
    const char *component = text;
    while (*text >= '0' && *text <= '9') {
      if (value > 25U || (value == 25U && *text > '5')) return false;
      value = value * 10U + (unsigned int)(*text - '0');
      ++digits;
      ++text;
    }
    if (digits == 0U || (digits > 1U && *component == '0')) return false;
    output[index] = (unsigned char)value;
    if (index + 1U == 4U) return *text == '\0';
    if (*text++ != '.') return false;
  }
  return false;
}

static unsigned int laghu_service_hex(char character) {
  if (character >= '0' && character <= '9') return (unsigned int)(character - '0');
  if (character >= 'a' && character <= 'f') return (unsigned int)(character - 'a') + 10U;
  if (character >= 'A' && character <= 'F') return (unsigned int)(character - 'A') + 10U;
  return 16U;
}

static void laghu_service_ipv6_word_write(unsigned char output[16], size_t index, unsigned int value) {
  output[index * 2U] = (unsigned char)(value >> 8U);
  output[index * 2U + 1U] = (unsigned char)value;
}

static bool laghu_service_ipv6_parse(const char *text, unsigned char output[16]) {
  unsigned int words[8], compression = UINT_MAX;
  size_t count = 0U, index;
  if (text == NULL || output == NULL || *text == '\0') return false;
  if (*text == ':') {
    if (*++text != ':') return false;
    compression = 0U;
    ++text;
  }
  while (*text != '\0') {
    unsigned int value = 0U, digits = 0U;
    if (count >= 8U) return false;
    const char *dot = strchr(text, '.');
    const char *separator = strchr(text, ':');
    if (dot != NULL && (separator == NULL || dot < separator)) {
      unsigned char ipv4[4];
      if (count > 6U || !laghu_service_ipv4_parse(text, ipv4)) return false;
      words[count++] = ((unsigned int)ipv4[0] << 8U) | ipv4[1];
      words[count++] = ((unsigned int)ipv4[2] << 8U) | ipv4[3];
      text += strlen(text);
      break;
    }
    while (*text != '\0' && *text != ':') {
      unsigned int digit = laghu_service_hex(*text++);
      if (digit >= 16U || digits == 4U) return false;
      value = (value << 4U) | digit;
      ++digits;
    }
    if (digits == 0U) return false;
    words[count++] = value;
    if (*text == '\0') break;
    ++text;
    if (*text == ':') {
      if (compression != UINT_MAX) return false;
      compression = (unsigned int)count;
      ++text;
    }
  }
  if ((compression == UINT_MAX && count != 8U) || (compression != UINT_MAX && count >= 8U)) return false;
  memset(output, 0, 16U);
  if (compression == UINT_MAX) {
    for (index = 0U; index < count; ++index) laghu_service_ipv6_word_write(output, index, words[index]);
    return true;
  }
  for (index = 0U; index < compression; ++index) laghu_service_ipv6_word_write(output, index, words[index]);
  for (index = compression; index < count; ++index) laghu_service_ipv6_word_write(output, index + 8U - count, words[index]);
  return true;
}

static bool laghu_service_bool(const char *value, bool *output) {
  if (laghu_service_name_equal(value, "on") || laghu_service_name_equal(value, "true")) {
    *output = true;
    return true;
  }
  if (laghu_service_name_equal(value, "off") || laghu_service_name_equal(value, "false")) {
    *output = false;
    return true;
  }
  return false;
}

static bool laghu_service_unsigned(const char *value, uint64_t minimum, uint64_t maximum, unsigned int *output) {
  uint64_t parsed;
  return laghu_base_parse_u64(value, minimum, maximum, &parsed) && parsed <= UINT_MAX && ((*output = (unsigned int)parsed), true);
}

static bool laghu_service_size(const char *value, uint64_t minimum, uint64_t maximum, size_t *output) {
  uint64_t parsed;
  return laghu_cache_size_parse(value, minimum, maximum, &parsed) && parsed <= SIZE_MAX && ((*output = (size_t)parsed), true);
}

static bool laghu_service_duration(const char *value, unsigned int minimum, unsigned int maximum, unsigned int *output) {
  return laghu_cache_duration_parse(value, minimum, maximum, output);
}

static bool laghu_service_present(const laghu_service_config *config, laghu_service_setting setting) {
  return (config->present & LAGHU_SERVICE_BIT(setting)) != 0U;
}

static void laghu_service_mark(laghu_service_config *config, laghu_service_setting setting) { config->present |= LAGHU_SERVICE_BIT(setting); }

static void laghu_service_config_clear_loaded(laghu_service_config *config) {
  if (config == NULL) return;
  free(config->owned_font_providers);
  free(config->owned_javascript_observations);
  free(config->owned_javascript_defer);
  free(config->owned_layout_reservations);
  free(config->owned_asset_offload);
  config->font_providers = NULL;
  config->javascript_observations = NULL;
  config->javascript_defer = NULL;
  config->asset_offload = NULL;
  config->layout_reservations = NULL;
  config->owned_font_providers = NULL;
  config->owned_javascript_observations = NULL;
  config->owned_javascript_defer = NULL;
  config->owned_asset_offload = NULL;
  config->owned_layout_reservations = NULL;
}

static void laghu_service_config_clear_font_providers(laghu_service_config *config) {
  free(config->owned_font_providers);
  config->font_providers = NULL;
  config->owned_font_providers = NULL;
}

static void laghu_service_config_clear_javascript_observations(laghu_service_config *config) {
  free(config->owned_javascript_observations);
  config->javascript_observations = NULL;
  config->owned_javascript_observations = NULL;
}

static void laghu_service_config_clear_javascript_defer(laghu_service_config *config) {
  free(config->owned_javascript_defer);
  config->javascript_defer = NULL;
  config->owned_javascript_defer = NULL;
}

static void laghu_service_config_clear_asset_offload(laghu_service_config *config) {
  free(config->owned_asset_offload);
  config->asset_offload = NULL;
  config->owned_asset_offload = NULL;
}

static bool laghu_service_cidr_add(laghu_service_cidr *values, size_t *count, const laghu_service_cidr *value) {
  size_t index;
  if (*count >= LAGHU_SERVICE_CONFIG_MAX_CIDRS) return false;
  for (index = 0U; index < *count; ++index)
    if (laghu_service_cidr_equal(&values[index], value)) return false;
  values[(*count)++] = *value;
  return true;
}

void laghu_service_config_init(laghu_service_config *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  laghu_cache_limits_init(&config->cache_limits);
  laghu_source_policy_init(&config->source_policy);
  (void)laghu_base_string_copy(config->javascript_target, sizeof(config->javascript_target), "defaults and supports es6-module and not dead");
  (void)laghu_base_string_copy(config->rum_store, sizeof(config->rum_store), "local:");
  config->rum_timeout_ms = LAGHU_RUM_DEFAULT_TIMEOUT_MS;
  config->chrome_analysis_timeout_ms = 1500U;
  config->rum_ttl = LAGHU_IMAGE_METADATA_TTL_DEFAULT;
  config->rum_retry_limit = LAGHU_RUM_DEFAULT_RETRY_LIMIT;
  config->rum_sync_interval = LAGHU_RUM_DEFAULT_SYNC_SECONDS;
  config->rum_memory_limit = LAGHU_RUM_DEFAULT_MEMORY_BYTES;
  config->rum_pending_limit = LAGHU_RUM_DEFAULT_PENDING_BYTES;
}

void laghu_service_config_dispose(laghu_service_config *config) { laghu_service_config_clear_loaded(config); }

laghu_service_setting laghu_service_setting_find(const char *name) {
  size_t index, alias;
  for (index = 0U; index < sizeof(laghu_service_descriptors) / sizeof(laghu_service_descriptors[0]); ++index) {
    if (laghu_service_name_equal(name, laghu_service_descriptors[index].descriptor.name)) return laghu_service_descriptors[index].descriptor.setting;
    for (alias = 0U; alias < sizeof(laghu_service_descriptors[index].aliases) / sizeof(laghu_service_descriptors[index].aliases[0]); ++alias)
      if (laghu_service_descriptors[index].aliases[alias] != NULL && laghu_service_name_equal(name, laghu_service_descriptors[index].aliases[alias]))
        return laghu_service_descriptors[index].descriptor.setting;
  }
  return LAGHU_SERVICE_SETTING_UNKNOWN;
}

const laghu_service_setting_descriptor *laghu_service_setting_describe(laghu_service_setting setting) {
  size_t index;
  for (index = 0U; index < sizeof(laghu_service_descriptors) / sizeof(laghu_service_descriptors[0]); ++index)
    if (laghu_service_descriptors[index].descriptor.setting == setting) return &laghu_service_descriptors[index].descriptor;
  return NULL;
}

bool laghu_service_cidr_parse(const char *value, laghu_service_cidr *cidr) {
  char address[LAGHU_SERVICE_IPV6_TEXT_SIZE];
  const char *slash = value == NULL ? NULL : strrchr(value, '/');
  uint64_t prefix;
  unsigned int maximum, index;
  size_t length;
  if (cidr == NULL || slash == NULL || slash == value) return false;
  length = (size_t)(slash - value);
  if (length == 0U || length >= sizeof(address) || !laghu_base_parse_u64(slash + 1U, 0U, 128U, &prefix)) return false;
  memcpy(address, value, length);
  address[length] = '\0';
  memset(cidr, 0, sizeof(*cidr));
  if (laghu_service_ipv4_parse(address, cidr->address)) {
    cidr->family = LAGHU_SERVICE_CIDR_FAMILY_IPV4;
    maximum = 32U;
  } else if (laghu_service_ipv6_parse(address, cidr->address)) {
    cidr->family = LAGHU_SERVICE_CIDR_FAMILY_IPV6;
    maximum = 128U;
  } else {
    return false;
  }
  if (prefix > maximum) return false;
  cidr->prefix = (unsigned int)prefix;
  for (index = cidr->prefix; index < maximum; ++index)
    if ((cidr->address[index / 8U] & (unsigned char)(1U << (7U - index % 8U))) != 0U) return false;
  return true;
}

bool laghu_service_cidr_equal(const laghu_service_cidr *left, const laghu_service_cidr *right) {
  size_t length;
  if (left == NULL || right == NULL || left->family != right->family || left->prefix != right->prefix) return false;
  length = left->family == LAGHU_SERVICE_CIDR_FAMILY_IPV4 ? 4U : 16U;
  return memcmp(left->address, right->address, length) == 0;
}

bool laghu_service_cidr_matches(const laghu_service_cidr *cidr, const unsigned char address[16], unsigned int family) {
  unsigned int index;
  if (cidr == NULL || address == NULL || cidr->family != family) return false;
  for (index = 0U; index < cidr->prefix; ++index)
    if ((cidr->address[index / 8U] & (unsigned char)(1U << (7U - index % 8U))) != (address[index / 8U] & (unsigned char)(1U << (7U - index % 8U))))
      return false;
  return true;
}

bool laghu_service_config_apply(laghu_service_config *config, laghu_service_setting setting, const char *value,
                                laghu_service_diagnostic *diagnostic) {
  const laghu_service_setting_descriptor *descriptor = laghu_service_setting_describe(setting);
  bool flag;
  uint64_t count;
  char normalized[LAGHU_JAVASCRIPT_TARGET_SIZE];
  laghu_service_cidr cidr;
  if (config == NULL || descriptor == NULL || value == NULL)
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_ARGUMENT, setting, "missing service setting value");
  if (!descriptor->repeatable && laghu_service_present(config, setting))
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_DUPLICATE, setting, "duplicate service setting");
  switch (setting) {
    case LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND:
      if (!laghu_service_copy(config->file_cache_backend, sizeof(config->file_cache_backend), value)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_IMAGE_CACHE:
      if (!laghu_service_copy(config->image_cache, sizeof(config->image_cache), value)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_FILE_CACHE_SIZE:
      if (!laghu_cache_size_parse(value, descriptor->minimum, descriptor->maximum, &config->cache_limits.size_limit)) goto range;
      break;
    case LAGHU_SERVICE_SETTING_FILE_CACHE_INODE_LIMIT:
      if (!laghu_cache_count_parse(value, descriptor->minimum, descriptor->maximum, &count)) goto range;
      config->cache_limits.inode_limit = count;
      break;
    case LAGHU_SERVICE_SETTING_FILE_CACHE_CLEAN_INTERVAL:
      if (!laghu_service_duration(value, (unsigned int)descriptor->minimum, (unsigned int)descriptor->maximum, &config->cache_limits.clean_interval))
        goto range;
      break;
    case LAGHU_SERVICE_SETTING_FILE_CACHE_METADATA_SIZE:
      if (!laghu_service_size(value, descriptor->minimum, descriptor->maximum, &config->cache_limits.metadata_size)) goto range;
      break;
    case LAGHU_SERVICE_SETTING_WORKER_QUEUE:
      if (!laghu_service_copy(config->worker_queue, sizeof(config->worker_queue), value)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_HTML_REFRESH_QUEUE:
      if (!laghu_service_copy(config->html_refresh_queue, sizeof(config->html_refresh_queue), value)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_QUEUE:
      if (!laghu_service_copy(config->chrome_analysis_queue, sizeof(config->chrome_analysis_queue), value)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_OUTPUT:
      if (!laghu_service_copy(config->chrome_analysis_output, sizeof(config->chrome_analysis_output), value)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_TIMEOUT:
      if (!laghu_service_unsigned(value, descriptor->minimum, descriptor->maximum, &config->chrome_analysis_timeout_ms)) goto range;
      break;
    case LAGHU_SERVICE_SETTING_FONT_FETCH_QUEUE:
      if (!laghu_service_copy(config->font_fetch_queue, sizeof(config->font_fetch_queue), value)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_FONT_PROVIDER_CONFIG:
      if (!laghu_service_copy(config->font_provider_config, sizeof(config->font_provider_config), value)) goto format;
      laghu_service_config_clear_font_providers(config);
      break;
    case LAGHU_SERVICE_SETTING_JAVASCRIPT_QUEUE:
      if (!laghu_service_copy(config->javascript_queue, sizeof(config->javascript_queue), value)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_JAVASCRIPT_TARGET:
      if (!laghu_javascript_target_normalize(value, normalized) ||
          !laghu_service_copy(config->javascript_target, sizeof(config->javascript_target), normalized))
        goto format;
      break;
    case LAGHU_SERVICE_SETTING_JAVASCRIPT_OBSERVATION_CONFIG:
      if (!laghu_service_copy(config->javascript_observation_config, sizeof(config->javascript_observation_config), value)) goto format;
      laghu_service_config_clear_javascript_observations(config);
      break;
    case LAGHU_SERVICE_SETTING_JAVASCRIPT_DEFER_CONFIG:
      if (!laghu_service_copy(config->javascript_defer_config, sizeof(config->javascript_defer_config), value)) goto format;
      laghu_service_config_clear_javascript_defer(config);
      break;
    case LAGHU_SERVICE_SETTING_LAYOUT_RESERVATION_CONFIG:
      if (!laghu_service_copy(config->layout_reservation_config, sizeof(config->layout_reservation_config), value)) goto format;
      free(config->owned_layout_reservations);
      config->layout_reservations = NULL;
      config->owned_layout_reservations = NULL;
      break;
    case LAGHU_SERVICE_SETTING_ASSET_OFFLOAD_CONFIG:
      if (!laghu_service_copy(config->asset_offload_config, sizeof(config->asset_offload_config), value)) goto format;
      laghu_service_config_clear_asset_offload(config);
      break;
    case LAGHU_SERVICE_SETTING_ASSET_UPLOAD_QUEUE:
      if (!laghu_service_copy(config->asset_upload_queue, sizeof(config->asset_upload_queue), value)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_RUM_STORE:
      if (!laghu_rum_store_validate(value, NULL, 0U) || !laghu_service_copy(config->rum_store, sizeof(config->rum_store), value)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_RUM_STORE_LOCAL_SNAPSHOT:
      if (!laghu_service_copy(config->rum_snapshot_path, sizeof(config->rum_snapshot_path), value)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_RUM_STORE_CLIENT_LIBRARY:
      if (!laghu_service_copy(config->rum_client_library, sizeof(config->rum_client_library), value)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_RUM_STORE_REQUIRED:
      if (value[0] == '\0')
        flag = true;
      else if (!laghu_service_bool(value, &flag))
        goto format;
      config->rum_store_required = flag;
      break;
    case LAGHU_SERVICE_SETTING_RUM_STORE_TIMEOUT:
      if (!laghu_service_unsigned(value, descriptor->minimum, descriptor->maximum, &config->rum_timeout_ms)) goto range;
      break;
    case LAGHU_SERVICE_SETTING_RUM_STORE_TTL:
      if (!laghu_service_duration(value, (unsigned int)descriptor->minimum, (unsigned int)descriptor->maximum, &config->rum_ttl)) goto range;
      break;
    case LAGHU_SERVICE_SETTING_RUM_STORE_RETRY_LIMIT:
      if (!laghu_service_unsigned(value, descriptor->minimum, descriptor->maximum, &config->rum_retry_limit)) goto range;
      break;
    case LAGHU_SERVICE_SETTING_RUM_STORE_SYNC_INTERVAL:
      if (!laghu_service_duration(value, (unsigned int)descriptor->minimum, (unsigned int)descriptor->maximum, &config->rum_sync_interval))
        goto range;
      break;
    case LAGHU_SERVICE_SETTING_RUM_STORE_MEMORY_LIMIT:
      if (!laghu_service_size(value, descriptor->minimum, descriptor->maximum, &config->rum_memory_limit)) goto range;
      break;
    case LAGHU_SERVICE_SETTING_RUM_STORE_PENDING_LIMIT:
      if (!laghu_service_size(value, descriptor->minimum, descriptor->maximum, &config->rum_pending_limit)) goto range;
      break;
    case LAGHU_SERVICE_SETTING_LOAD_FROM_FILE:
      if (!laghu_source_mode_parse(value, true, &config->source_policy.mode)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_PURGE_METHOD:
      if (laghu_service_name_equal(value, "PURGE"))
        config->purge_method = true;
      else if (!laghu_service_bool(value, &config->purge_method))
        goto format;
      break;
    case LAGHU_SERVICE_SETTING_PURGE_QUERY:
      if (!laghu_service_bool(value, &config->purge_query)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_STATISTICS:
      if (!laghu_service_bool(value, &config->statistics)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_METRICS:
      if (!laghu_service_bool(value, &config->metrics)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_READINESS:
      if (!laghu_service_bool(value, &config->readiness)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_READINESS_POLICY:
      if (laghu_service_name_equal(value, "strict"))
        config->readiness_strict = true;
      else if (laghu_service_name_equal(value, "degraded"))
        config->readiness_strict = false;
      else
        goto format;
      break;
    case LAGHU_SERVICE_SETTING_PURGE_TOKEN_FILE:
      if (!laghu_service_copy(config->purge_token_file, sizeof(config->purge_token_file), value)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_CACHE_FLUSH_FILE:
      if (!laghu_service_copy(config->cache_flush_file, sizeof(config->cache_flush_file), value)) goto format;
      break;
    case LAGHU_SERVICE_SETTING_PURGE_ALLOW:
      if (!laghu_service_cidr_parse(value, &cidr) || !laghu_service_cidr_add(config->purge_allow, &config->purge_allow_count, &cidr)) goto capacity;
      break;
    case LAGHU_SERVICE_SETTING_TRUSTED_PROXY:
      if (!laghu_service_cidr_parse(value, &cidr) || !laghu_service_cidr_add(config->trusted_proxies, &config->trusted_proxy_count, &cidr))
        goto capacity;
      break;
    case LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP:
    case LAGHU_SERVICE_SETTING_UNKNOWN:
    case LAGHU_SERVICE_SETTING_COUNT:
      return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_ARGUMENT, setting, "service setting requires two values");
  }
  laghu_service_mark(config, setting);
  laghu_service_diagnostic_set(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_NONE, setting, "");
  return true;
format:
  return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_FORMAT, setting, "invalid service setting value");
range:
  return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_RANGE, setting, "service setting is outside its bounds");
capacity:
  return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_CAPACITY, setting, "duplicate or excessive service setting");
}

bool laghu_service_config_apply_pair(laghu_service_config *config, laghu_service_setting setting, const char *first, const char *second,
                                     laghu_service_diagnostic *diagnostic) {
  if (config == NULL || setting != LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP || first == NULL || second == NULL ||
      !laghu_source_mapping_add(&config->source_policy, first, second))
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_FORMAT, setting, "invalid source mapping");
  laghu_service_mark(config, setting);
  laghu_service_diagnostic_set(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_NONE, setting, "");
  return true;
}

#define LAGHU_SERVICE_MERGE_FIELD(setting, field)        \
  do {                                                   \
    if (laghu_service_present(child, setting)) {         \
      merged->field = child->field;                      \
      laghu_service_mark(merged, setting);               \
    } else if (laghu_service_present(parent, setting)) { \
      merged->field = parent->field;                     \
      laghu_service_mark(merged, setting);               \
    }                                                    \
  } while (0)

#define LAGHU_SERVICE_MERGE_STRING(setting, field)                                       \
  do {                                                                                   \
    if (laghu_service_present(child, setting)) {                                         \
      (void)laghu_base_string_copy(merged->field, sizeof(merged->field), child->field);  \
      laghu_service_mark(merged, setting);                                               \
    } else if (laghu_service_present(parent, setting)) {                                 \
      (void)laghu_base_string_copy(merged->field, sizeof(merged->field), parent->field); \
      laghu_service_mark(merged, setting);                                               \
    }                                                                                    \
  } while (0)

bool laghu_service_config_merge(laghu_service_config *merged, const laghu_service_config *parent, const laghu_service_config *child,
                                laghu_service_diagnostic *diagnostic) {
  laghu_source_policy source;
  const laghu_service_config *list_source;
  if (merged == NULL || parent == NULL || child == NULL || merged == parent)
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_ARGUMENT, LAGHU_SERVICE_SETTING_UNKNOWN, "invalid service configuration merge");
  /* Native configuration APIs merge into a fresh or child allocation.  Parent
   * aliasing is forbidden because a parent can outlive child borrowed state. */
  if (merged != child) laghu_service_config_init(merged);
  /* FileCacheBackend and the legacy ImageCache name are one mutually
   * exclusive selector.  An explicit child selector replaces either parent
   * spelling; merging them independently would manufacture a conflict. */
  if (laghu_service_present(child, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND)) {
    (void)laghu_base_string_copy(merged->file_cache_backend, sizeof(merged->file_cache_backend), child->file_cache_backend);
    laghu_service_mark(merged, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND);
  } else if (laghu_service_present(child, LAGHU_SERVICE_SETTING_IMAGE_CACHE)) {
    (void)laghu_base_string_copy(merged->image_cache, sizeof(merged->image_cache), child->image_cache);
    laghu_service_mark(merged, LAGHU_SERVICE_SETTING_IMAGE_CACHE);
  } else if (laghu_service_present(parent, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND)) {
    (void)laghu_base_string_copy(merged->file_cache_backend, sizeof(merged->file_cache_backend), parent->file_cache_backend);
    laghu_service_mark(merged, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND);
  } else if (laghu_service_present(parent, LAGHU_SERVICE_SETTING_IMAGE_CACHE)) {
    (void)laghu_base_string_copy(merged->image_cache, sizeof(merged->image_cache), parent->image_cache);
    laghu_service_mark(merged, LAGHU_SERVICE_SETTING_IMAGE_CACHE);
  }
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_FILE_CACHE_SIZE, cache_limits.size_limit);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_FILE_CACHE_INODE_LIMIT, cache_limits.inode_limit);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_FILE_CACHE_CLEAN_INTERVAL, cache_limits.clean_interval);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_FILE_CACHE_METADATA_SIZE, cache_limits.metadata_size);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_WORKER_QUEUE, worker_queue);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_HTML_REFRESH_QUEUE, html_refresh_queue);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_QUEUE, chrome_analysis_queue);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_OUTPUT, chrome_analysis_output);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_TIMEOUT, chrome_analysis_timeout_ms);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_FONT_FETCH_QUEUE, font_fetch_queue);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_FONT_PROVIDER_CONFIG, font_provider_config);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_JAVASCRIPT_QUEUE, javascript_queue);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_JAVASCRIPT_TARGET, javascript_target);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_JAVASCRIPT_OBSERVATION_CONFIG, javascript_observation_config);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_JAVASCRIPT_DEFER_CONFIG, javascript_defer_config);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_LAYOUT_RESERVATION_CONFIG, layout_reservation_config);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_ASSET_OFFLOAD_CONFIG, asset_offload_config);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_ASSET_UPLOAD_QUEUE, asset_upload_queue);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_RUM_STORE, rum_store);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_RUM_STORE_LOCAL_SNAPSHOT, rum_snapshot_path);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_RUM_STORE_CLIENT_LIBRARY, rum_client_library);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_RUM_STORE_REQUIRED, rum_store_required);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_RUM_STORE_TIMEOUT, rum_timeout_ms);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_RUM_STORE_TTL, rum_ttl);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_RUM_STORE_RETRY_LIMIT, rum_retry_limit);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_RUM_STORE_SYNC_INTERVAL, rum_sync_interval);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_RUM_STORE_MEMORY_LIMIT, rum_memory_limit);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_RUM_STORE_PENDING_LIMIT, rum_pending_limit);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_PURGE_METHOD, purge_method);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_PURGE_QUERY, purge_query);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_STATISTICS, statistics);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_METRICS, metrics);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_READINESS, readiness);
  LAGHU_SERVICE_MERGE_FIELD(LAGHU_SERVICE_SETTING_READINESS_POLICY, readiness_strict);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_PURGE_TOKEN_FILE, purge_token_file);
  LAGHU_SERVICE_MERGE_STRING(LAGHU_SERVICE_SETTING_CACHE_FLUSH_FILE, cache_flush_file);
  /* Apache merges effective configuration on request paths.  Select prepared
   * startup records by pointer and retain ownership only in their originating
   * configuration, so merge never allocates, loads, or copies large records. */
#define LAGHU_SERVICE_SELECT_RESOURCE(setting, field)                                               \
  do {                                                                                              \
    const laghu_service_config *resource_source = laghu_service_present(child, setting)    ? child  \
                                                  : laghu_service_present(parent, setting) ? parent \
                                                                                           : NULL;  \
    if (merged != resource_source) {                                                                \
      merged->field = resource_source == NULL ? NULL : resource_source->field;                      \
    }                                                                                               \
  } while (0)
  LAGHU_SERVICE_SELECT_RESOURCE(LAGHU_SERVICE_SETTING_FONT_PROVIDER_CONFIG, font_providers);
  LAGHU_SERVICE_SELECT_RESOURCE(LAGHU_SERVICE_SETTING_JAVASCRIPT_OBSERVATION_CONFIG, javascript_observations);
  LAGHU_SERVICE_SELECT_RESOURCE(LAGHU_SERVICE_SETTING_JAVASCRIPT_DEFER_CONFIG, javascript_defer);
  LAGHU_SERVICE_SELECT_RESOURCE(LAGHU_SERVICE_SETTING_ASSET_OFFLOAD_CONFIG, asset_offload);
#undef LAGHU_SERVICE_SELECT_RESOURCE
  if (!laghu_source_policy_merge(&source, &parent->source_policy, &child->source_policy))
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_CONFLICT, LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP,
                              "invalid inherited source mapping");
  if (laghu_service_present(child, LAGHU_SERVICE_SETTING_LOAD_FROM_FILE)) {
    source.mode = child->source_policy.mode;
    laghu_service_mark(merged, LAGHU_SERVICE_SETTING_LOAD_FROM_FILE);
  } else if (laghu_service_present(parent, LAGHU_SERVICE_SETTING_LOAD_FROM_FILE)) {
    source.mode = parent->source_policy.mode;
    laghu_service_mark(merged, LAGHU_SERVICE_SETTING_LOAD_FROM_FILE);
  }
  if (source.mode == LAGHU_SOURCE_FILE_OFF) {
    source.mapping_count = 0U;
    source.native_root[0] = '\0';
  }
  merged->source_policy = source;
  if (laghu_service_present(child, LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP) || laghu_service_present(parent, LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP))
    laghu_service_mark(merged, LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP);
  list_source = laghu_service_present(child, LAGHU_SERVICE_SETTING_PURGE_ALLOW) ? child : parent;
  if (laghu_service_present(list_source, LAGHU_SERVICE_SETTING_PURGE_ALLOW)) {
    memcpy(merged->purge_allow, list_source->purge_allow, sizeof(merged->purge_allow));
    merged->purge_allow_count = list_source->purge_allow_count;
    laghu_service_mark(merged, LAGHU_SERVICE_SETTING_PURGE_ALLOW);
  }
  list_source = laghu_service_present(child, LAGHU_SERVICE_SETTING_TRUSTED_PROXY) ? child : parent;
  if (laghu_service_present(list_source, LAGHU_SERVICE_SETTING_TRUSTED_PROXY)) {
    memcpy(merged->trusted_proxies, list_source->trusted_proxies, sizeof(merged->trusted_proxies));
    merged->trusted_proxy_count = list_source->trusted_proxy_count;
    laghu_service_mark(merged, LAGHU_SERVICE_SETTING_TRUSTED_PROXY);
  }
  laghu_service_diagnostic_set(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_NONE, LAGHU_SERVICE_SETTING_UNKNOWN, "");
  return true;
}

#undef LAGHU_SERVICE_MERGE_FIELD
#undef LAGHU_SERVICE_MERGE_STRING

bool laghu_service_config_prepare_resources(laghu_service_config *config, laghu_service_diagnostic *diagnostic) {
  char error[LAGHU_SERVICE_CONFIG_DIAGNOSTIC_SIZE];
  if (config == NULL)
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_ARGUMENT, LAGHU_SERVICE_SETTING_UNKNOWN, "missing service configuration");
  if (config->font_provider_config[0] != '\0' && config->font_providers == NULL) {
    config->owned_font_providers = calloc(1U, sizeof(*config->owned_font_providers));
    config->font_providers = config->owned_font_providers;
    if (config->font_providers == NULL) {
      laghu_service_config_clear_loaded(config);
      return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_IO, LAGHU_SERVICE_SETTING_FONT_PROVIDER_CONFIG,
                                "unable to load font provider configuration");
    }
    if (!laghu_font_providers_load(config->font_provider_config, config->font_providers, error, sizeof(error))) {
      laghu_service_config_clear_loaded(config);
      return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_IO, LAGHU_SERVICE_SETTING_FONT_PROVIDER_CONFIG,
                                "unable to load font provider configuration");
    }
  }
  if (config->javascript_observation_config[0] != '\0' && config->javascript_observations == NULL) {
    config->owned_javascript_observations = calloc(1U, sizeof(*config->owned_javascript_observations));
    config->javascript_observations = config->owned_javascript_observations;
    if (config->javascript_observations == NULL) {
      laghu_service_config_clear_loaded(config);
      return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_IO, LAGHU_SERVICE_SETTING_JAVASCRIPT_OBSERVATION_CONFIG,
                                "unable to load JavaScript observation configuration");
    }
    if (!laghu_javascript_observations_load(config->javascript_observation_config, config->javascript_observations, error, sizeof(error))) {
      laghu_service_config_clear_loaded(config);
      return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_IO, LAGHU_SERVICE_SETTING_JAVASCRIPT_OBSERVATION_CONFIG,
                                "unable to load JavaScript observation configuration");
    }
  }
  if (config->javascript_defer_config[0] != '\0' && config->javascript_defer == NULL) {
    config->owned_javascript_defer = calloc(1U, sizeof(*config->owned_javascript_defer));
    config->javascript_defer = config->owned_javascript_defer;
    if (config->javascript_defer == NULL) {
      laghu_service_config_clear_loaded(config);
      return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_IO, LAGHU_SERVICE_SETTING_JAVASCRIPT_DEFER_CONFIG,
                                "unable to load JavaScript defer configuration");
    }
    if (!laghu_javascript_defer_load(config->javascript_defer_config, config->javascript_defer, error, sizeof(error))) {
      laghu_service_config_clear_loaded(config);
      return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_IO, LAGHU_SERVICE_SETTING_JAVASCRIPT_DEFER_CONFIG,
                                "unable to load JavaScript defer configuration");
    }
  }
  if (config->layout_reservation_config[0] != '\0' && config->layout_reservations == NULL) {
    config->owned_layout_reservations = calloc(1U, sizeof(*config->owned_layout_reservations));
    config->layout_reservations = config->owned_layout_reservations;
    if (config->layout_reservations == NULL ||
        !laghu_layout_reservations_load(config->layout_reservation_config, config->layout_reservations, error, sizeof(error))) {
      laghu_service_config_clear_loaded(config);
      return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_IO, LAGHU_SERVICE_SETTING_LAYOUT_RESERVATION_CONFIG,
                                "unable to load layout reservation configuration");
    }
  }
  if (config->asset_offload_config[0] != '\0' && config->asset_offload == NULL) {
    config->owned_asset_offload = calloc(1U, sizeof(*config->owned_asset_offload));
    config->asset_offload = config->owned_asset_offload;
    if (config->asset_offload == NULL) {
      laghu_service_config_clear_loaded(config);
      return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_IO, LAGHU_SERVICE_SETTING_ASSET_OFFLOAD_CONFIG,
                                "unable to load asset offload configuration");
    }
    if (!laghu_asset_config_load(config->asset_offload_config, config->asset_offload, error, sizeof(error))) {
      laghu_service_config_clear_loaded(config);
      return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_IO, LAGHU_SERVICE_SETTING_ASSET_OFFLOAD_CONFIG,
                                "unable to load asset offload configuration");
    }
  }
  laghu_service_diagnostic_set(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_NONE, LAGHU_SERVICE_SETTING_UNKNOWN, "");
  return true;
}

bool laghu_service_config_validate(laghu_service_config *config, const laghu_service_finalize_options *options,
                                   laghu_service_diagnostic *diagnostic) {
  bool administration;
  char error[LAGHU_SERVICE_CONFIG_DIAGNOSTIC_SIZE];
  if (config == NULL || options == NULL)
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_ARGUMENT, LAGHU_SERVICE_SETTING_UNKNOWN, "missing service configuration");
  if (laghu_service_present(config, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND) && laghu_service_present(config, LAGHU_SERVICE_SETTING_IMAGE_CACHE))
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_CONFLICT, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND,
                              "file cache backend conflicts with legacy image cache");
  if (options->require_cache && config->file_cache_backend[0] == '\0' && config->image_cache[0] == '\0')
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_DEPENDENCY, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND, "file cache is required");
  if (config->file_cache_backend[0] != '\0') {
    char parsed[LAGHU_RUNTIME_PATH_SIZE];
    if (!laghu_cache_backend_uri_parse(config->file_cache_backend, parsed, sizeof(parsed)))
      return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_FORMAT, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND, "invalid file cache backend");
    (void)laghu_base_string_copy(config->image_cache, sizeof(config->image_cache), parsed);
  }
  if (options->require_worker_queue && config->worker_queue[0] == '\0')
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_DEPENDENCY, LAGHU_SERVICE_SETTING_WORKER_QUEUE, "worker queue is required");
  if (laghu_service_present(config, LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_TIMEOUT) && config->chrome_analysis_queue[0] == '\0')
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_DEPENDENCY, LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_TIMEOUT,
                              "chrome analysis timeout requires chrome analysis queue");
  if (config->chrome_analysis_output[0] != '\0' && config->chrome_analysis_queue[0] == '\0')
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_DEPENDENCY, LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_OUTPUT,
                              "chrome analysis output requires chrome analysis queue");
  if ((config->font_provider_config[0] != '\0' && config->font_providers == NULL) ||
      (config->javascript_observation_config[0] != '\0' && config->javascript_observations == NULL) ||
      (config->javascript_defer_config[0] != '\0' && config->javascript_defer == NULL) ||
      (config->layout_reservation_config[0] != '\0' && config->layout_reservations == NULL) ||
      (config->asset_offload_config[0] != '\0' && config->asset_offload == NULL))
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_IO, LAGHU_SERVICE_SETTING_UNKNOWN, "service resources are not prepared");
  if ((config->font_fetch_queue[0] == '\0') != (config->font_provider_config[0] == '\0'))
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_DEPENDENCY, LAGHU_SERVICE_SETTING_FONT_PROVIDER_CONFIG,
                              "font provider config and queue require each other");
  if ((config->asset_offload_config[0] == '\0') != (config->asset_upload_queue[0] == '\0')) {
    laghu_service_config_clear_loaded(config);
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_DEPENDENCY, LAGHU_SERVICE_SETTING_ASSET_OFFLOAD_CONFIG,
                              "asset offload config and upload queue require each other");
  }
  if (config->asset_offload_config[0] != '\0') {
    if (strcmp(config->asset_offload->queue_path, config->asset_upload_queue) != 0) {
      laghu_service_config_clear_loaded(config);
      return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_CONFLICT, LAGHU_SERVICE_SETTING_ASSET_UPLOAD_QUEUE,
                                "asset upload queue does not match asset configuration");
    }
  }
  if (!laghu_source_policy_validate(&config->source_policy, options->native_file_loading, error, sizeof(error))) {
    laghu_service_config_clear_loaded(config);
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_CONFLICT, LAGHU_SERVICE_SETTING_LOAD_FROM_FILE, "invalid source policy");
  }
  if (config->source_policy.mode != LAGHU_SOURCE_FILE_OFF && config->asset_offload == NULL) {
    laghu_service_config_clear_loaded(config);
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_DEPENDENCY, LAGHU_SERVICE_SETTING_LOAD_FROM_FILE,
                              "direct file loading requires asset offload");
  }
  if (!laghu_rum_store_validate(config->rum_store, NULL, 0U)) {
    laghu_service_config_clear_loaded(config);
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_FORMAT, LAGHU_SERVICE_SETTING_RUM_STORE, "invalid RUM store");
  }
  administration = config->purge_method || config->purge_query || config->statistics || config->metrics || config->readiness;
  if (options->require_admin_authorization && administration && (config->purge_token_file[0] == '\0' || config->purge_allow_count == 0U)) {
    laghu_service_config_clear_loaded(config);
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_DEPENDENCY, LAGHU_SERVICE_SETTING_PURGE_ALLOW,
                              "network administration requires token and allowlist");
  }
  if (options->respect_x_forwarded_proto && config->trusted_proxy_count == 0U) {
    laghu_service_config_clear_loaded(config);
    return laghu_service_fail(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_DEPENDENCY, LAGHU_SERVICE_SETTING_TRUSTED_PROXY,
                              "forwarded protocol trust requires trusted proxy");
  }
  laghu_service_diagnostic_set(diagnostic, LAGHU_SERVICE_DIAGNOSTIC_NONE, LAGHU_SERVICE_SETTING_UNKNOWN, "");
  return true;
}

bool laghu_service_config_finalize(laghu_service_config *config, const laghu_service_finalize_options *options,
                                   laghu_service_diagnostic *diagnostic) {
  if (!laghu_service_config_prepare_resources(config, diagnostic)) return false;
  return laghu_service_config_validate(config, options, diagnostic);
}
