// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "laghu/proxy.h"
#include "laghu/types.h"

static bool proxy_uint(const char *value, unsigned int minimum, unsigned int maximum, unsigned int *output) {
  unsigned long parsed;
  char *end = NULL;
  if (value == NULL || *value == '\0') return false;
  errno = 0;
  parsed = strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < minimum || parsed > maximum) return false;
  *output = (unsigned int)parsed;
  return true;
}

static bool proxy_copy(char *output, size_t capacity, const char *value) {
  size_t length = value == NULL ? 0U : strlen(value);
  if (length == 0U || length >= capacity) return false;
  memcpy(output, value, length + 1U);
  return true;
}

static bool proxy_absolute_path(const char *value) { return value != NULL && value[0] == '/'; }

static bool proxy_has_tls_target(const laghu_proxy_options *options) {
  size_t route_index;
  if (options == NULL) return false;
  if (options->origin_tls) return true;
  for (route_index = 0U; route_index < options->route_count; ++route_index) {
    size_t failover_index;
    if (options->routes[route_index].upstream_tls) return true;
    for (failover_index = 0U; failover_index < options->routes[route_index].failover_count; ++failover_index)
      if (options->routes[route_index].failovers[failover_index].tls) return true;
  }
  return false;
}

static bool proxy_rule_name_equal(const char *left, const char *right) {
  if (left == NULL || right == NULL) return false;
  if (left[0] == '-' && left[1] == '-') left += 2U;
  while (*left != '\0' && *right != '\0') {
    if ((*left == '-' ? '_' : *left) != *right) return false;
    ++left;
    ++right;
  }
  return *left == '\0' && *right == '\0';
}

static bool proxy_rule_uint(const char *value, uint64_t minimum, uint64_t maximum, size_t *output) {
  uint64_t parsed = 0U;
  unsigned int multiplier = 1U;
  char *end = NULL;
  if (value == NULL || value[0] == '\0') return false;
  errno = 0;
  parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value) return false;
  if (*end != '\0') {
    if ((end[1] != '\0') || (*end != 'k' && *end != 'K' && *end != 'm' && *end != 'M' && *end != 'g' && *end != 'G')) return false;
    multiplier = *end == 'k' || *end == 'K' ? 1024U : (*end == 'm' || *end == 'M' ? 1024U * 1024U : 1024U * 1024U * 1024U);
  }
  if (parsed > UINT64_MAX / multiplier) return false;
  parsed *= multiplier;
  if (parsed < minimum || parsed > maximum || parsed > SIZE_MAX) return false;
  *output = (size_t)parsed;
  return true;
}

static bool proxy_rule_cidr_add(laghu_service_cidr *values, size_t *count, const char *value) {
  laghu_service_cidr parsed;
  size_t index;
  if (*count >= LAGHU_SERVICE_CONFIG_MAX_CIDRS || !laghu_service_cidr_parse(value, &parsed)) return false;
  for (index = 0U; index < *count; ++index)
    if (laghu_service_cidr_equal(&values[index], &parsed)) return false;
  values[(*count)++] = parsed;
  return true;
}

static bool proxy_rule_hex(const char *value, unsigned char output[32U]) {
  size_t index;
  if (value == NULL || strlen(value) != 64U) return false;
  for (index = 0U; index < 32U; ++index) {
    int high = isdigit((unsigned char)value[index * 2U])              ? value[index * 2U] - '0'
               : value[index * 2U] >= 'a' && value[index * 2U] <= 'f' ? value[index * 2U] - 'a' + 10
               : value[index * 2U] >= 'A' && value[index * 2U] <= 'F' ? value[index * 2U] - 'A' + 10
                                                                      : -1;
    int low = isdigit((unsigned char)value[index * 2U + 1U])                   ? value[index * 2U + 1U] - '0'
              : value[index * 2U + 1U] >= 'a' && value[index * 2U + 1U] <= 'f' ? value[index * 2U + 1U] - 'a' + 10
              : value[index * 2U + 1U] >= 'A' && value[index * 2U + 1U] <= 'F' ? value[index * 2U + 1U] - 'A' + 10
                                                                               : -1;
    if (high < 0 || low < 0) return false;
    output[index] = (unsigned char)((high << 4U) | low);
  }
  return true;
}

