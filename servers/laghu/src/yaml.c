// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <yaml.h>

#include "laghu/proxy.h"
#include "server_internal.h"

#define LAGHU_YAML_ARGUMENTS 512U

typedef struct {
  char *values[LAGHU_YAML_ARGUMENTS];
  size_t count;
} laghu_yaml_arguments;

static laghu_proxy_parse_result yaml_error(char *error, size_t capacity, const char *message) {
  if (capacity != 0U) (void)snprintf(error, capacity, "%s", message);
  return LAGHU_PROXY_PARSE_ERROR;
}

static bool yaml_secure_file(const char *path) {
  struct stat status;
  return path != NULL && path[0] == '/' && lstat(path, &status) == 0 && S_ISREG(status.st_mode) && (status.st_mode & (S_IWGRP | S_IWOTH)) == 0U;
}

static const char *yaml_scalar(const yaml_document_t *document, int index) {
  yaml_node_t *node = yaml_document_get_node((yaml_document_t *)document, index);
  return node != NULL && node->type == YAML_SCALAR_NODE ? (const char *)node->data.scalar.value : NULL;
}

static bool yaml_add(laghu_yaml_arguments *arguments, const char *value) {
  size_t length;
  char *copy;
  if (value == NULL || arguments->count >= LAGHU_YAML_ARGUMENTS) return false;
  length = strlen(value);
  if (length == 0U || length >= LAGHU_RUNTIME_PATH_SIZE) return false;
  copy = malloc(length + 1U);
  if (copy == NULL) return false;
  memcpy(copy, value, length + 1U);
  arguments->values[arguments->count++] = copy;
  return true;
}

static void yaml_arguments_dispose(laghu_yaml_arguments *arguments) {
  size_t index;
  for (index = 0U; index < arguments->count; ++index) free(arguments->values[index]);
  memset(arguments, 0, sizeof(*arguments));
}

static bool yaml_option_name(const char *key, char output[128U]) {
  size_t index;
  if (key == NULL || key[0] == '\0' || strlen(key) + 3U > 128U) return false;
  output[0] = '-';
  output[1] = '-';
  for (index = 0U; key[index] != '\0'; ++index) {
    unsigned char value = (unsigned char)key[index];
    if ((value < 'a' || value > 'z') && (value < '0' || value > '9') && value != '_' && value != '-') return false;
    output[index + 2U] = value == '_' ? '-' : (char)value;
  }
  output[index + 2U] = '\0';
  return true;
}

static bool yaml_add_scalar_setting(laghu_yaml_arguments *arguments, const char *key, const char *value) {
  char option[128U];
  if (!yaml_option_name(key, option)) return false;
  if (!strcmp(option, "--allow-api") || !strcmp(option, "--image-beacon") || !strcmp(option, "--critical-css-beacon") ||
      !strcmp(option, "--instrumentation-beacon") || !strcmp(option, "--include-js-source-maps")) {
    return !strcmp(value, "false") || (!strcmp(value, "true") && yaml_add(arguments, option));
  }
  return yaml_add(arguments, option) && yaml_add(arguments, value);
}

static bool yaml_add_sequence_setting(const yaml_document_t *document, laghu_yaml_arguments *arguments, const char *key, yaml_node_t *sequence) {
  yaml_node_item_t *item;
  char option[128U];
  size_t count = (size_t)(sequence->data.sequence.items.top - sequence->data.sequence.items.start);
  if (!yaml_option_name(key, option) || count == 0U) return false;
  if (!strcmp(option, "--add-header")) {
    for (item = sequence->data.sequence.items.start; item < sequence->data.sequence.items.top; ++item) {
      yaml_node_t *pair = yaml_document_get_node((yaml_document_t *)document, *item);
      const char *first;
      const char *second;
      size_t pair_count;
      if (pair == NULL || pair->type != YAML_SEQUENCE_NODE) return false;
      pair_count = (size_t)(pair->data.sequence.items.top - pair->data.sequence.items.start);
      if (pair_count != 2U) return false;
      first = yaml_scalar(document, pair->data.sequence.items.start[0]);
      second = yaml_scalar(document, pair->data.sequence.items.start[1]);
      if (first == NULL || second == NULL || !yaml_add(arguments, option) || !yaml_add(arguments, first) || !yaml_add(arguments, second))
        return false;
    }
    return true;
  }
  if (!strcmp(option, "--map-rewrite-domain") || !strcmp(option, "--map-proxy-domain") || !strcmp(option, "--shard-domain")) {
    const char *first;
    const char *second;
    if (count != 2U) return false;
    first = yaml_scalar(document, sequence->data.sequence.items.start[0]);
    second = yaml_scalar(document, sequence->data.sequence.items.start[1]);
    return first != NULL && second != NULL && yaml_add(arguments, option) && yaml_add(arguments, first) && yaml_add(arguments, second);
  }
  for (item = sequence->data.sequence.items.start; item < sequence->data.sequence.items.top; ++item) {
    const char *value = yaml_scalar(document, *item);
    if (value == NULL || !yaml_add(arguments, option) || !yaml_add(arguments, value)) return false;
  }
  return true;
}

