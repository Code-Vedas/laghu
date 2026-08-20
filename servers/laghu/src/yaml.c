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
  return path != NULL && path[0] == '/' && lstat(path, &status) == 0 && S_ISREG(status.st_mode) &&
         (status.st_mode & (S_IWGRP | S_IWOTH)) == 0U;
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
      if (first == NULL || second == NULL || !yaml_add(arguments, option) || !yaml_add(arguments, first) || !yaml_add(arguments, second)) return false;
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

static bool yaml_sites(const yaml_document_t *document, laghu_proxy_options *options, yaml_node_t *sequence) {
  yaml_node_item_t *item;
  if (sequence->type != YAML_SEQUENCE_NODE || options->site_count != 0U) return false;
  for (item = sequence->data.sequence.items.start; item < sequence->data.sequence.items.top; ++item) {
    yaml_node_t *mapping = yaml_document_get_node((yaml_document_t *)document, *item);
    yaml_node_pair_t *pair;
    laghu_proxy_site *site;
    bool host_seen = false, root_seen = false, index_seen = false;
    if (mapping == NULL || mapping->type != YAML_MAPPING_NODE || options->site_count == LAGHU_PROXY_MAX_SITES) return false;
    site = &options->sites[options->site_count];
    for (pair = mapping->data.mapping.pairs.start; pair < mapping->data.mapping.pairs.top; ++pair) {
      const char *key = yaml_scalar(document, pair->key);
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
      } else {
        return false;
      }
    }
    if (!host_seen || !root_seen) return false;
    if (!index_seen) (void)snprintf(site->index_file, sizeof(site->index_file), "%s", "index.html");
    ++options->site_count;
  }
  return options->site_count != 0U;
}

static bool yaml_routes(const yaml_document_t *document, laghu_proxy_options *options, yaml_node_t *sequence) {
  yaml_node_item_t *item;
  if (sequence->type != YAML_SEQUENCE_NODE || options->route_count != 0U) return false;
  for (item = sequence->data.sequence.items.start; item < sequence->data.sequence.items.top; ++item) {
    yaml_node_t *mapping = yaml_document_get_node((yaml_document_t *)document, *item);
    yaml_node_pair_t *pair;
    laghu_proxy_route *route;
    bool match_seen = false, pattern_seen = false, redirect_seen = false, rewrite_seen = false, proxy_seen = false, status_seen = false;
    if (mapping == NULL || mapping->type != YAML_MAPPING_NODE || options->route_count == LAGHU_PROXY_MAX_ROUTES) return false;
    route = &options->routes[options->route_count];
    route->status = 302U;
    for (pair = mapping->data.mapping.pairs.start; pair < mapping->data.mapping.pairs.top; ++pair) {
      const char *key = yaml_scalar(document, pair->key);
      const char *value = yaml_scalar(document, pair->value);
      char *end = NULL;
      unsigned long parsed;
      if (key == NULL || value == NULL) return false;
      if (!strcmp(key, "match")) {
        if (match_seen) return false;
        if (!strcmp(value, "exact")) route->match = LAGHU_PROXY_ROUTE_EXACT;
        else if (!strcmp(value, "prefix")) route->match = LAGHU_PROXY_ROUTE_PREFIX;
        else if (!strcmp(value, "ordered_regex")) route->match = LAGHU_PROXY_ROUTE_ORDERED_REGEX;
        else return false;
        match_seen = true;
      } else if (!strcmp(key, "pattern")) {
        if (pattern_seen || !yaml_site_scalar(document, pair, "pattern", route->pattern, sizeof(route->pattern)))
          return false;
        pattern_seen = true;
      } else if (!strcmp(key, "redirect")) {
        if (redirect_seen || !yaml_site_scalar(document, pair, "redirect", route->redirect, sizeof(route->redirect)) || route->redirect[0] != '/') return false;
        redirect_seen = true;
      } else if (!strcmp(key, "rewrite")) {
        if (rewrite_seen || !yaml_site_scalar(document, pair, "rewrite", route->rewrite, sizeof(route->rewrite)) || route->rewrite[0] != '/') return false;
        rewrite_seen = true;
      } else if (!strcmp(key, "proxy_pass")) {
        const char *authority;
        const char *colon;
        size_t host_length;
        unsigned long port = 80U;
        if (proxy_seen || strncmp(value, "http://", 7U) != 0 || strpbrk(value + 7U, "/?#@") != NULL) return false;
        authority = value + 7U;
        colon = strrchr(authority, ':');
        host_length = colon == NULL ? strlen(authority) : (size_t)(colon - authority);
        if (host_length == 0U || host_length >= sizeof(route->upstream_host) || strlen(authority) >= sizeof(route->upstream_authority)) return false;
        if (colon != NULL) {
          char *end = NULL;
          port = strtoul(colon + 1U, &end, 10);
          if (end == colon + 1U || *end != '\0' || port == 0U || port > 65535U) return false;
        }
        memcpy(route->upstream_host, authority, host_length);
        route->upstream_host[host_length] = '\0';
        (void)snprintf(route->upstream_port, sizeof(route->upstream_port), "%lu", port);
        (void)snprintf(route->upstream_authority, sizeof(route->upstream_authority), "%s", authority);
        proxy_seen = true;
      } else if (!strcmp(key, "status")) {
        parsed = strtoul(value, &end, 10);
        if (status_seen || end == value || *end != '\0' || (parsed != 301U && parsed != 302U && parsed != 307U && parsed != 308U)) return false;
        route->status = (unsigned int)parsed;
        status_seen = true;
      } else if (!strcmp(key, "response_header_name")) {
        if (route->response_header_name[0] != '\0' || !yaml_site_scalar(document, pair, "response_header_name", route->response_header_name,
                                                                          sizeof(route->response_header_name)) ||
            strpbrk(route->response_header_name, "\r\n:"))
          return false;
      } else if (!strcmp(key, "response_header_value")) {
        if (route->response_header_value[0] != '\0' || !yaml_site_scalar(document, pair, "response_header_value", route->response_header_value,
                                                                           sizeof(route->response_header_value)) ||
            strpbrk(route->response_header_value, "\r\n"))
          return false;
      } else return false;
    }
    if (!match_seen || !pattern_seen || (unsigned int)redirect_seen + (unsigned int)rewrite_seen + (unsigned int)proxy_seen != 1U ||
        (route->match != LAGHU_PROXY_ROUTE_ORDERED_REGEX && route->pattern[0] != '/') ||
        (route->response_header_name[0] == '\0') != (route->response_header_value[0] == '\0'))
      return false;
    if (route->match == LAGHU_PROXY_ROUTE_ORDERED_REGEX) {
      regex_t expression;
      int compiled = regcomp(&expression, route->pattern, REG_EXTENDED | REG_NOSUB);
      if (compiled != 0) return false;
      regfree(&expression);
    }
    ++options->route_count;
  }
  return options->route_count != 0U;
}