static bool proxy_rule_auth_load(laghu_proxy_rules *rules, const char *path) {
  struct stat listed;
  struct stat status;
  FILE *file = NULL;
  char line[512U];
  size_t count = 0U;
  int descriptor;
  if (!proxy_absolute_path(path) || lstat(path, &listed) != 0 || !S_ISREG(listed.st_mode) || listed.st_uid != geteuid() ||
      (listed.st_mode & (S_IRWXG | S_IRWXO)) != 0U)
    return false;
  descriptor = open(path, O_RDONLY);
  if (descriptor < 0) return false;
  if (fstat(descriptor, &status) != 0 || status.st_dev != listed.st_dev || status.st_ino != listed.st_ino || !S_ISREG(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & (S_IRWXG | S_IRWXO)) != 0U) {
    (void)close(descriptor);
    return false;
  }
  file = fdopen(descriptor, "rb");
  if (file == NULL) {
    (void)close(descriptor);
    return false;
  }
  while (fgets(line, sizeof(line), file) != NULL) {
    char *separator;
    char *hash;
    size_t length;
    size_t index;
    size_t username_length;
    if (count == LAGHU_PROXY_MAX_AUTH_USERS) goto failed;
    length = strlen(line);
    while (length != 0U && (line[length - 1U] == '\n' || line[length - 1U] == '\r')) line[--length] = '\0';
    if (length == 0U || line[0] == '#') continue;
    separator = strchr(line, ':');
    if (separator == NULL || separator == line || strncmp(separator + 1U, "sha256:", 7U) != 0) goto failed;
    *separator = '\0';
    hash = separator + 8U;
    username_length = strlen(line);
    if (username_length >= sizeof(rules->basic_auth_users[0].username) || !proxy_rule_hex(hash, rules->basic_auth_users[count].password_hash))
      goto failed;
    for (index = 0U; line[index] != '\0'; ++index)
      if ((unsigned char)line[index] <= 32U || line[index] == ':') goto failed;
    for (index = 0U; index < count; ++index)
      if (strcmp(rules->basic_auth_users[index].username, line) == 0) goto failed;
    memcpy(rules->basic_auth_users[count].username, line, username_length + 1U);
    ++count;
  }
  {
    bool read_failed = ferror(file) != 0;
    bool close_failed = fclose(file) != 0;
    if (read_failed || close_failed || count == 0U) return false;
  }
  rules->basic_auth_user_count = count;
  return true;
failed:
  (void)fclose(file);
  memset(rules->basic_auth_users, 0, sizeof(rules->basic_auth_users));
  rules->basic_auth_user_count = 0U;
  return false;
}

void laghu_proxy_rules_init(laghu_proxy_rules *rules) {
  if (rules == NULL) return;
  memset(rules, 0, sizeof(*rules));
  rules->header_limit = LAGHU_PROXY_HEADER_BYTES;
  rules->body_limit = LAGHU_PROXY_REQUEST_BODY_BYTES;
  rules->static_max_bytes = 64U * 1024U * 1024U;
  rules->compression = LAGHU_PROXY_COMPRESSION_OFF;
}

bool laghu_proxy_rules_apply(laghu_proxy_rules *rules, const char *name, const char *value, char *error, size_t error_size) {
  size_t parsed;
  if (error_size != 0U) error[0] = '\0';
  if (rules == NULL || name == NULL || value == NULL) goto invalid;
  if (proxy_rule_name_equal(name, "spa_fallback")) {
    if (!proxy_absolute_path(value) || strchr(value, '?') != NULL || strchr(value, '#') != NULL || strstr(value, "..") != NULL ||
        !proxy_copy(rules->spa_fallback, sizeof(rules->spa_fallback), value))
      goto invalid;
    rules->present |= LAGHU_PROXY_RULE_SPA_FALLBACK;
  } else if (proxy_rule_name_equal(name, "request_header_limit")) {
    if (!proxy_rule_uint(value, 1024U, LAGHU_PROXY_HEADER_BYTES, &rules->header_limit)) goto invalid;
    rules->present |= LAGHU_PROXY_RULE_HEADER_LIMIT;
  } else if (proxy_rule_name_equal(name, "request_body_limit")) {
    if (!proxy_rule_uint(value, 0U, 64U * 1024U * 1024U, &rules->body_limit)) goto invalid;
    rules->present |= LAGHU_PROXY_RULE_BODY_LIMIT;
  } else if (proxy_rule_name_equal(name, "static_max_bytes")) {
    if (!proxy_rule_uint(value, 1U, 64U * 1024U * 1024U, &rules->static_max_bytes)) goto invalid;
    rules->present |= LAGHU_PROXY_RULE_STATIC_MAX;
  } else if (proxy_rule_name_equal(name, "compression")) {
    if (strcmp(value, "off") == 0)
      rules->compression = LAGHU_PROXY_COMPRESSION_OFF;
    else if (strcmp(value, "gzip") == 0)
      rules->compression = LAGHU_PROXY_COMPRESSION_GZIP;
    else
      goto invalid;
    rules->present |= LAGHU_PROXY_RULE_COMPRESSION;
  } else if (proxy_rule_name_equal(name, "allow")) {
    if ((rules->present & LAGHU_PROXY_RULE_ALLOW) == 0U) rules->allow_count = 0U;
    if (!proxy_rule_cidr_add(rules->allow, &rules->allow_count, value)) goto invalid;
    rules->present |= LAGHU_PROXY_RULE_ALLOW;
  } else if (proxy_rule_name_equal(name, "deny")) {
    if ((rules->present & LAGHU_PROXY_RULE_DENY) == 0U) rules->deny_count = 0U;
    if (!proxy_rule_cidr_add(rules->deny, &rules->deny_count, value)) goto invalid;
    rules->present |= LAGHU_PROXY_RULE_DENY;
  } else if (proxy_rule_name_equal(name, "basic_auth_file")) {
    if (!proxy_copy(rules->basic_auth_file, sizeof(rules->basic_auth_file), value) || !proxy_rule_auth_load(rules, value)) goto invalid;
    rules->present |= LAGHU_PROXY_RULE_BASIC_AUTH;
  } else if (proxy_rule_name_equal(name, "basic_auth_realm")) {
    if (strpbrk(value, "\r\n\"") != NULL || !proxy_copy(rules->basic_auth_realm, sizeof(rules->basic_auth_realm), value)) goto invalid;
    rules->present |= LAGHU_PROXY_RULE_BASIC_AUTH_REALM;
  } else if (proxy_rule_name_equal(name, "rate_limit")) {
    if (!proxy_rule_uint(value, 1U, 100000U, &parsed)) goto invalid;
    rules->rate_per_second = (unsigned int)parsed;
    rules->present |= LAGHU_PROXY_RULE_RATE;
  } else if (proxy_rule_name_equal(name, "rate_burst")) {
    if (!proxy_rule_uint(value, 1U, 100000U, &parsed)) goto invalid;
    rules->rate_burst = (unsigned int)parsed;
    rules->present |= LAGHU_PROXY_RULE_RATE;
  } else {
    return false;
  }
  return true;
invalid:
  if (error_size != 0U) (void)snprintf(error, error_size, "invalid standalone route rule");
  return false;
}