static bool yaml_runtime(const yaml_document_t *document, laghu_yaml_arguments *arguments, yaml_node_t *mapping) {
  yaml_node_pair_t *pair;
  for (pair = mapping->data.mapping.pairs.start; pair < mapping->data.mapping.pairs.top; ++pair) {
    const char *key = yaml_scalar(document, pair->key);
    yaml_node_t *value = yaml_document_get_node((yaml_document_t *)document, pair->value);
    if (key == NULL || value == NULL) return false;
    if (value->type == YAML_SCALAR_NODE) {
      if (!yaml_add_scalar_setting(arguments, key, (const char *)value->data.scalar.value)) return false;
    } else if (value->type == YAML_SEQUENCE_NODE) {
      if (!yaml_add_sequence_setting(document, arguments, key, value)) return false;
    } else {
      return false;
    }
  }
  return true;
}

/* Scoped settings deliberately use the shared setting parsers.  That keeps
 * validation and inheritance semantics identical to the native adapters,
 * while YAML only owns shape validation. */
static bool yaml_rule_key(const char *key) {
  return !strcmp(key, "spa_fallback") || !strcmp(key, "request_header_limit") || !strcmp(key, "request_body_limit") ||
         !strcmp(key, "static_max_bytes") || !strcmp(key, "compression") || !strcmp(key, "allow") || !strcmp(key, "deny") ||
         !strcmp(key, "basic_auth_file") || !strcmp(key, "basic_auth_realm") || !strcmp(key, "rate_limit") || !strcmp(key, "rate_burst");
}