static bool yaml_load_file(const char *path, laghu_yaml_arguments *arguments, laghu_proxy_options *options, char *error, size_t error_size) {
  FILE *file = NULL;
  yaml_parser_t parser;
  yaml_document_t document;
  yaml_node_t *root;
  yaml_node_pair_t *pair;
  bool schema_seen = false;
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
    if (!strcmp(key, "schema")) {
      const char *schema = yaml_scalar(&document, pair->value);
      if (schema_seen || schema == NULL || strcmp(schema, "1") != 0) goto document_done;
      schema_seen = true;
    } else if (!strcmp(key, "runtime")) {
      if (runtime_seen || value->type != YAML_MAPPING_NODE || !yaml_runtime(&document, arguments, value)) goto document_done;
      runtime_seen = true;
    } else if (!strcmp(key, "sites")) {
      if (sites_seen || !yaml_sites(&document, options, value)) goto document_done;
      sites_seen = true;
    } else if (!strcmp(key, "routes")) {
      if (routes_seen || !yaml_routes(&document, options, value)) goto document_done;
      routes_seen = true;
    } else {
      goto document_done;
    }
  }
  valid = schema_seen && runtime_seen;
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

laghu_proxy_parse_result laghu_proxy_load_yaml(const char *path, laghu_proxy_options *options, char *error, size_t error_size) {
  laghu_yaml_arguments arguments = {0};
  laghu_proxy_parse_result result = LAGHU_PROXY_PARSE_ERROR;
  if (error_size != 0U) error[0] = '\0';
  if (options == NULL || !yaml_add(&arguments, "laghu") || !yaml_load_file(path, &arguments, options, error, error_size) ||
      !yaml_load_fragments(path, &arguments, options, error, error_size)) {
    goto done;
  }
  result = laghu_proxy_parse_options((int)arguments.count, arguments.values, options, error, error_size);
done:
  yaml_arguments_dispose(&arguments);
  if (result != LAGHU_PROXY_PARSE_OK && error_size != 0U && error[0] == '\0') (void)snprintf(error, error_size, "invalid YAML configuration");
  return result;
}