bool laghu_proxy_rules_merge(laghu_proxy_rules *merged, const laghu_proxy_rules *parent, const laghu_proxy_rules *child, char *error,
                             size_t error_size) {
  if (merged == NULL || parent == NULL || child == NULL) return false;
  *merged = *parent;
  if ((child->present & LAGHU_PROXY_RULE_SPA_FALLBACK) != 0U) memcpy(merged->spa_fallback, child->spa_fallback, sizeof(merged->spa_fallback));
  if ((child->present & LAGHU_PROXY_RULE_HEADER_LIMIT) != 0U) merged->header_limit = child->header_limit;
  if ((child->present & LAGHU_PROXY_RULE_BODY_LIMIT) != 0U) merged->body_limit = child->body_limit;
  if ((child->present & LAGHU_PROXY_RULE_STATIC_MAX) != 0U) merged->static_max_bytes = child->static_max_bytes;
  if ((child->present & LAGHU_PROXY_RULE_COMPRESSION) != 0U) merged->compression = child->compression;
  if ((child->present & LAGHU_PROXY_RULE_ALLOW) != 0U) {
    memcpy(merged->allow, child->allow, sizeof(merged->allow));
    merged->allow_count = child->allow_count;
  }
  if ((child->present & LAGHU_PROXY_RULE_DENY) != 0U) {
    memcpy(merged->deny, child->deny, sizeof(merged->deny));
    merged->deny_count = child->deny_count;
  }
  if ((child->present & LAGHU_PROXY_RULE_BASIC_AUTH) != 0U) {
    memcpy(merged->basic_auth_file, child->basic_auth_file, sizeof(merged->basic_auth_file));
    memcpy(merged->basic_auth_users, child->basic_auth_users, sizeof(merged->basic_auth_users));
    merged->basic_auth_user_count = child->basic_auth_user_count;
  }
  if ((child->present & LAGHU_PROXY_RULE_BASIC_AUTH_REALM) != 0U)
    memcpy(merged->basic_auth_realm, child->basic_auth_realm, sizeof(merged->basic_auth_realm));
  if ((child->present & LAGHU_PROXY_RULE_RATE) != 0U) {
    if (child->rate_per_second != 0U) merged->rate_per_second = child->rate_per_second;
    if (child->rate_burst != 0U) merged->rate_burst = child->rate_burst;
  }
  merged->present = parent->present | child->present;
  merged->scope_id = child->scope_id;
  if ((merged->present & LAGHU_PROXY_RULE_BASIC_AUTH_REALM) != 0U && (merged->present & LAGHU_PROXY_RULE_BASIC_AUTH) == 0U) goto invalid;
  if ((merged->present & LAGHU_PROXY_RULE_RATE) != 0U && (merged->rate_per_second == 0U || merged->rate_burst == 0U)) goto invalid;
  return true;
invalid:
  if (error_size != 0U) (void)snprintf(error, error_size, "invalid inherited standalone route rule");
  return false;
}