static bool yaml_scope_value(const yaml_document_t *document, const char *key, yaml_node_t *value, laghu_config *core, laghu_service_config *service,
                             laghu_proxy_rules *rules) {
  laghu_config_setting core_setting = laghu_config_setting_find(key);
  laghu_service_setting service_setting = laghu_service_setting_find(key);
  laghu_service_diagnostic service_error = {0};
  char core_error[160U];
  yaml_node_item_t *item;
  size_t count;
  if (yaml_rule_key(key)) {
    if (value->type == YAML_SCALAR_NODE)
      return laghu_proxy_rules_apply(rules, key, (const char *)value->data.scalar.value, core_error, sizeof(core_error));
    if ((strcmp(key, "allow") != 0 && strcmp(key, "deny") != 0) || value->type != YAML_SEQUENCE_NODE) return false;
    count = (size_t)(value->data.sequence.items.top - value->data.sequence.items.start);
    if (count == 0U) return false;
    for (item = value->data.sequence.items.start; item < value->data.sequence.items.top; ++item) {
      const char *scalar = yaml_scalar(document, *item);
      if (scalar == NULL || !laghu_proxy_rules_apply(rules, key, scalar, core_error, sizeof(core_error))) return false;
    }
    return true;
  }
  if (!strcmp(key, "mode")) {
    const char *mode = value->type == YAML_SCALAR_NODE ? (const char *)value->data.scalar.value : NULL;
    if (mode == NULL || core->mode != LAGHU_MODE_UNSET || (strcmp(mode, "on") != 0 && strcmp(mode, "off") != 0)) return false;
    core->mode = strcmp(mode, "on") == 0 ? LAGHU_MODE_ON : LAGHU_MODE_OFF;
    return true;
  }
  if ((core_setting == LAGHU_CONFIG_SETTING_UNKNOWN && service_setting == LAGHU_SERVICE_SETTING_UNKNOWN) ||
      (core_setting != LAGHU_CONFIG_SETTING_UNKNOWN && service_setting != LAGHU_SERVICE_SETTING_UNKNOWN))
    return false;
  if (value->type == YAML_SCALAR_NODE) {
    const char *scalar = (const char *)value->data.scalar.value;
    if (core_setting != LAGHU_CONFIG_SETTING_UNKNOWN) return laghu_config_setting_apply(core, core_setting, scalar, core_error, sizeof(core_error));
    return laghu_service_config_apply(service, service_setting, scalar, &service_error);
  }
  if (value->type != YAML_SEQUENCE_NODE) return false;
  count = (size_t)(value->data.sequence.items.top - value->data.sequence.items.start);
  if (count == 0U) return false;
  if (core_setting == LAGHU_CONFIG_SETTING_MAP_REWRITE_DOMAIN || core_setting == LAGHU_CONFIG_SETTING_MAP_PROXY_DOMAIN ||
      core_setting == LAGHU_CONFIG_SETTING_SHARD_DOMAIN || service_setting == LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP) {
    const char *first;
    const char *second;
    if (count != 2U) return false;
    first = yaml_scalar(document, value->data.sequence.items.start[0]);
    second = yaml_scalar(document, value->data.sequence.items.start[1]);
    if (first == NULL || second == NULL) return false;
    return core_setting != LAGHU_CONFIG_SETTING_UNKNOWN
               ? laghu_config_setting_apply_pair(core, core_setting, first, second, core_error, sizeof(core_error))
               : laghu_service_config_apply_pair(service, service_setting, first, second, &service_error);
  }
  for (item = value->data.sequence.items.start; item < value->data.sequence.items.top; ++item) {
    const char *scalar = yaml_scalar(document, *item);
    if (scalar == NULL ||
        (core_setting != LAGHU_CONFIG_SETTING_UNKNOWN ? !laghu_config_setting_apply(core, core_setting, scalar, core_error, sizeof(core_error))
                                                      : !laghu_service_config_apply(service, service_setting, scalar, &service_error)))
      return false;
  }
  return true;
}

static bool yaml_scope(const yaml_document_t *document, yaml_node_t *mapping, laghu_config *core, laghu_service_config *service,
                       laghu_proxy_rules *rules) {
  yaml_node_pair_t *pair;
  if (mapping == NULL || mapping->type != YAML_MAPPING_NODE) return false;
  for (pair = mapping->data.mapping.pairs.start; pair < mapping->data.mapping.pairs.top; ++pair) {
    const char *key = yaml_scalar(document, pair->key);
    yaml_node_t *value = yaml_document_get_node((yaml_document_t *)document, pair->value);
    if (key == NULL || value == NULL || !yaml_scope_value(document, key, value, core, service, rules)) return false;
  }
  return true;
}

static bool yaml_site_scalar(const yaml_document_t *document, yaml_node_pair_t *pair, const char *name, char *output, size_t capacity) {
  const char *key = yaml_scalar(document, pair->key);
  const char *value = yaml_scalar(document, pair->value);
  size_t length;
  if (key == NULL || value == NULL || strcmp(key, name) != 0) return false;
  length = strlen(value);
  if (length == 0U || length >= capacity) return false;
  memcpy(output, value, length + 1U);
  return true;
}

