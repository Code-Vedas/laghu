// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/config.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/proxy.h"
#include "laghu/types.h"

static bool proxy_uint(const char *value, unsigned int minimum,
                       unsigned int maximum, unsigned int *output) {
  unsigned long parsed;
  char *end = NULL;
  if (value == NULL || *value == '\0') return false;
  errno = 0;
  parsed = strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < minimum ||
      parsed > maximum)
    return false;
  *output = (unsigned int)parsed;
  return true;
}

static bool proxy_copy(char *output, size_t capacity, const char *value) {
  size_t length = value == NULL ? 0U : strlen(value);
  if (length == 0U || length >= capacity) return false;
  memcpy(output, value, length + 1U);
  return true;
}

static bool proxy_absolute_path(const char *value) {
  return value != NULL && value[0] == '/';
}

static bool proxy_endpoint(const char *value, char *host, size_t host_capacity,
                           char port[6]) {
  const char *separator;
  size_t host_length;
  unsigned int parsed_port;
  if (value == NULL || *value == '\0') return false;
  if (*value == '[') {
    const char *end = strchr(value, ']');
    if (end == NULL || end[1] != ':') return false;
    separator = end + 1;
    host_length = (size_t)(end - value - 1);
    ++value;
  } else {
    separator = strrchr(value, ':');
    if (separator == NULL || strchr(value, ':') != separator) return false;
    host_length = (size_t)(separator - value);
  }
  if (host_length == 0U || host_length >= host_capacity ||
      !proxy_uint(separator + 1, 1U, 65535U, &parsed_port))
    return false;
  {
    size_t index;
    for (index = 0U; index < host_length; ++index)
      if ((unsigned char)value[index] <= 32U || value[index] == '/' ||
          value[index] == '\\')
        return false;
  }
  memcpy(host, value, host_length);
  host[host_length] = '\0';
  (void)snprintf(port, 6U, "%u", parsed_port);
  return true;
}

static bool proxy_origin(const char *value, laghu_proxy_options *options) {
  const char *authority;
  const char *separator;
  size_t length;
  unsigned int port;
  if (value != NULL && strncmp(value, "http://", 7U) == 0) {
    options->origin_tls = false;
    authority = value + 7U;
    port = 80U;
  } else if (value != NULL && strncmp(value, "https://", 8U) == 0) {
    options->origin_tls = true;
    authority = value + 8U;
    port = 443U;
  } else {
    return false;
  }
  if (*authority == '\0' || strpbrk(authority, "/?#@") != NULL) return false;
  length = strlen(authority);
  if (length >= sizeof(options->origin_authority)) return false;
  memcpy(options->origin_authority, authority, length + 1U);
  if (*authority == '[') {
    const char *end = strchr(authority, ']');
    size_t host_length;
    if (end == NULL || (end[1] != '\0' && end[1] != ':')) return false;
    host_length = (size_t)(end - authority - 1);
    if (host_length == 0U || host_length >= sizeof(options->origin_host))
      return false;
    memcpy(options->origin_host, authority + 1, host_length);
    options->origin_host[host_length] = '\0';
    if (end[1] == ':' && !proxy_uint(end + 2, 1U, 65535U, &port)) return false;
  } else if ((separator = strrchr(authority, ':')) != NULL) {
    size_t host_length = (size_t)(separator - authority);
    if (host_length == 0U || host_length >= sizeof(options->origin_host) ||
        !proxy_uint(separator + 1, 1U, 65535U, &port))
      return false;
    memcpy(options->origin_host, authority, host_length);
    options->origin_host[host_length] = '\0';
  } else if (!proxy_copy(options->origin_host, sizeof(options->origin_host),
                         authority)) {
    return false;
  }
  {
    size_t index;
    for (index = 0U; options->origin_host[index] != '\0'; ++index)
      if ((unsigned char)options->origin_host[index] <= 32U ||
          options->origin_host[index] == '\\')
        return false;
  }
  (void)snprintf(options->origin_port, sizeof(options->origin_port), "%u",
                 port);
  return true;
}

void laghu_proxy_options_init(laghu_proxy_options *options) {
  laghu_config child;
  memset(options, 0, sizeof(*options));
  laghu_config_init(&child);
  child.mode = LAGHU_MODE_ON;
  child.preset = LAGHU_PRESET_BALANCED;
  laghu_config_merge(&options->config, NULL, &child);
  options->workers = LAGHU_PROXY_DEFAULT_WORKERS;
  options->connection_queue = LAGHU_PROXY_DEFAULT_QUEUE;
  options->connect_timeout = LAGHU_PROXY_DEFAULT_CONNECT_TIMEOUT;
  options->io_timeout = LAGHU_PROXY_DEFAULT_IO_TIMEOUT;
  options->drain_timeout = LAGHU_PROXY_DEFAULT_DRAIN_TIMEOUT;
  laghu_service_config_init(&options->service);
}