static bool proxy_endpoint(const char *value, char *host, size_t host_capacity, char port[6]) {
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
  if (host_length == 0U || host_length >= host_capacity || !proxy_uint(separator + 1, 1U, 65535U, &parsed_port)) return false;
  {
    size_t index;
    for (index = 0U; index < host_length; ++index)
      if ((unsigned char)value[index] <= 32U || value[index] == '/' || value[index] == '\\') return false;
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
    if (host_length == 0U || host_length >= sizeof(options->origin_host)) return false;
    memcpy(options->origin_host, authority + 1, host_length);
    options->origin_host[host_length] = '\0';
    if (end[1] == ':' && !proxy_uint(end + 2, 1U, 65535U, &port)) return false;
  } else if ((separator = strrchr(authority, ':')) != NULL) {
    size_t host_length = (size_t)(separator - authority);
    if (host_length == 0U || host_length >= sizeof(options->origin_host) || !proxy_uint(separator + 1, 1U, 65535U, &port)) return false;
    memcpy(options->origin_host, authority, host_length);
    options->origin_host[host_length] = '\0';
  } else if (!proxy_copy(options->origin_host, sizeof(options->origin_host), authority)) {
    return false;
  }
  {
    size_t index;
    for (index = 0U; options->origin_host[index] != '\0'; ++index)
      if ((unsigned char)options->origin_host[index] <= 32U || options->origin_host[index] == '\\') return false;
  }
  (void)snprintf(options->origin_port, sizeof(options->origin_port), "%u", port);
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
  options->origin_pool_size = LAGHU_PROXY_DEFAULT_ORIGIN_POOL_SIZE;
  options->origin_idle_timeout = LAGHU_PROXY_DEFAULT_ORIGIN_IDLE_TIMEOUT;
  options->access_log = true;
  (void)snprintf(options->index_file, sizeof(options->index_file), "%s", "index.html");
  laghu_service_config_init(&options->service);
  laghu_proxy_rules_init(&options->rules);
  {
    size_t index;
    for (index = 0U; index < LAGHU_PROXY_MAX_SITES; ++index) {
      laghu_config_init(&options->sites[index].config);
      laghu_service_config_init(&options->sites[index].service);
      laghu_proxy_rules_init(&options->sites[index].rules);
    }
    for (index = 0U; index < LAGHU_PROXY_MAX_ROUTES; ++index) {
      options->routes[index].site_index = LAGHU_PROXY_SITE_GLOBAL;
      laghu_config_init(&options->routes[index].config);
      laghu_service_config_init(&options->routes[index].service);
      laghu_proxy_rules_init(&options->routes[index].rules);
    }
  }
}

void laghu_proxy_options_dispose(laghu_proxy_options *options) {
  size_t index;
  if (options == NULL) return;
  laghu_service_config_dispose(&options->service);
  for (index = 0U; index < options->site_count; ++index) {
    SSL_CTX_free(options->sites[index].downstream_tls_context);
    options->sites[index].downstream_tls_context = NULL;
    laghu_service_config_dispose(&options->sites[index].service);
  }
  for (index = 0U; index < options->route_count; ++index) laghu_service_config_dispose(&options->routes[index].service);
}

static laghu_proxy_parse_result proxy_error(char *error, size_t capacity, const char *message) {
  if (capacity != 0U) (void)snprintf(error, capacity, "%s", message);
  return LAGHU_PROXY_PARSE_ERROR;
}