static bool yaml_route_target(const char *value, laghu_proxy_upstream_protocol *protocol, laghu_proxy_upstream_target *target) {
  const char *authority;
  const char *suffix;
  const char *colon = NULL;
  unsigned long port;
  char *end = NULL;
  size_t host_length;
  if (value == NULL || protocol == NULL || target == NULL) return false;
  memset(target, 0, sizeof(*target));
  if (!strncmp(value, "http://", 7U)) {
    *protocol = LAGHU_PROXY_UPSTREAM_HTTP;
    authority = value + 7U;
    port = 80U;
  } else if (!strncmp(value, "https://", 8U)) {
    *protocol = LAGHU_PROXY_UPSTREAM_HTTP;
    authority = value + 8U;
    target->tls = true;
    port = 443U;
  } else if (!strncmp(value, "fastcgi://", 10U)) {
    *protocol = LAGHU_PROXY_UPSTREAM_FASTCGI;
    authority = value + 10U;
    port = 9000U;
  } else if (!strncmp(value, "uwsgi://", 8U)) {
    *protocol = LAGHU_PROXY_UPSTREAM_UWSGI;
    authority = value + 8U;
    port = 3031U;
  } else if (!strncmp(value, "scgi://", 7U)) {
    *protocol = LAGHU_PROXY_UPSTREAM_SCGI;
    authority = value + 7U;
    port = 4000U;
  } else {
    return false;
  }
  if (*authority == '\0' || strpbrk(authority, "/?#@") != NULL) return false;
  suffix = authority + strlen(authority);
  if (*authority == '[') {
    const char *closing = strchr(authority, ']');
    if (closing == NULL || (closing[1] != '\0' && closing[1] != ':')) return false;
    host_length = (size_t)(closing - authority - 1U);
    if (closing[1] == ':') colon = closing + 1U;
    if (host_length == 0U || host_length >= sizeof(target->host)) return false;
    memcpy(target->host, authority + 1U, host_length);
  } else {
    colon = strrchr(authority, ':');
    if (colon != NULL && strchr(authority, ':') != colon) return false;
    host_length = colon == NULL ? (size_t)(suffix - authority) : (size_t)(colon - authority);
    if (host_length == 0U || host_length >= sizeof(target->host)) return false;
    memcpy(target->host, authority, host_length);
  }
  if (strlen(authority) >= sizeof(target->authority)) return false;
  target->host[host_length] = '\0';
  if (colon != NULL) {
    port = strtoul(colon + 1U, &end, 10);
    if (end == colon + 1U || *end != '\0' || port == 0U || port > 65535U) return false;
  }
  (void)snprintf(target->port, sizeof(target->port), "%lu", port);
  (void)snprintf(target->authority, sizeof(target->authority), "%s", authority);
  return true;
}

static bool yaml_routes(const yaml_document_t *document, laghu_proxy_options *options, yaml_node_t *sequence, size_t site_index);

static bool yaml_sites(const yaml_document_t *document, laghu_proxy_options *options, yaml_node_t *sequence) {
  yaml_node_item_t *item;
  if (sequence->type != YAML_SEQUENCE_NODE || options->site_count != 0U) return false;
  for (item = sequence->data.sequence.items.start; item < sequence->data.sequence.items.top; ++item) {
    yaml_node_t *mapping = yaml_document_get_node((yaml_document_t *)document, *item);
    yaml_node_pair_t *pair;
    laghu_proxy_site *site;
    bool host_seen = false, root_seen = false, index_seen = false, certificate_seen = false, key_seen = false;
    bool laghu_seen = false, service_seen = false, routes_seen = false;
    if (mapping == NULL || mapping->type != YAML_MAPPING_NODE || !proxy_options_append_site(options, &site)) return false;
    for (pair = mapping->data.mapping.pairs.start; pair < mapping->data.mapping.pairs.top; ++pair) {
      const char *key = yaml_scalar(document, pair->key);
      yaml_node_t *value = yaml_document_get_node((yaml_document_t *)document, pair->value);
      if (key == NULL) return false;
      if (!strcmp(key, "host")) {
        if (host_seen || !yaml_site_scalar(document, pair, "host", site->host, sizeof(site->host))) return false;
        host_seen = true;
      } else if (!strcmp(key, "document_root")) {
        if (root_seen || !yaml_site_scalar(document, pair, "document_root", site->document_root, sizeof(site->document_root)) ||
            site->document_root[0] != '/')
          return false;
        root_seen = true;
      } else if (!strcmp(key, "index")) {
        if (index_seen || !yaml_site_scalar(document, pair, "index", site->index_file, sizeof(site->index_file)) ||
            strchr(site->index_file, '/') != NULL || strstr(site->index_file, "..") != NULL)
          return false;
        index_seen = true;
      } else if (!strcmp(key, "tls_certificate")) {
        if (certificate_seen || !yaml_site_scalar(document, pair, "tls_certificate", site->tls_certificate, sizeof(site->tls_certificate)) ||
            site->tls_certificate[0] != '/')
          return false;
        certificate_seen = true;
      } else if (!strcmp(key, "tls_private_key")) {
        if (key_seen || !yaml_site_scalar(document, pair, "tls_private_key", site->tls_private_key, sizeof(site->tls_private_key)) ||
            site->tls_private_key[0] != '/')
          return false;
        key_seen = true;
      } else if (!strcmp(key, "laghu")) {
        if (laghu_seen || !yaml_scope(document, value, &site->config, &site->service, &site->rules)) return false;
        laghu_seen = true;
      } else if (!strcmp(key, "service")) {
        if (service_seen || !yaml_scope(document, value, &site->config, &site->service, &site->rules)) return false;
        service_seen = true;
      } else if (!strcmp(key, "routes")) {
        if (routes_seen || !yaml_routes(document, options, value, options->site_count - 1U)) return false;
        routes_seen = true;
      } else {
        return false;
      }
    }
    if (!host_seen || !root_seen || certificate_seen != key_seen || strpbrk(site->host, " \t\r\n:/\\")) return false;
    {
      size_t previous;
      for (previous = 0U; previous + 1U < options->site_count; ++previous)
        if (!strcasecmp(site->host, options->sites[previous].host)) return false;
    }
    if (!index_seen) (void)snprintf(site->index_file, sizeof(site->index_file), "%s", "index.html");
  }
  return options->site_count != 0U;
}