void laghu_proxy_options_dispose(laghu_proxy_options *options) {
  if (options != NULL) laghu_service_config_dispose(&options->service);
}

static laghu_proxy_parse_result proxy_error(char *error, size_t capacity,
                                            const char *message) {
  if (capacity != 0U) (void)snprintf(error, capacity, "%s", message);
  return LAGHU_PROXY_PARSE_ERROR;
}

laghu_proxy_parse_result laghu_proxy_parse_options(int argc, char **argv,
                                                   laghu_proxy_options *options,
                                                   char *error,
                                                   size_t error_size) {
  bool listen_seen = false, origin_seen = false, selector_seen = false;
  bool javascript_inline_limit_seen = false;
  bool javascript_outline_threshold_seen = false;
  bool instrumentation_sample_rate_seen = false;
  bool javascript_defer_suggestions_seen = false;
  bool quality_seen = false, workers_seen = false;
  bool connection_queue_seen = false, connect_timeout_seen = false;
  bool io_timeout_seen = false, drain_timeout_seen = false;
  bool ca_seen = false, forwarded_seen = false;
  bool respect_vary_seen = false, respect_proto_seen = false;
  bool query_overrides_seen = false;
  laghu_config shared_config;
  int index;
  if (options == NULL || argc < 1)
    return proxy_error(error, error_size, "invalid arguments");
  laghu_config_init(&shared_config);
  for (index = 1; index < argc; ++index) {
    const char *name = argv[index];
    const char *value = index + 1 < argc ? argv[index + 1] : NULL;
    laghu_config_setting shared_setting = laghu_config_setting_find(name);
    laghu_service_setting service_setting = laghu_service_setting_find(name);
    laghu_service_diagnostic service_diagnostic;
#define NEED_VALUE()                                                    \
  do {                                                                  \
    if (value == NULL || value[0] == '-')                               \
      return proxy_error(error, error_size, "option requires a value"); \
    ++index;                                                            \
  } while (0)
    if (strcmp(name, "--help") == 0) return LAGHU_PROXY_PARSE_HELP;
    if (strcmp(name, "--version") == 0) return LAGHU_PROXY_PARSE_VERSION;
    if (shared_setting != LAGHU_CONFIG_SETTING_UNKNOWN) {
      if (shared_setting == LAGHU_CONFIG_SETTING_MAP_REWRITE_DOMAIN ||
          shared_setting == LAGHU_CONFIG_SETTING_MAP_PROXY_DOMAIN ||
          shared_setting == LAGHU_CONFIG_SETTING_SHARD_DOMAIN) {
        const char *second = index + 2 < argc ? argv[index + 2] : NULL;
        if (value == NULL || second == NULL || value[0] == '-' ||
            second[0] == '-' ||
            !laghu_config_setting_apply_pair(&shared_config, shared_setting,
                                             value, second, error, error_size))
          return LAGHU_PROXY_PARSE_ERROR;
        index += 2;
        continue;
      }
      bool implicit_on =
          shared_setting == LAGHU_CONFIG_SETTING_ALLOW_API ||
          shared_setting == LAGHU_CONFIG_SETTING_IMAGE_BEACON ||
          shared_setting == LAGHU_CONFIG_SETTING_CRITICAL_CSS_BEACON ||
          shared_setting == LAGHU_CONFIG_SETTING_INSTRUMENTATION_BEACON ||
          shared_setting == LAGHU_CONFIG_SETTING_INCLUDE_JS_SOURCE_MAPS;
      const char *shared_value = implicit_on ? "on" : value;
      if (!implicit_on) NEED_VALUE();
      if (!laghu_config_setting_apply(&shared_config, shared_setting,
                                      shared_value, error, error_size))
        return LAGHU_PROXY_PARSE_ERROR;
      continue;
    }
    if (service_setting != LAGHU_SERVICE_SETTING_UNKNOWN) {
      const char *service_value = value;
      if (service_setting == LAGHU_SERVICE_SETTING_RUM_STORE_REQUIRED) {
        service_value = "on";
      } else {
        NEED_VALUE();
      }
      if (service_setting == LAGHU_SERVICE_SETTING_PURGE_METHOD &&
          strcmp(service_value, "PURGE") != 0)
        return proxy_error(error, error_size,
                           "--purge-method accepts PURGE once");
      if ((service_setting == LAGHU_SERVICE_SETTING_PURGE_TOKEN_FILE ||
           service_setting == LAGHU_SERVICE_SETTING_CACHE_FLUSH_FILE) &&
          !proxy_absolute_path(service_value))
        return proxy_error(error, error_size, "invalid absolute file");
      if (service_setting == LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP) {
        const char *separator = strchr(service_value, '=');
        char prefix[LAGHU_RUNTIME_PATH_SIZE];
        size_t prefix_length =
            separator == NULL ? 0U : (size_t)(separator - service_value);
        if (prefix_length == 0U || prefix_length >= sizeof(prefix) ||
            separator[1] == '\0')
          return proxy_error(error, error_size, "invalid --file-source-map");
        memcpy(prefix, service_value, prefix_length);
        prefix[prefix_length] = '\0';
        if (!laghu_service_config_apply_pair(&options->service, service_setting,
                                             prefix, separator + 1U,
                                             &service_diagnostic))
          return proxy_error(error, error_size, service_diagnostic.message);
      } else if (!laghu_service_config_apply(&options->service, service_setting,
                                             service_value,
                                             &service_diagnostic)) {
        return proxy_error(error, error_size, service_diagnostic.message);
      }
      continue;
    }
    if (strcmp(name, "--listen") == 0) {
      NEED_VALUE();
      if (listen_seen ||
          !proxy_endpoint(value, options->listen_host,
                          sizeof(options->listen_host), options->listen_port))
        return proxy_error(error, error_size, "invalid or duplicate --listen");
      listen_seen = true;
    } else if (strcmp(name, "--origin") == 0) {
      NEED_VALUE();
      if (origin_seen || !proxy_origin(value, options))
        return proxy_error(error, error_size, "invalid or duplicate --origin");
      origin_seen = true;
    } else if (strcmp(name, "--javascript-inline-limit") == 0) {
      char *end = NULL;
      unsigned long limit;
      NEED_VALUE();
      limit = strtoul(value, &end, 10);
      if (javascript_inline_limit_seen || end == value || *end != '\0' ||
          limit > 65536U)
        return proxy_error(error, error_size,
                           "invalid or duplicate --javascript-inline-limit");
      options->config.javascript_inline_limit = (unsigned int)limit;
      javascript_inline_limit_seen = true;
    } else if (strcmp(name, "--javascript-outline-threshold") == 0) {
      char *end = NULL;
      unsigned long threshold;
      NEED_VALUE();
      threshold = strtoul(value, &end, 10);
      if (javascript_outline_threshold_seen || end == value || *end != '\0' ||
          threshold < 1024U || threshold > 1048576U)
        return proxy_error(
            error, error_size,
            "invalid or duplicate --javascript-outline-threshold");
      options->config.javascript_outline_threshold = (unsigned int)threshold;
      javascript_outline_threshold_seen = true;
    } else if (strcmp(name, "--cache-mime-types") == 0) {
      NEED_VALUE();
      if (options->config.cache_mime_types[0] != '\0' ||
          strlen(value) >= sizeof(options->config.cache_mime_types))
        return proxy_error(error, error_size,
                           "invalid or duplicate --cache-mime-types");
      (void)snprintf(options->config.cache_mime_types,
                     sizeof(options->config.cache_mime_types), "%s", value);
    } else if (strcmp(name, "--preset") == 0) {
      laghu_preset preset;
      NEED_VALUE();
      if (selector_seen || !laghu_parse_preset(value, &preset))
        return proxy_error(error, error_size,
                           "invalid or conflicting --preset");
      options->config.preset = preset;
      options->config.rewrite_level = LAGHU_REWRITE_LEVEL_UNSET;
      selector_seen = true;
    } else if (strcmp(name, "--rewrite-level") == 0) {
      laghu_rewrite_level level;
      NEED_VALUE();
      if (selector_seen || !laghu_parse_rewrite_level(value, &level))
        return proxy_error(error, error_size,
                           "invalid or conflicting --rewrite-level");
      options->config.preset = LAGHU_PRESET_UNSET;
      options->config.rewrite_level = level;
      selector_seen = true;
    } else if (strcmp(name, "--enable-filter") == 0 ||
               strcmp(name, "--disable-filter") == 0 ||
               strcmp(name, "--forbid-filter") == 0) {
      uint32_t filter;
      uint32_t declared;
      NEED_VALUE();
      declared = options->config.enabled_filters |
                 options->config.disabled_filters |
                 options->config.forbidden_filters;
      if (!laghu_parse_filter(value, &filter))
        return proxy_error(error, error_size, "unknown filter name");
      if ((declared & filter) != 0U)
        return proxy_error(error, error_size,
                           "duplicate or conflicting filter control");
      if (strcmp(name, "--enable-filter") == 0)
        options->config.enabled_filters |= filter;
      else if (strcmp(name, "--disable-filter") == 0)
        options->config.disabled_filters |= filter;
      else
        options->config.forbidden_filters |= filter;
    } else if (strcmp(name, "--allow-api") == 0) {
      if (options->config.allow_api == LAGHU_MODE_ON)
        return proxy_error(error, error_size, "duplicate --allow-api");
      options->config.allow_api = LAGHU_MODE_ON;
    } else if (strcmp(name, "--allow-resources") == 0 ||
               strcmp(name, "--disallow") == 0) {
      NEED_VALUE();
      if (!laghu_resource_rule_add(
              &options->config, strcmp(name, "--allow-resources") == 0, value))
        return proxy_error(
            error, error_size,
            "invalid, duplicate, conflicting, or excessive resource rule");
    } else if (strcmp(name, "--respect-vary") == 0 ||
               strcmp(name, "--respect-x-forwarded-proto") == 0 ||
               strcmp(name, "--query-filter-overrides") == 0) {
      bool *seen = strcmp(name, "--respect-vary") == 0 ? &respect_vary_seen
                   : strcmp(name, "--respect-x-forwarded-proto") == 0
                       ? &respect_proto_seen
                       : &query_overrides_seen;
      laghu_mode *target = strcmp(name, "--respect-vary") == 0
                               ? &options->config.respect_vary
                           : strcmp(name, "--respect-x-forwarded-proto") == 0
                               ? &options->config.respect_x_forwarded_proto
                               : &options->config.query_filter_overrides;
      NEED_VALUE();
      if (*seen || (strcmp(value, "on") != 0 && strcmp(value, "off") != 0))
        return proxy_error(error, error_size,
                           "request policy toggle expects on or off once");
      *target = strcmp(value, "on") == 0 ? LAGHU_MODE_ON : LAGHU_MODE_OFF;
      *seen = true;
    } else if (strcmp(name, "--image-beacon") == 0) {
      if (options->config.image_beacon == LAGHU_MODE_ON)
        return proxy_error(error, error_size, "duplicate --image-beacon");
      options->config.image_beacon = LAGHU_MODE_ON;
    } else if (strcmp(name, "--critical-css-beacon") == 0) {
      if (options->config.critical_css_beacon == LAGHU_MODE_ON)
        return proxy_error(error, error_size,
                           "duplicate --critical-css-beacon");
      options->config.critical_css_beacon = LAGHU_MODE_ON;
    } else if (strcmp(name, "--instrumentation-beacon") == 0) {
      if (options->config.instrumentation_beacon == LAGHU_MODE_ON)
        return proxy_error(error, error_size,
                           "duplicate --instrumentation-beacon");
      options->config.instrumentation_beacon = LAGHU_MODE_ON;
    } else if (strcmp(name, "--instrumentation-sample-rate") == 0) {
      NEED_VALUE();
      if (instrumentation_sample_rate_seen ||
          !proxy_uint(value, 0U, 100U,
                      &options->config.instrumentation_sample_rate))
        return proxy_error(error, error_size,
                           "invalid --instrumentation-sample-rate");
      instrumentation_sample_rate_seen = true;
    } else if (strcmp(name, "--javascript-defer-suggestions") == 0) {
      NEED_VALUE();
      if (javascript_defer_suggestions_seen ||
          (strcmp(value, "on") != 0 && strcmp(value, "off") != 0))
        return proxy_error(error, error_size,
                           "invalid --javascript-defer-suggestions");
      options->config.javascript_defer_suggestions =
          strcmp(value, "on") == 0 ? LAGHU_MODE_ON : LAGHU_MODE_OFF;
      javascript_defer_suggestions_seen = true;
    } else if (strcmp(name, "--include-js-source-maps") == 0) {
      if (options->config.include_js_source_maps == LAGHU_MODE_ON)
        return proxy_error(error, error_size,
                           "duplicate --include-js-source-maps");
      options->config.include_js_source_maps = LAGHU_MODE_ON;
    } else if (strcmp(name, "--image-quality") == 0) {
      NEED_VALUE();
      if (quality_seen ||
          !proxy_uint(value, 1U, 100U, &options->config.image_quality))
        return proxy_error(error, error_size,
                           "invalid or duplicate --image-quality");
      quality_seen = true;
    } else if (strcmp(name, "--workers") == 0) {
      NEED_VALUE();
      if (workers_seen || !proxy_uint(value, 1U, 256U, &options->workers))
        return proxy_error(error, error_size, "invalid or duplicate --workers");
      workers_seen = true;
    } else if (strcmp(name, "--connection-queue") == 0) {
      NEED_VALUE();
      if (connection_queue_seen ||
          !proxy_uint(value, 1U, 65536U, &options->connection_queue))
        return proxy_error(error, error_size,
                           "invalid or duplicate --connection-queue");
      connection_queue_seen = true;
    } else if (strcmp(name, "--connect-timeout") == 0) {
      NEED_VALUE();
      if (connect_timeout_seen ||
          !proxy_uint(value, 1U, 300U, &options->connect_timeout))
        return proxy_error(error, error_size,
                           "invalid or duplicate --connect-timeout");
      connect_timeout_seen = true;
    } else if (strcmp(name, "--io-timeout") == 0) {
      NEED_VALUE();
      if (io_timeout_seen || !proxy_uint(value, 1U, 300U, &options->io_timeout))
        return proxy_error(error, error_size,
                           "invalid or duplicate --io-timeout");
      io_timeout_seen = true;
    } else if (strcmp(name, "--drain-timeout") == 0) {
      NEED_VALUE();
      if (drain_timeout_seen ||
          !proxy_uint(value, 1U, 300U, &options->drain_timeout))
        return proxy_error(error, error_size,
                           "invalid or duplicate --drain-timeout");
      drain_timeout_seen = true;
    } else if (strcmp(name, "--origin-ca-file") == 0) {
      NEED_VALUE();
      if (ca_seen || !proxy_copy(options->origin_ca_file,
                                 sizeof(options->origin_ca_file), value))
        return proxy_error(error, error_size,
                           "invalid or duplicate --origin-ca-file");
      ca_seen = true;
    } else if (strcmp(name, "--forwarded-headers") == 0) {
      NEED_VALUE();
      if (forwarded_seen)
        return proxy_error(error, error_size, "duplicate --forwarded-headers");
      if (strcmp(value, "off") == 0)
        options->forwarded_mode = LAGHU_PROXY_FORWARDED_OFF;
      else if (strcmp(value, "forwarded") == 0)
        options->forwarded_mode = LAGHU_PROXY_FORWARDED_STANDARD;
      else if (strcmp(value, "x-forwarded") == 0)
        options->forwarded_mode = LAGHU_PROXY_FORWARDED_X;
      else if (strcmp(value, "both") == 0)
        options->forwarded_mode = LAGHU_PROXY_FORWARDED_BOTH;
      else
        return proxy_error(error, error_size, "invalid --forwarded-headers");
      forwarded_seen = true;
    } else {
      return proxy_error(error, error_size, "unknown option");
    }
#undef NEED_VALUE
  }
  {
    laghu_config resolved;
    laghu_config_merge(&resolved, &options->config, &shared_config);
    options->config = resolved;
  }
  {
    laghu_service_finalize_options finalize_options = {
        .native_file_loading = false,
        .require_cache = true,
        .require_worker_queue = true,
        .require_admin_authorization = true,
        .respect_x_forwarded_proto =
            options->config.respect_x_forwarded_proto == LAGHU_MODE_ON};
    laghu_service_diagnostic diagnostic;
    if (!laghu_service_config_finalize(&options->service, &finalize_options,
                                       &diagnostic))
      return proxy_error(error, error_size, diagnostic.message);
  }
  if (!listen_seen || !origin_seen)
    return proxy_error(error, error_size,
                       "--listen, --origin, a file cache backend, and "
                       "--worker-queue are required");
  if (ca_seen && !options->origin_tls)
    return proxy_error(error, error_size,
                       "--origin-ca-file requires an https origin");
  if (options->service.trusted_proxy_count != 0U &&
      options->forwarded_mode == LAGHU_PROXY_FORWARDED_OFF &&
      options->config.respect_x_forwarded_proto != LAGHU_MODE_ON)
    return proxy_error(error, error_size,
                       "--trusted-proxy requires forwarded headers");
  {
    laghu_policy policy;
    char policy_error[160U];
    if (!laghu_resolve_config_policy_with_error(
            &options->config, &policy, policy_error, sizeof(policy_error)))
      return proxy_error(error, error_size, policy_error);
  }
  return LAGHU_PROXY_PARSE_OK;
}