laghu_proxy_parse_result laghu_proxy_parse_options(int argc, char **argv, laghu_proxy_options *options, char *error, size_t error_size) {
  bool listen_seen = false, origin_seen = false, selector_seen = false;
  bool javascript_inline_limit_seen = false;
  bool javascript_outline_threshold_seen = false;
  bool instrumentation_sample_rate_seen = false;
  bool javascript_defer_suggestions_seen = false;
  bool quality_seen = false, workers_seen = false;
  bool connection_queue_seen = false, connect_timeout_seen = false;
  bool io_timeout_seen = false, drain_timeout_seen = false, origin_pool_size_seen = false, origin_idle_timeout_seen = false;
  bool ca_seen = false, tls_certificate_seen = false, tls_private_key_seen = false, pid_file_seen = false, forwarded_seen = false;
  bool document_root_seen = false, index_seen = false;
  bool directory_listing_seen = false, access_log_seen = false;
  bool static_cache_control_seen = false;
  bool respect_vary_seen = false, respect_proto_seen = false;
  bool query_overrides_seen = false;
  laghu_config shared_config;
  int index;
  if (options == NULL || argc < 1) return proxy_error(error, error_size, "invalid arguments");
  laghu_config_init(&shared_config);
  for (index = 1; index < argc; ++index) {
    const char *name = argv[index];
    const char *value = index + 1 < argc ? argv[index + 1] : NULL;
    laghu_config_setting shared_setting = laghu_config_setting_find(name);
    laghu_service_setting service_setting = laghu_service_setting_find(name);
    laghu_service_diagnostic service_diagnostic;
#define NEED_VALUE()                                                                                        \
  do {                                                                                                      \
    if (value == NULL || value[0] == '-') return proxy_error(error, error_size, "option requires a value"); \
    ++index;                                                                                                \
  } while (0)
    if (strcmp(name, "--help") == 0) return LAGHU_PROXY_PARSE_HELP;
    if (strcmp(name, "--version") == 0) return LAGHU_PROXY_PARSE_VERSION;
    if (shared_setting != LAGHU_CONFIG_SETTING_UNKNOWN) {
      if (shared_setting == LAGHU_CONFIG_SETTING_MAP_REWRITE_DOMAIN || shared_setting == LAGHU_CONFIG_SETTING_MAP_PROXY_DOMAIN ||
          shared_setting == LAGHU_CONFIG_SETTING_SHARD_DOMAIN) {
        const char *second = index + 2 < argc ? argv[index + 2] : NULL;
        if (value == NULL || second == NULL || value[0] == '-' || second[0] == '-' ||
            !laghu_config_setting_apply_pair(&shared_config, shared_setting, value, second, error, error_size))
          return LAGHU_PROXY_PARSE_ERROR;
        index += 2;
        continue;
      }
      bool implicit_on = shared_setting == LAGHU_CONFIG_SETTING_ALLOW_API || shared_setting == LAGHU_CONFIG_SETTING_IMAGE_BEACON ||
                         shared_setting == LAGHU_CONFIG_SETTING_CRITICAL_CSS_BEACON ||
                         shared_setting == LAGHU_CONFIG_SETTING_INSTRUMENTATION_BEACON ||
                         shared_setting == LAGHU_CONFIG_SETTING_INCLUDE_JS_SOURCE_MAPS;
      const char *shared_value = implicit_on ? "on" : value;
      if (!implicit_on) NEED_VALUE();
      if (!laghu_config_setting_apply(&shared_config, shared_setting, shared_value, error, error_size)) return LAGHU_PROXY_PARSE_ERROR;
      continue;
    }
    if (service_setting != LAGHU_SERVICE_SETTING_UNKNOWN) {
      const char *service_value = value;
      if (service_setting == LAGHU_SERVICE_SETTING_RUM_STORE_REQUIRED) {
        service_value = "on";
      } else {
        NEED_VALUE();
      }
      if (service_setting == LAGHU_SERVICE_SETTING_PURGE_METHOD && strcmp(service_value, "PURGE") != 0)
        return proxy_error(error, error_size, "--purge-method accepts PURGE once");
      if ((service_setting == LAGHU_SERVICE_SETTING_PURGE_TOKEN_FILE || service_setting == LAGHU_SERVICE_SETTING_CACHE_FLUSH_FILE) &&
          !proxy_absolute_path(service_value))
        return proxy_error(error, error_size, "invalid absolute file");
      if (service_setting == LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP) {
        const char *separator = strchr(service_value, '=');
        char prefix[LAGHU_RUNTIME_PATH_SIZE];
        size_t prefix_length = separator == NULL ? 0U : (size_t)(separator - service_value);
        if (prefix_length == 0U || prefix_length >= sizeof(prefix) || separator[1] == '\0')
          return proxy_error(error, error_size, "invalid --file-source-map");
        memcpy(prefix, service_value, prefix_length);
        prefix[prefix_length] = '\0';
        if (!laghu_service_config_apply_pair(&options->service, service_setting, prefix, separator + 1U, &service_diagnostic))
          return proxy_error(error, error_size, service_diagnostic.message);
      } else if (!laghu_service_config_apply(&options->service, service_setting, service_value, &service_diagnostic)) {
        return proxy_error(error, error_size, service_diagnostic.message);
      }
      continue;
    }
    if (strcmp(name, "--listen") == 0) {
      NEED_VALUE();
      if (listen_seen || !proxy_endpoint(value, options->listen_host, sizeof(options->listen_host), options->listen_port))
        return proxy_error(error, error_size, "invalid or duplicate --listen");
      listen_seen = true;
    } else if (strcmp(name, "--origin") == 0) {
      NEED_VALUE();
      if (origin_seen || !proxy_origin(value, options)) return proxy_error(error, error_size, "invalid or duplicate --origin");
      origin_seen = true;
    } else if (strcmp(name, "--add-header") == 0) {
      const char *second = index + 2 < argc ? argv[index + 2] : NULL;
      laghu_proxy_response_header *header;
      size_t name_length;
      if (second == NULL || value == NULL || value[0] == '-' || second[0] == '-' ||
          options->response_header_count == LAGHU_PROXY_MAX_RESPONSE_HEADERS)
        return proxy_error(error, error_size, "invalid or excessive --add-header");
      name_length = strlen(value);
      if (name_length == 0U || name_length >= sizeof(header->name) || strpbrk(value, " \t\r\n:") || strlen(second) >= sizeof(header->value) ||
          strpbrk(second, "\r\n"))
        return proxy_error(error, error_size, "invalid --add-header");
      header = &options->response_headers[options->response_header_count++];
      (void)snprintf(header->name, sizeof(header->name), "%s", value);
      (void)snprintf(header->value, sizeof(header->value), "%s", second);
      index += 2;
    } else if (strcmp(name, "--document-root") == 0) {
      NEED_VALUE();
      if (document_root_seen || !proxy_absolute_path(value) || !proxy_copy(options->document_root, sizeof(options->document_root), value))
        return proxy_error(error, error_size, "invalid or duplicate --document-root");
      document_root_seen = true;
    } else if (strcmp(name, "--index") == 0) {
      size_t value_length;
      NEED_VALUE();
      value_length = strlen(value);
      if (index_seen || value_length == 0U || value_length >= sizeof(options->index_file) || strchr(value, '/') != NULL ||
          strstr(value, "..") != NULL)
        return proxy_error(error, error_size, "invalid or duplicate --index");
      (void)snprintf(options->index_file, sizeof(options->index_file), "%s", value);
      index_seen = true;
    } else if (strcmp(name, "--directory-listing") == 0) {
      NEED_VALUE();
      if (directory_listing_seen || (strcmp(value, "on") != 0 && strcmp(value, "off") != 0))
        return proxy_error(error, error_size, "invalid or duplicate --directory-listing");
      options->directory_listing = strcmp(value, "on") == 0;
      directory_listing_seen = true;
    } else if (strcmp(name, "--access-log") == 0) {
      NEED_VALUE();
      if (access_log_seen || (strcmp(value, "on") != 0 && strcmp(value, "off") != 0))
        return proxy_error(error, error_size, "invalid or duplicate --access-log");
      options->access_log = strcmp(value, "on") == 0;
      access_log_seen = true;
    } else if (strcmp(name, "--static-cache-control") == 0) {
      NEED_VALUE();
      if (static_cache_control_seen || strlen(value) >= sizeof(options->static_cache_control) || strpbrk(value, "\r\n"))
        return proxy_error(error, error_size, "invalid or duplicate --static-cache-control");
      (void)snprintf(options->static_cache_control, sizeof(options->static_cache_control), "%s", value);
      static_cache_control_seen = true;
    } else if (strcmp(name, "--spa-fallback") == 0 || strcmp(name, "--request-header-limit") == 0 || strcmp(name, "--request-body-limit") == 0 ||
               strcmp(name, "--static-max-bytes") == 0 || strcmp(name, "--compression") == 0 || strcmp(name, "--allow") == 0 ||
               strcmp(name, "--deny") == 0 || strcmp(name, "--basic-auth-file") == 0 || strcmp(name, "--basic-auth-realm") == 0 ||
               strcmp(name, "--rate-limit") == 0 || strcmp(name, "--rate-burst") == 0) {
      NEED_VALUE();
      if (!laghu_proxy_rules_apply(&options->rules, name, value, error, error_size))
        return error[0] == '\0' ? proxy_error(error, error_size, "invalid standalone route rule") : LAGHU_PROXY_PARSE_ERROR;
    } else if (strcmp(name, "--javascript-inline-limit") == 0) {
      char *end = NULL;
      unsigned long limit;
      NEED_VALUE();
      limit = strtoul(value, &end, 10);
      if (javascript_inline_limit_seen || end == value || *end != '\0' || limit > 65536U)
        return proxy_error(error, error_size, "invalid or duplicate --javascript-inline-limit");
      options->config.javascript_inline_limit = (unsigned int)limit;
      javascript_inline_limit_seen = true;
    } else if (strcmp(name, "--javascript-outline-threshold") == 0) {
      char *end = NULL;
      unsigned long threshold;
      NEED_VALUE();
      threshold = strtoul(value, &end, 10);
      if (javascript_outline_threshold_seen || end == value || *end != '\0' || threshold < 1024U || threshold > 1048576U)
        return proxy_error(error, error_size, "invalid or duplicate --javascript-outline-threshold");
      options->config.javascript_outline_threshold = (unsigned int)threshold;
      javascript_outline_threshold_seen = true;
    } else if (strcmp(name, "--cache-mime-types") == 0) {
      NEED_VALUE();
      if (options->config.cache_mime_types[0] != '\0' || strlen(value) >= sizeof(options->config.cache_mime_types))
        return proxy_error(error, error_size, "invalid or duplicate --cache-mime-types");
      (void)snprintf(options->config.cache_mime_types, sizeof(options->config.cache_mime_types), "%s", value);
    } else if (strcmp(name, "--preset") == 0) {
      laghu_preset preset;
      NEED_VALUE();
      if (selector_seen || !laghu_parse_preset(value, &preset)) return proxy_error(error, error_size, "invalid or conflicting --preset");
      options->config.preset = preset;
      options->config.rewrite_level = LAGHU_REWRITE_LEVEL_UNSET;
      selector_seen = true;
    } else if (strcmp(name, "--rewrite-level") == 0) {
      laghu_rewrite_level level;
      NEED_VALUE();
      if (selector_seen || !laghu_parse_rewrite_level(value, &level)) return proxy_error(error, error_size, "invalid or conflicting --rewrite-level");
      options->config.preset = LAGHU_PRESET_UNSET;
      options->config.rewrite_level = level;
      selector_seen = true;
    } else if (strcmp(name, "--enable-filter") == 0 || strcmp(name, "--disable-filter") == 0 || strcmp(name, "--forbid-filter") == 0) {
      uint32_t filter;
      uint32_t declared;
      NEED_VALUE();
      declared = options->config.enabled_filters | options->config.disabled_filters | options->config.forbidden_filters;
      if (!laghu_parse_filter(value, &filter)) return proxy_error(error, error_size, "unknown filter name");
      if ((declared & filter) != 0U) return proxy_error(error, error_size, "duplicate or conflicting filter control");
      if (strcmp(name, "--enable-filter") == 0)
        options->config.enabled_filters |= filter;
      else if (strcmp(name, "--disable-filter") == 0)
        options->config.disabled_filters |= filter;
      else
        options->config.forbidden_filters |= filter;
    } else if (strcmp(name, "--allow-api") == 0) {
      if (options->config.allow_api == LAGHU_MODE_ON) return proxy_error(error, error_size, "duplicate --allow-api");
      options->config.allow_api = LAGHU_MODE_ON;
    } else if (strcmp(name, "--allow-resources") == 0 || strcmp(name, "--disallow") == 0) {
      NEED_VALUE();
      if (!laghu_resource_rule_add(&options->config, strcmp(name, "--allow-resources") == 0, value))
        return proxy_error(error, error_size, "invalid, duplicate, conflicting, or excessive resource rule");
    } else if (strcmp(name, "--respect-vary") == 0 || strcmp(name, "--respect-x-forwarded-proto") == 0 ||
               strcmp(name, "--query-filter-overrides") == 0) {
      bool *seen = strcmp(name, "--respect-vary") == 0                ? &respect_vary_seen
                   : strcmp(name, "--respect-x-forwarded-proto") == 0 ? &respect_proto_seen
                                                                      : &query_overrides_seen;
      laghu_mode *target = strcmp(name, "--respect-vary") == 0                ? &options->config.respect_vary
                           : strcmp(name, "--respect-x-forwarded-proto") == 0 ? &options->config.respect_x_forwarded_proto
                                                                              : &options->config.query_filter_overrides;
      NEED_VALUE();
      if (*seen || (strcmp(value, "on") != 0 && strcmp(value, "off") != 0))
        return proxy_error(error, error_size, "request policy toggle expects on or off once");
      *target = strcmp(value, "on") == 0 ? LAGHU_MODE_ON : LAGHU_MODE_OFF;
      *seen = true;
    } else if (strcmp(name, "--image-beacon") == 0) {
      if (options->config.image_beacon == LAGHU_MODE_ON) return proxy_error(error, error_size, "duplicate --image-beacon");
      options->config.image_beacon = LAGHU_MODE_ON;
    } else if (strcmp(name, "--critical-css-beacon") == 0) {
      if (options->config.critical_css_beacon == LAGHU_MODE_ON) return proxy_error(error, error_size, "duplicate --critical-css-beacon");
      options->config.critical_css_beacon = LAGHU_MODE_ON;
    } else if (strcmp(name, "--instrumentation-beacon") == 0) {
      if (options->config.instrumentation_beacon == LAGHU_MODE_ON) return proxy_error(error, error_size, "duplicate --instrumentation-beacon");
      options->config.instrumentation_beacon = LAGHU_MODE_ON;
    } else if (strcmp(name, "--instrumentation-sample-rate") == 0) {
      NEED_VALUE();
      if (instrumentation_sample_rate_seen || !proxy_uint(value, 0U, 100U, &options->config.instrumentation_sample_rate))
        return proxy_error(error, error_size, "invalid --instrumentation-sample-rate");
      instrumentation_sample_rate_seen = true;
    } else if (strcmp(name, "--optimization-profiles") == 0 || strcmp(name, "--javascript-defer-suggestions") == 0) {
      NEED_VALUE();
      if (strcmp(value, "on") != 0 && strcmp(value, "off") != 0)
        return proxy_error(error, error_size, "profile and JavaScript toggles expect on or off");
      if (strcmp(name, "--optimization-profiles") == 0) {
        if (options->config.optimization_profiles != LAGHU_MODE_UNSET) return proxy_error(error, error_size, "duplicate --optimization-profiles");
        options->config.optimization_profiles = strcmp(value, "on") == 0 ? LAGHU_MODE_ON : LAGHU_MODE_OFF;
      } else {
        if (javascript_defer_suggestions_seen) return proxy_error(error, error_size, "invalid --javascript-defer-suggestions");
        options->config.javascript_defer_suggestions = strcmp(value, "on") == 0 ? LAGHU_MODE_ON : LAGHU_MODE_OFF;
        javascript_defer_suggestions_seen = true;
      }
    } else if (strcmp(name, "--include-js-source-maps") == 0) {
      if (options->config.include_js_source_maps == LAGHU_MODE_ON) return proxy_error(error, error_size, "duplicate --include-js-source-maps");
      options->config.include_js_source_maps = LAGHU_MODE_ON;
    } else if (strcmp(name, "--image-quality") == 0) {
      NEED_VALUE();
      if (quality_seen || !proxy_uint(value, 1U, 100U, &options->config.image_quality))
        return proxy_error(error, error_size, "invalid or duplicate --image-quality");
      quality_seen = true;
    } else if (strcmp(name, "--workers") == 0) {
      NEED_VALUE();
      if (workers_seen || !proxy_uint(value, 1U, 256U, &options->workers)) return proxy_error(error, error_size, "invalid or duplicate --workers");
      workers_seen = true;
    } else if (strcmp(name, "--connection-queue") == 0) {
      NEED_VALUE();
      if (connection_queue_seen || !proxy_uint(value, 1U, 65536U, &options->connection_queue))
        return proxy_error(error, error_size, "invalid or duplicate --connection-queue");
      connection_queue_seen = true;
    } else if (strcmp(name, "--connect-timeout") == 0) {
      NEED_VALUE();
      if (connect_timeout_seen || !proxy_uint(value, 1U, 300U, &options->connect_timeout))
        return proxy_error(error, error_size, "invalid or duplicate --connect-timeout");
      connect_timeout_seen = true;
    } else if (strcmp(name, "--io-timeout") == 0) {
      NEED_VALUE();
      if (io_timeout_seen || !proxy_uint(value, 1U, 300U, &options->io_timeout))
        return proxy_error(error, error_size, "invalid or duplicate --io-timeout");
      io_timeout_seen = true;
    } else if (strcmp(name, "--drain-timeout") == 0) {
      NEED_VALUE();
      if (drain_timeout_seen || !proxy_uint(value, 1U, 300U, &options->drain_timeout))
        return proxy_error(error, error_size, "invalid or duplicate --drain-timeout");
      drain_timeout_seen = true;
    } else if (strcmp(name, "--origin-pool-size") == 0) {
      NEED_VALUE();
      if (origin_pool_size_seen || !proxy_uint(value, 0U, 1024U, &options->origin_pool_size))
        return proxy_error(error, error_size, "invalid or duplicate --origin-pool-size");
      origin_pool_size_seen = true;
    } else if (strcmp(name, "--origin-idle-timeout") == 0) {
      NEED_VALUE();
      if (origin_idle_timeout_seen || !proxy_uint(value, 1U, 3600U, &options->origin_idle_timeout))
        return proxy_error(error, error_size, "invalid or duplicate --origin-idle-timeout");
      origin_idle_timeout_seen = true;
    } else if (strcmp(name, "--origin-ca-file") == 0) {
      NEED_VALUE();
      if (ca_seen || !proxy_copy(options->origin_ca_file, sizeof(options->origin_ca_file), value))
        return proxy_error(error, error_size, "invalid or duplicate --origin-ca-file");
      ca_seen = true;
    } else if (strcmp(name, "--tls-certificate") == 0 || strcmp(name, "--tls-private-key") == 0) {
      bool *seen = strcmp(name, "--tls-certificate") == 0 ? &tls_certificate_seen : &tls_private_key_seen;
      char *target = strcmp(name, "--tls-certificate") == 0 ? options->tls_certificate : options->tls_private_key;
      size_t capacity = strcmp(name, "--tls-certificate") == 0 ? sizeof(options->tls_certificate) : sizeof(options->tls_private_key);
      NEED_VALUE();
      if (*seen || !proxy_absolute_path(value) || !proxy_copy(target, capacity, value))
        return proxy_error(error, error_size, "invalid or duplicate downstream TLS file");
      *seen = true;
    } else if (strcmp(name, "--pid-file") == 0) {
      NEED_VALUE();
      if (pid_file_seen || !proxy_absolute_path(value) || !proxy_copy(options->pid_file, sizeof(options->pid_file), value))
        return proxy_error(error, error_size, "invalid or duplicate --pid-file");
      pid_file_seen = true;
    } else if (strcmp(name, "--forwarded-headers") == 0) {
      NEED_VALUE();
      if (forwarded_seen) return proxy_error(error, error_size, "duplicate --forwarded-headers");
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
    laghu_service_finalize_options finalize_options = {.native_file_loading = false,
                                                       .require_cache = true,
                                                       .require_worker_queue = true,
                                                       .require_admin_authorization = true,
                                                       .respect_x_forwarded_proto = options->config.respect_x_forwarded_proto == LAGHU_MODE_ON};
    laghu_service_diagnostic diagnostic;
    if (!laghu_service_config_finalize(&options->service, &finalize_options, &diagnostic)) return proxy_error(error, error_size, diagnostic.message);
  }
  if (!listen_seen || (!origin_seen && options->document_root[0] == '\0' && options->site_count == 0U))
    return proxy_error(error, error_size,
                       options->config.rewrite_level == LAGHU_REWRITE_LEVEL_PASSTHROUGH
                           ? "--listen, a static document root or --origin, and a file cache backend are required"
                           : "--listen, a static document root or --origin, a file cache backend, and --worker-queue are required");
  if (ca_seen && !proxy_has_tls_target(options)) return proxy_error(error, error_size, "--origin-ca-file requires an https origin");
  if (tls_certificate_seen != tls_private_key_seen)
    return proxy_error(error, error_size, "--tls-certificate and --tls-private-key are required together");
  options->downstream_tls = tls_certificate_seen;
  if (options->service.trusted_proxy_count != 0U && options->forwarded_mode == LAGHU_PROXY_FORWARDED_OFF &&
      options->config.respect_x_forwarded_proto != LAGHU_MODE_ON)
    return proxy_error(error, error_size, "--trusted-proxy requires forwarded headers");
  {
    laghu_policy policy;
    char policy_error[160U];
    if (!laghu_resolve_config_policy_with_error(&options->config, &policy, policy_error, sizeof(policy_error)))
      return proxy_error(error, error_size, policy_error);
  }
  {
    laghu_proxy_rules empty;
    laghu_proxy_rules resolved;
    laghu_proxy_rules_init(&empty);
    if (!laghu_proxy_rules_merge(&resolved, &options->rules, &empty, error, error_size))
      return error[0] == '\0' ? proxy_error(error, error_size, "invalid standalone route rule") : LAGHU_PROXY_PARSE_ERROR;
    options->rules = resolved;
  }
  return LAGHU_PROXY_PARSE_OK;
}