static bool yaml_routes(const yaml_document_t *document, laghu_proxy_options *options, yaml_node_t *sequence, size_t site_index) {
  yaml_node_item_t *item;
  if (sequence->type != YAML_SEQUENCE_NODE || (site_index == LAGHU_PROXY_SITE_GLOBAL && options->global_routes_seen)) return false;
  for (item = sequence->data.sequence.items.start; item < sequence->data.sequence.items.top; ++item) {
    yaml_node_t *mapping = yaml_document_get_node((yaml_document_t *)document, *item);
    yaml_node_pair_t *pair;
    laghu_proxy_route *route;
    bool match_seen = false, pattern_seen = false, redirect_seen = false, rewrite_seen = false, proxy_seen = false, status_seen = false;
    bool health_seen = false, health_interval_seen = false, failover_seen = false;
    laghu_proxy_upstream_protocol failover_protocol = LAGHU_PROXY_UPSTREAM_HTTP;
    bool laghu_seen = false, service_seen = false;
    if (mapping == NULL || mapping->type != YAML_MAPPING_NODE || !proxy_options_append_route(options, &route)) return false;
    route->status = 302U;
    route->site_index = site_index;
    for (pair = mapping->data.mapping.pairs.start; pair < mapping->data.mapping.pairs.top; ++pair) {
      const char *key = yaml_scalar(document, pair->key);
      yaml_node_t *node = yaml_document_get_node((yaml_document_t *)document, pair->value);
      const char *value = yaml_scalar(document, pair->value);
      char *end = NULL;
      unsigned long parsed;
      if (key == NULL || node == NULL) return false;
      if (!strcmp(key, "laghu")) {
        if (laghu_seen || !yaml_scope(document, node, &route->config, &route->service, &route->rules)) return false;
        laghu_seen = true;
        continue;
      }
      if (!strcmp(key, "service")) {
        if (service_seen || !yaml_scope(document, node, &route->config, &route->service, &route->rules)) return false;
        service_seen = true;
        continue;
      }
      if (!strcmp(key, "failover")) {
        yaml_node_item_t *item;
        size_t count;
        if (failover_seen || node->type != YAML_SEQUENCE_NODE) return false;
        count = (size_t)(node->data.sequence.items.top - node->data.sequence.items.start);
        if (count == 0U || count > LAGHU_PROXY_MAX_FAILOVERS) return false;
        for (item = node->data.sequence.items.start; item < node->data.sequence.items.top; ++item) {
          const char *candidate = yaml_scalar(document, *item);
          laghu_proxy_upstream_protocol protocol;
          if (candidate == NULL || !yaml_route_target(candidate, &protocol, &route->failovers[route->failover_count]) ||
              (route->failover_count != 0U && protocol != failover_protocol))
            return false;
          failover_protocol = protocol;
          ++route->failover_count;
        }
        failover_seen = true;
        continue;
      }
      if (value == NULL) return false;
      if (!strcmp(key, "match")) {
        if (match_seen) return false;
        if (!strcmp(value, "exact"))
          route->match = LAGHU_PROXY_ROUTE_EXACT;
        else if (!strcmp(value, "prefix"))
          route->match = LAGHU_PROXY_ROUTE_PREFIX;
        else if (!strcmp(value, "ordered_regex"))
          route->match = LAGHU_PROXY_ROUTE_ORDERED_REGEX;
        else
          return false;
        match_seen = true;
      } else if (!strcmp(key, "pattern")) {
        if (pattern_seen || !yaml_site_scalar(document, pair, "pattern", route->pattern, sizeof(route->pattern))) return false;
        pattern_seen = true;
      } else if (!strcmp(key, "redirect")) {
        if (redirect_seen || !yaml_site_scalar(document, pair, "redirect", route->redirect, sizeof(route->redirect)) || route->redirect[0] != '/')
          return false;
        redirect_seen = true;
      } else if (!strcmp(key, "rewrite")) {
        if (rewrite_seen || !yaml_site_scalar(document, pair, "rewrite", route->rewrite, sizeof(route->rewrite)) || route->rewrite[0] != '/')
          return false;
        rewrite_seen = true;
      } else if (!strcmp(key, "proxy_pass")) {
        laghu_proxy_upstream_target target;
        if (proxy_seen || !yaml_route_target(value, &route->upstream_protocol, &target)) return false;
        (void)snprintf(route->upstream_host, sizeof(route->upstream_host), "%s", target.host);
        (void)snprintf(route->upstream_port, sizeof(route->upstream_port), "%s", target.port);
        (void)snprintf(route->upstream_authority, sizeof(route->upstream_authority), "%s", target.authority);
        route->upstream_tls = target.tls;
        proxy_seen = true;
      } else if (!strcmp(key, "health_check")) {
        if (health_seen || !yaml_site_scalar(document, pair, "health_check", route->health_path, sizeof(route->health_path)) ||
            route->health_path[0] != '/' || strpbrk(route->health_path, "\r\n"))
          return false;
        health_seen = true;
      } else if (!strcmp(key, "health_interval")) {
        if (health_interval_seen || !yaml_site_scalar(document, pair, "health_interval", (char[32]){0}, 32U)) return false;
        parsed = strtoul(value, &end, 10);
        if (end == value || *end != '\0' || parsed == 0U || parsed > 3600U) return false;
        route->health_interval = (unsigned int)parsed;
        health_interval_seen = true;
      } else if (!strcmp(key, "status")) {
        parsed = strtoul(value, &end, 10);
        if (status_seen || end == value || *end != '\0' || (parsed != 301U && parsed != 302U && parsed != 307U && parsed != 308U)) return false;
        route->status = (unsigned int)parsed;
        status_seen = true;
      } else if (!strcmp(key, "response_header_name")) {
        if (route->response_header_name[0] != '\0' ||
            !yaml_site_scalar(document, pair, "response_header_name", route->response_header_name, sizeof(route->response_header_name)) ||
            strpbrk(route->response_header_name, "\r\n:"))
          return false;
      } else if (!strcmp(key, "response_header_value")) {
        if (route->response_header_value[0] != '\0' ||
            !yaml_site_scalar(document, pair, "response_header_value", route->response_header_value, sizeof(route->response_header_value)) ||
            strpbrk(route->response_header_value, "\r\n"))
          return false;
      } else
        return false;
    }
    if (!match_seen || !pattern_seen || (unsigned int)redirect_seen + (unsigned int)rewrite_seen + (unsigned int)proxy_seen != 1U ||
        (route->match != LAGHU_PROXY_ROUTE_ORDERED_REGEX && route->pattern[0] != '/') ||
        (route->response_header_name[0] == '\0') != (route->response_header_value[0] == '\0') ||
        (failover_seen && (!proxy_seen || route->upstream_protocol != failover_protocol)) || (health_interval_seen && !health_seen) ||
        ((health_seen || health_interval_seen) && (!proxy_seen || route->upstream_protocol != LAGHU_PROXY_UPSTREAM_HTTP)))
      return false;
    if (health_seen && !health_interval_seen) route->health_interval = 5U;
    if (route->match == LAGHU_PROXY_ROUTE_ORDERED_REGEX) {
      regex_t expression;
      int compiled = regcomp(&expression, route->pattern, REG_EXTENDED | REG_NOSUB);
      if (compiled != 0) return false;
      regfree(&expression);
    }
  }
  if (site_index == LAGHU_PROXY_SITE_GLOBAL) options->global_routes_seen = true;
  return options->route_count != 0U;
}

static bool yaml_load_file(const char *path, laghu_yaml_arguments *arguments, laghu_proxy_options *options, char *error, size_t error_size) {
  FILE *file = NULL;
  yaml_parser_t parser;
  yaml_document_t document;
  yaml_node_t *root;
  yaml_node_pair_t *pair;
  bool runtime_seen = false;
  bool sites_seen = false;
  bool routes_seen = false;
  bool valid = false;
  if (!yaml_secure_file(path)) {
    (void)yaml_error(error, error_size, "invalid YAML configuration file");
    return false;
  }
  file = fopen(path, "rb");
  if (file == NULL) {
    (void)yaml_error(error, error_size, "cannot open YAML configuration file");
    return false;
  }
  if (!yaml_parser_initialize(&parser)) goto done;
  yaml_parser_set_input_file(&parser, file);
  if (!yaml_parser_load(&parser, &document)) {
    if (parser.problem != NULL) (void)yaml_error(error, error_size, parser.problem);
    yaml_parser_delete(&parser);
    goto done;
  }
  yaml_parser_delete(&parser);
  root = yaml_document_get_root_node(&document);
  if (root == NULL || root->type != YAML_MAPPING_NODE) goto document_done;
  for (pair = root->data.mapping.pairs.start; pair < root->data.mapping.pairs.top; ++pair) {
    const char *key = yaml_scalar(&document, pair->key);
    yaml_node_t *value = yaml_document_get_node(&document, pair->value);
    if (key == NULL || value == NULL) goto document_done;
    if (!strcmp(key, "runtime")) {
      if (runtime_seen || value->type != YAML_MAPPING_NODE || !yaml_runtime(&document, arguments, value)) goto document_done;
      runtime_seen = true;
    } else if (!strcmp(key, "sites")) {
      if (sites_seen || !yaml_sites(&document, options, value)) goto document_done;
      sites_seen = true;
    } else if (!strcmp(key, "routes")) {
      if (routes_seen || !yaml_routes(&document, options, value, LAGHU_PROXY_SITE_GLOBAL)) goto document_done;
      routes_seen = true;
    } else {
      goto document_done;
    }
  }
  valid = runtime_seen;
document_done:
  yaml_document_delete(&document);
done:
  if (file != NULL) (void)fclose(file);
  if (!valid && error_size != 0U && error[0] == '\0') (void)snprintf(error, error_size, "invalid YAML configuration");
  return valid;
}

static bool yaml_load_fragments(const char *path, laghu_yaml_arguments *arguments, laghu_proxy_options *options, char *error, size_t error_size) {
  char pattern[PATH_MAX];
  const char *slash;
  glob_t matches;
  size_t index;
  int glob_result;
  if (path == NULL || (slash = strrchr(path, '/')) == NULL) return false;
  if ((size_t)(slash - path) + sizeof("/conf.d/*.yaml") >= sizeof(pattern)) return false;
  (void)snprintf(pattern, sizeof(pattern), "%.*s/conf.d/*.yaml", (int)(slash - path), path);
  memset(&matches, 0, sizeof(matches));
  glob_result = glob(pattern, 0, NULL, &matches);
  if (glob_result == GLOB_NOMATCH) {
    globfree(&matches);
    return true;
  }
  if (glob_result != 0) {
    globfree(&matches);
    (void)yaml_error(error, error_size, "cannot read YAML configuration fragments");
    return false;
  }
  for (index = 0U; index < matches.gl_pathc; ++index) {
    if (!yaml_load_file(matches.gl_pathv[index], arguments, options, error, error_size)) {
      globfree(&matches);
      return false;
    }
  }
  globfree(&matches);
  return true;
}

static bool yaml_finalize_scope(const laghu_config *parent_core, const laghu_service_config *parent_service, laghu_config *core,
                                laghu_service_config *service, const laghu_proxy_rules *parent_rules, laghu_proxy_rules *rules, unsigned int scope_id,
                                char *error, size_t error_size) {
  laghu_config merged_core;
  laghu_service_config merged_service;
  laghu_proxy_rules merged_rules;
  laghu_service_diagnostic diagnostic = {0};
  laghu_policy policy;
  char policy_error[160U] = {0};
  proxy_lifecycle_requirements requirements = {0};
  laghu_service_finalize_options finalize_options = {.native_file_loading = false, .require_admin_authorization = true};
  if (!laghu_resource_rules_merge_valid(parent_core, core) || !laghu_domain_policy_merge_valid(&parent_core->domain_policy, &core->domain_policy)) {
    (void)yaml_error(error, error_size, "invalid inherited Laghu policy");
    return false;
  }
  laghu_config_merge(&merged_core, parent_core, core);
  finalize_options.respect_x_forwarded_proto = merged_core.respect_x_forwarded_proto == LAGHU_MODE_ON;
  if (!laghu_service_config_merge(&merged_service, parent_service, service, &diagnostic)) {
    laghu_config_dispose(&merged_core);
    (void)yaml_error(error, error_size, diagnostic.message);
    return false;
  }
  if (!laghu_proxy_rules_merge(&merged_rules, parent_rules, rules, error, error_size)) {
    laghu_config_dispose(&merged_core);
    laghu_service_config_dispose(&merged_service);
    return false;
  }
  merged_rules.scope_id = scope_id;
  if (!laghu_resolve_config_policy_with_error(&merged_core, &policy, policy_error, sizeof(policy_error)) ||
      !proxy_scope_lifecycle_requirements(&merged_core, &merged_service, &requirements)) {
    laghu_config_dispose(&merged_core);
    laghu_service_config_dispose(&merged_service);
    (void)yaml_error(error, error_size, policy_error[0] != '\0' ? policy_error : "invalid lifecycle policy");
    return false;
  }
  finalize_options.require_cache = requirements.cache;
  finalize_options.require_worker_queue = requirements.image_queue;
  if (!laghu_service_config_finalize(&merged_service, &finalize_options, &diagnostic)) {
    laghu_config_dispose(&merged_core);
    laghu_service_config_dispose(&merged_service);
    (void)yaml_error(error, error_size, diagnostic.message);
    return false;
  }
  laghu_config_dispose(core);
  laghu_service_config_dispose(service);
  *core = merged_core;
  *service = merged_service;
  *rules = merged_rules;
  return true;
}

static bool yaml_finalize_scopes(laghu_proxy_options *options, char *error, size_t error_size) {
  size_t index;
  for (index = 0U; index < options->site_count; ++index)
    if (!yaml_finalize_scope(&options->config, &options->service, &options->sites[index].config, &options->sites[index].service, &options->rules,
                             &options->sites[index].rules, (unsigned int)index + 1U, error, error_size))
      return false;
  for (index = 0U; index < options->route_count; ++index) {
    const laghu_config *parent_core = &options->config;
    const laghu_service_config *parent_service = &options->service;
    const laghu_proxy_rules *parent_rules = &options->rules;
    unsigned int scope_id = 1000U + (unsigned int)index;
    if (options->routes[index].site_index != LAGHU_PROXY_SITE_GLOBAL) {
      if (options->routes[index].site_index >= options->site_count) return false;
      parent_core = &options->sites[options->routes[index].site_index].config;
      parent_service = &options->sites[options->routes[index].site_index].service;
      parent_rules = &options->sites[options->routes[index].site_index].rules;
    }
    if (!yaml_finalize_scope(parent_core, parent_service, &options->routes[index].config, &options->routes[index].service, parent_rules,
                             &options->routes[index].rules, scope_id, error, error_size))
      return false;
  }
  options->rules.scope_id = 0U;
  for (index = 0U; index < options->site_count; ++index)
    if (options->sites[index].tls_certificate[0] != '\0') options->downstream_tls = true;
  return true;
}

laghu_proxy_parse_result laghu_proxy_load_yaml(const char *path, laghu_proxy_options *options, char *error, size_t error_size) {
  laghu_yaml_arguments arguments = {0};
  laghu_proxy_parse_result result = LAGHU_PROXY_PARSE_ERROR;
  if (error_size != 0U) error[0] = '\0';
  if (options == NULL || !yaml_add(&arguments, "laghu") || !yaml_load_file(path, &arguments, options, error, error_size) ||
      !yaml_load_fragments(path, &arguments, options, error, error_size)) {
    goto done;
  }
  result = laghu_proxy_parse_options((int)arguments.count, arguments.values, options, error, error_size);
  if (result == LAGHU_PROXY_PARSE_OK && !yaml_finalize_scopes(options, error, error_size)) result = LAGHU_PROXY_PARSE_ERROR;
done:
  yaml_arguments_dispose(&arguments);
  if (result != LAGHU_PROXY_PARSE_OK && error_size != 0U && error[0] == '\0') (void)snprintf(error, error_size, "invalid YAML configuration");
  return result;
}
