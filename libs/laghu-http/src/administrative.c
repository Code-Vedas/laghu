// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "laghu/http.h"

static unsigned char laghu_http_administrative_ascii_lower(
    unsigned char value) {
  return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A'))
                                      : value;
}

static bool laghu_http_administrative_view_valid(laghu_buffer value) {
  return value.length == 0U || value.data != NULL;
}

static bool laghu_http_administrative_is_digit(unsigned char value) {
  return value >= '0' && value <= '9';
}

static bool laghu_http_administrative_target_split_query(laghu_buffer target,
                                                       laghu_buffer *path,
                                                       laghu_buffer *query) {
  size_t index;
  size_t query_start = target.length;
  if (!laghu_http_administrative_view_valid(target) || path == NULL ||
      query == NULL) {
    return false;
  }
  for (index = 0U; index < target.length; ++index) {
    if (target.data[index] == '?') {
      query_start = index;
      break;
    }
  }
  *path = (laghu_buffer){target.data, query_start};
  if (query_start < target.length) {
    *query = (laghu_buffer){target.data + query_start + 1U,
                            target.length - query_start - 1U};
  } else {
    *query = (laghu_buffer){NULL, 0U};
  }
  return true;
}

static bool laghu_http_administrative_query_param_value(
    laghu_buffer query, const char *name, bool required,
    char *output, size_t output_capacity) {
  size_t name_length = strlen(name);
  size_t index = 0U;
  size_t end = 0U;
  size_t equal = 0U;
  if (!laghu_http_administrative_view_valid(query) ||
      (required && output == NULL) || output_capacity == 0U) {
    return false;
  }
  if (query.data == NULL || query.length == 0U) {
    return !required;
  }
  while (index < query.length) {
    end = index;
    while (end < query.length && query.data[end] != '&') {
      ++end;
    }
    equal = index;
    while (equal < end && query.data[equal] != '=') {
      ++equal;
    }
    if (equal > index &&
        (size_t)(equal - index) == name_length &&
        memcmp(query.data + index, name, name_length) == 0) {
      if (equal == end) {
        if (required) return false;
        return true;
      }
      if ((size_t)(end - (equal + 1U)) >= output_capacity) return false;
      (void)memcpy(output, query.data + equal + 1U,
                   end - (equal + 1U));
      output[end - (equal + 1U)] = '\0';
      return true;
    }
    index = end + 1U;
  }
  return !required;
}

static bool laghu_http_administrative_query_parse_limit(laghu_buffer query,
                                                       unsigned int *value) {
  char text[12];
  size_t index;
  size_t digits = 0U;
  unsigned int parsed = 0U;
  if (value == NULL) {
    return false;
  }
  if (!laghu_http_administrative_query_param_value(query, "limit", false, text,
                                                  sizeof(text))) {
    return false;
  }
  if (text[0] == '\0') {
    *value = 0U;
    return true;
  }
  for (index = 0U; text[index] != '\0'; ++index) {
    if (!laghu_http_administrative_is_digit((unsigned char)text[index])) {
      return false;
    }
    if ((parsed > UINT_MAX / 10U) ||
        (parsed > (UINT_MAX - ((unsigned int)(text[index] - '0'))) / 10U)) {
      return false;
    }
    parsed = parsed * 10U + (unsigned int)(text[index] - '0');
    ++digits;
  }
  if (digits == 0U) return false;
  *value = parsed;
  return true;
}

static bool laghu_http_administrative_query_parse_path(
    laghu_buffer query, laghu_http_administrative_explain_query *query_out) {
  if (query_out == NULL) return false;
  if (query_out->target[0] != '\0') {
    query_out->target[0] = '\0';
  }
  query_out->json = false;
  if (!laghu_http_administrative_query_param_value(query, "path", false,
                                                  query_out->target,
                                                  sizeof(query_out->target))) {
    return false;
  }
  if (query_out->target[0] == '\0') {
    if (!laghu_http_administrative_query_param_value(query, "url", false,
                                                    query_out->target,
                                                    sizeof(query_out->target))) {
      return false;
    }
  }
  {
    char format[16];
    format[0] = '\0';
    if (!laghu_http_administrative_query_param_value(query, "format", false,
                                                    format, sizeof(format))) {
      return false;
    }
    if (format[0] != '\0' && strcmp(format, "json") != 0) return false;
    query_out->json = format[0] != '\0';
  }
  return query_out->target[0] != '\0';
}

static bool laghu_http_administrative_normalize_legacy_path(
    laghu_buffer path, char *output, size_t capacity) {
  if (!laghu_http_administrative_view_valid(path) || output == NULL ||
      capacity < 2U || path.length == 0U || path.length >= capacity) {
    return false;
  }
  if (path.data == NULL) return false;
  memcpy(output, path.data, path.length);
  output[path.length] = '\0';
  if (strcmp(output, "/pagespeed_admin") == 0 ||
      strcmp(output, "/pagespeed_admin/") == 0) {
    memcpy(output, "/.laghu/console", sizeof("/.laghu/console"));
    return true;
  }
  if (strcmp(output, "/pagespeed_console") == 0 ||
      strcmp(output, "/pagespeed_console/") == 0) {
    memcpy(output, "/.laghu/metrics", sizeof("/.laghu/metrics"));
    return true;
  }
  if (strcmp(output, "/pagespeed_statistics") == 0 ||
      strcmp(output, "/pagespeed_statistics/") == 0 ||
      strcmp(output, "/pagespeed_stats") == 0 ||
      strcmp(output, "/pagespeed_stats/") == 0) {
    memcpy(output, "/.laghu/stats", sizeof("/.laghu/stats"));
    return true;
  }
  return true;
}

static bool laghu_http_administrative_method_is(laghu_buffer method,
                                                const char *expected) {
  size_t expected_length = strlen(expected);
  size_t index;
  if (!laghu_http_administrative_view_valid(method) ||
      method.length != expected_length) {
    return false;
  }
  for (index = 0U; index < expected_length; ++index) {
    if (laghu_http_administrative_ascii_lower(method.data[index]) !=
        laghu_http_administrative_ascii_lower((unsigned char)expected[index])) {
      return false;
    }
  }
  return true;
}

static bool laghu_http_administrative_target_copy(laghu_buffer target,
                                                  char *output,
                                                  size_t capacity) {
  size_t index;
  if (!laghu_http_administrative_view_valid(target) || output == NULL ||
      capacity < 2U || target.length == 0U || target.length >= capacity) {
    return false;
  }
  for (index = 0U; index < target.length; ++index) {
    if (target.data[index] == '\0' || target.data[index] == '\r' ||
        target.data[index] == '\n') {
      return false;
    }
  }
  memcpy(output, target.data, target.length);
  output[target.length] = '\0';
  return true;
}

static bool laghu_http_administrative_target_mentions_purge(
    laghu_buffer target) {
  static const char marker[] = "laghu=purge";
  size_t index;
  if (!laghu_http_administrative_view_valid(target) ||
      target.length < sizeof(marker) - 1U) {
    return false;
  }
  for (index = 0U; index + sizeof(marker) - 1U <= target.length; ++index) {
    if (memcmp(target.data + index, marker, sizeof(marker) - 1U) == 0) {
      return true;
    }
  }
  return false;
}

void laghu_http_administrative_options_init(
    laghu_http_administrative_options *options) {
  if (options != NULL) {
    memset(options, 0, sizeof(*options));
  }
}

bool laghu_http_administrative_plan_build(
    laghu_http_administrative_plan *plan, laghu_buffer method,
    laghu_buffer target, const laghu_http_administrative_options *options) {
  char raw_target[LAGHU_RUNTIME_PATH_SIZE];
  char mapped_target[LAGHU_RUNTIME_PATH_SIZE];
  bool normalized;
  bool purge_control = false;
  bool method_purge;
  bool target_purge_query = false;
  laghu_buffer path;
  laghu_buffer query;
  unsigned int history_limit = 0U;
  if (!laghu_http_administrative_target_split_query(target, &path, &query) ||
      !laghu_http_administrative_view_valid(path) ||
      path.length >= sizeof(raw_target)) {
    if (!laghu_http_administrative_view_valid(target) || target.length == 0U) {
      return false;
    }
    path = target;
    query = (laghu_buffer){NULL, 0U};
  }
  if (plan == NULL || options == NULL ||
      !laghu_http_administrative_view_valid(method)) {
    return false;
  }
  memset(plan, 0, sizeof(*plan));
  target_purge_query = laghu_http_administrative_target_mentions_purge(target);
  plan->history_query.limit = LAGHU_OPERATIONAL_HISTORY_SIZE;
  method_purge = laghu_http_administrative_method_is(method, "PURGE");
  if (!laghu_http_administrative_normalize_legacy_path(path, mapped_target,
                                                     sizeof(mapped_target))) {
    return true;
  }
  path = (laghu_buffer){(const unsigned char *)mapped_target,
                        strlen(mapped_target)};
  normalized = laghu_http_administrative_target_copy(path, raw_target,
                                                     sizeof(raw_target));
  if (normalized) {
    normalized = laghu_cache_source_normalize(raw_target, plan->normalized_path,
                                              sizeof(plan->normalized_path),
                                              &purge_control);
  }
  if (!normalized) {
    if (!method_purge &&
        !laghu_http_administrative_target_mentions_purge(target)) {
      return true;
    }
    plan->recognized = true;
    plan->route = LAGHU_HTTP_ADMINISTRATIVE_ROUTE_PURGE;
    plan->requires_authorization = true;
    plan->status = 400U;
    return true;
  }
  if (strcmp(plan->normalized_path, "/.laghu/stats") == 0) {
    plan->route = LAGHU_HTTP_ADMINISTRATIVE_ROUTE_STATS;
  } else if (strcmp(plan->normalized_path, "/.laghu/metrics") == 0) {
    plan->route = LAGHU_HTTP_ADMINISTRATIVE_ROUTE_METRICS;
  } else if (strcmp(plan->normalized_path, "/.laghu/ready") == 0) {
    plan->route = LAGHU_HTTP_ADMINISTRATIVE_ROUTE_READINESS;
  } else if (strcmp(plan->normalized_path, "/.laghu/console") == 0) {
    plan->route = LAGHU_HTTP_ADMINISTRATIVE_ROUTE_CONSOLE;
  } else if (strcmp(plan->normalized_path, "/.laghu/history") == 0) {
    plan->route = LAGHU_HTTP_ADMINISTRATIVE_ROUTE_HISTORY;
  } else if (strcmp(plan->normalized_path, "/.laghu/explain") == 0) {
    plan->route = LAGHU_HTTP_ADMINISTRATIVE_ROUTE_EXPLAIN;
  } else if (method_purge || purge_control || target_purge_query) {
    plan->route = LAGHU_HTTP_ADMINISTRATIVE_ROUTE_PURGE;
  } else {
    return true;
  }
  plan->recognized = true;
  plan->head = laghu_http_administrative_method_is(method, "HEAD");
  if (plan->route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_METRICS &&
      !options->metrics_enabled) {
    plan->status = 404U;
    return true;
  }
  if (plan->route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_READINESS &&
      !options->readiness_enabled) {
    plan->status = 404U;
    return true;
  }
  plan->requires_authorization = true;
  switch (plan->route) {
    case LAGHU_HTTP_ADMINISTRATIVE_ROUTE_STATS:
      plan->action = LAGHU_HTTP_ADMINISTRATIVE_ACTION_STATS;
      if (!options->statistics_enabled ||
          (!laghu_http_administrative_method_is(method, "GET") &&
           !plan->head)) {
        plan->status = 405U;
      }
      break;
    case LAGHU_HTTP_ADMINISTRATIVE_ROUTE_METRICS:
      plan->action = LAGHU_HTTP_ADMINISTRATIVE_ACTION_METRICS;
      if (!laghu_http_administrative_method_is(method, "GET") && !plan->head) {
        plan->status = 405U;
      }
      break;
    case LAGHU_HTTP_ADMINISTRATIVE_ROUTE_READINESS:
      plan->action = LAGHU_HTTP_ADMINISTRATIVE_ACTION_READINESS;
      if (!laghu_http_administrative_method_is(method, "GET") && !plan->head) {
        plan->status = 405U;
      }
      break;
    case LAGHU_HTTP_ADMINISTRATIVE_ROUTE_CONSOLE:
      plan->action = LAGHU_HTTP_ADMINISTRATIVE_ACTION_CONSOLE;
      if (!options->statistics_enabled ||
          (!laghu_http_administrative_method_is(method, "GET") &&
           !plan->head))
        plan->status = 405U;
      break;
    case LAGHU_HTTP_ADMINISTRATIVE_ROUTE_HISTORY:
      plan->action = LAGHU_HTTP_ADMINISTRATIVE_ACTION_HISTORY;
      if (!options->statistics_enabled ||
          (!laghu_http_administrative_method_is(method, "GET") &&
           !plan->head)) {
        plan->status = 405U;
        break;
      }
      if (query.length > 0U &&
          !laghu_http_administrative_query_parse_limit(query, &history_limit)) {
        plan->status = 400U;
        break;
      }
      if (query.length == 0U || history_limit == 0U)
        history_limit = LAGHU_OPERATIONAL_HISTORY_SIZE;
      if (history_limit == 0U) {
        plan->status = 400U;
        break;
      }
      if (history_limit > LAGHU_OPERATIONAL_HISTORY_SIZE)
        history_limit = LAGHU_OPERATIONAL_HISTORY_SIZE;
      plan->history_query.limit = history_limit;
      break;
    case LAGHU_HTTP_ADMINISTRATIVE_ROUTE_EXPLAIN:
      plan->action = LAGHU_HTTP_ADMINISTRATIVE_ACTION_EXPLAIN;
      if (!options->statistics_enabled ||
          (!laghu_http_administrative_method_is(method, "GET") &&
           !plan->head)) {
        plan->status = 405U;
        break;
      }
      plan->explain_query.target[0] = '\0';
      if (!laghu_http_administrative_query_parse_path(query, &plan->explain_query)) {
        plan->status = 400U;
        break;
      }
      break;
    case LAGHU_HTTP_ADMINISTRATIVE_ROUTE_PURGE:
      purge_control = target_purge_query ? true : purge_control;
      plan->purge_by_method = method_purge;
      plan->purge_by_query = purge_control;
      plan->action = LAGHU_HTTP_ADMINISTRATIVE_ACTION_PURGE;
      if ((method_purge && !options->purge_method_enabled) ||
          (purge_control && !options->purge_query_enabled) ||
          (purge_control && options->purge_query_get_only &&
           !laghu_http_administrative_method_is(method, "GET"))) {
        plan->status = 405U;
      }
      break;
    case LAGHU_HTTP_ADMINISTRATIVE_ROUTE_NONE:
    default:
      return false;
  }
  return true;
}

bool laghu_http_administrative_render_operational(
    const laghu_http_administrative_plan *plan,
    const laghu_operational_snapshot *snapshot, uint64_t now,
    bool runtime_ready, bool cache_ready, bool strict_workers, char *output,
    size_t capacity, laghu_http_administrative_response *response) {
  laghu_operational_readiness readiness;
  size_t length = 0U;
  if (plan == NULL || snapshot == NULL || output == NULL || response == NULL ||
      plan->status != 0U ||
      (plan->action != LAGHU_HTTP_ADMINISTRATIVE_ACTION_METRICS &&
       plan->action != LAGHU_HTTP_ADMINISTRATIVE_ACTION_READINESS)) {
    return false;
  }
  memset(response, 0, sizeof(*response));
  if (plan->action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_METRICS) {
    if (!laghu_operational_render_prometheus(snapshot, now, output, capacity,
                                             &length)) {
      return false;
    }
    response->status = 200U;
    response->content = LAGHU_HTTP_ADMINISTRATIVE_CONTENT_PROMETHEUS;
    response->length = length;
    return true;
  }
  if (!laghu_operational_readiness_evaluate(snapshot, now, runtime_ready,
                                            cache_ready, strict_workers,
                                            &readiness) ||
      !laghu_operational_render_readiness(&readiness, strict_workers, output,
                                          capacity, &length)) {
    return false;
  }
  response->status = readiness.runtime_ready && readiness.cache_ready &&
                             readiness.workers_ready
                         ? 200U
                         : 503U;
  response->content = LAGHU_HTTP_ADMINISTRATIVE_CONTENT_JSON;
  response->length = length;
  return true;
}

bool laghu_http_administrative_render_purge(
    laghu_cache_purge_result purge_result, uint64_t matched_artifacts,
    char *output, size_t capacity,
    laghu_http_administrative_response *response) {
  int written;
  unsigned int status;
  if (output == NULL || response == NULL || capacity == 0U) {
    return false;
  }
  switch (purge_result) {
    case LAGHU_CACHE_PURGE_ACCEPTED:
      status = 202U;
      break;
    case LAGHU_CACHE_PURGE_SATURATED:
      status = 429U;
      break;
    case LAGHU_CACHE_PURGE_INVALID:
      status = 400U;
      break;
    case LAGHU_CACHE_PURGE_UNAVAILABLE:
    default:
      status = 503U;
      break;
  }
  written = snprintf(output, capacity,
                     "{\"status\":\"%s\",\"matched_artifacts\":%llu}",
                     status == 202U ? "accepted" : "rejected",
                     (unsigned long long)matched_artifacts);
  if (written < 0 || (size_t)written >= capacity) {
    return false;
  }
  response->status = status;
  response->content = LAGHU_HTTP_ADMINISTRATIVE_CONTENT_JSON;
  response->length = (size_t)written;
  return true;
}

static uint64_t laghu_http_administrative_hit_ratio_ppm(
    const laghu_cache_stats *stats) {
  long double requests;
  long double ratio;
  if (stats == NULL) {
    return 0U;
  }
  requests = (long double)stats->hits + (long double)stats->misses;
  if (requests <= 0.0L) {
    return 0U;
  }
  ratio = ((long double)stats->hits * 1000000.0L) / requests;
  if (ratio <= 0.0L) {
    return 0U;
  }
  if (ratio >= 1000000.0L) {
    return UINT64_C(1000000);
  }
  return (uint64_t)ratio;
}

static const char *laghu_http_administrative_surface_name(uint64_t surface) {
  switch ((laghu_operational_surface)surface) {
    case LAGHU_OPERATIONAL_SURFACE_NGINX:
      return "nginx";
    case LAGHU_OPERATIONAL_SURFACE_APACHE:
      return "apache";
    case LAGHU_OPERATIONAL_SURFACE_STANDALONE:
      return "standalone";
    case LAGHU_OPERATIONAL_SURFACE_WORKER:
      return "worker";
    default:
      return "unknown";
  }
}

static const char *laghu_http_administrative_process_name(uint64_t process) {
  switch ((laghu_operational_process_kind)process) {
    case LAGHU_OPERATIONAL_PROCESS_ADAPTER:
      return "adapter";
    case LAGHU_OPERATIONAL_PROCESS_LIBVIPS:
      return "libvips";
    case LAGHU_OPERATIONAL_PROCESS_JAVASCRIPT:
      return "javascript";
    case LAGHU_OPERATIONAL_PROCESS_RESOURCE_FETCH:
      return "resource-fetch";
    case LAGHU_OPERATIONAL_PROCESS_ASSET_UPLOAD:
      return "asset-upload";
    default:
      return "unknown";
  }
}

bool laghu_http_administrative_build_console_model(
    const laghu_cache_stats *stats, const laghu_operational_readiness *ready,
    laghu_http_administrative_console_model *model) {
  if (stats == NULL || ready == NULL || model == NULL) return false;
  memset(model, 0, sizeof(*model));
  (void)snprintf(model->runtime, sizeof(model->runtime), "%s",
                 ready->runtime_ready ? "ready" : "degraded");
  (void)snprintf(model->cache, sizeof(model->cache),
                 "%s", ready->cache_ready ? "ready" : "unavailable");
  (void)snprintf(model->workers, sizeof(model->workers),
                 "%s", ready->workers_ready ? "ready" : "unavailable");
  model->hits = stats->hits;
  model->misses = stats->misses;
  model->bytes = stats->bytes;
  return true;
}

bool laghu_http_administrative_render_console_model(
    const laghu_http_administrative_console_model *model, char *output,
    size_t capacity, laghu_http_administrative_response *response) {
  int written;
  if (model == NULL || output == NULL || response == NULL || capacity == 0U) {
    return false;
  }
  written = snprintf(
      output, capacity,
      "<!doctype html><meta charset=utf-8><title>Laghu console</title>"
      "<h1>Laghu console</h1><p>Runtime: %s · Cache: %s · Workers: %s</p>"
      "<dl><dt>Hits</dt><dd>%llu</dd><dt>Misses</dt><dd>%llu</dd>"
      "<dt>Bytes</dt><dd>%llu</dd></dl>"
      "<p><a href=\"/.laghu/stats\">Stats</a> · "
      "<a href=\"/.laghu/ready\">Readiness</a> · "
      "<a href=\"/.laghu/metrics\">Metrics</a> · "
      "<a href=\"/.laghu/history\">History</a> · "
      "<a href=\"/.laghu/explain?path=/\">Explain</a></p>",
      model->runtime, model->cache, model->workers,
      (unsigned long long)model->hits, (unsigned long long)model->misses,
      (unsigned long long)model->bytes);
  if (written < 0 || (size_t)written >= capacity) return false;
  response->status = 200U;
  response->content = LAGHU_HTTP_ADMINISTRATIVE_CONTENT_HTML;
  response->length = (size_t)written;
  return true;
}

bool laghu_http_administrative_render_stats(
    const laghu_cache_limits *limits, const laghu_cache_stats *stats,
    char *output, size_t capacity,
    laghu_http_administrative_response *response) {
  int written;
  if (limits == NULL || stats == NULL || output == NULL || response == NULL ||
      capacity == 0U) {
    return false;
  }
  written = snprintf(
      output, capacity,
      "{\"schema\":\"laghu-cache-stats-v1\",\"backend\":\"file\","
      "\"capacity\":{\"bytes\":%llu,\"files\":%llu},"
      "\"usage\":{\"bytes\":%llu,\"files\":%llu},"
      "\"requests\":{\"hits\":%llu,\"misses\":%llu,"
      "\"hit_ratio_ppm\":%llu},\"publications\":%llu,"
      "\"rejected_writes\":%llu,\"evictions\":%llu,"
      "\"purges\":{\"url\":%llu,\"full\":%llu,"
      "\"artifacts\":%llu,\"bytes\":%llu,\"generation\":%llu,"
      "\"last\":%llu},\"corrupt_removals\":%llu,"
      "\"cleaner_active\":%s,\"rebuilding\":%s,"
      "\"last_maintenance\":%llu}",
      (unsigned long long)limits->size_limit,
      (unsigned long long)limits->inode_limit, (unsigned long long)stats->bytes,
      (unsigned long long)stats->files, (unsigned long long)stats->hits,
      (unsigned long long)stats->misses,
      (unsigned long long)laghu_http_administrative_hit_ratio_ppm(stats),
      (unsigned long long)stats->publications,
      (unsigned long long)stats->rejected_publications,
      (unsigned long long)stats->evictions,
      (unsigned long long)stats->url_purges,
      (unsigned long long)stats->full_purges,
      (unsigned long long)stats->invalidated_artifacts,
      (unsigned long long)stats->invalidated_bytes,
      (unsigned long long)stats->cache_generation,
      (unsigned long long)stats->last_purge,
      (unsigned long long)stats->corrupt_removals,
      stats->cleaner_active ? "true" : "false",
      stats->rebuilding ? "true" : "false",
      (unsigned long long)stats->last_cleanup);
  if (written < 0 || (size_t)written >= capacity) {
    return false;
  }
  response->status = 200U;
  response->content = LAGHU_HTTP_ADMINISTRATIVE_CONTENT_JSON;
  response->length = (size_t)written;
  return true;
}

bool laghu_http_administrative_build_history_model(
    const laghu_operational_snapshot *snapshot,
    const laghu_http_administrative_history_query *query,
    laghu_http_administrative_history_model *model) {
  unsigned int index;
  unsigned int count;
  if (snapshot == NULL || query == NULL || model == NULL ||
      snapshot->version != LAGHU_OPERATIONAL_VERSION ||
      query->limit == 0U) {
    return false;
  }
  memset(model, 0, sizeof(*model));
  model->limit = query->limit;
  count = snapshot->slot_count < query->limit ? snapshot->slot_count : query->limit;
  for (index = 0U; index < count; ++index) {
    const laghu_operational_slot_snapshot *slot = &snapshot->slots[index];
    laghu_http_administrative_history_record *record = &model->records[index];
    record->index = index;
    record->active = slot->active != 0U;
    record->healthy = slot->healthy != 0U;
    record->required = slot->required != 0U;
    record->requests = slot->requests;
    record->original_bytes = slot->original_bytes;
    record->selected_bytes = slot->selected_bytes;
    (void)snprintf(record->surface, sizeof(record->surface), "%s",
                   laghu_http_administrative_surface_name(slot->surface));
    (void)snprintf(record->process, sizeof(record->process), "%s",
                   laghu_http_administrative_process_name(slot->process_kind));
    ++model->count;
  }
  return true;
}

bool laghu_http_administrative_render_history(
    const laghu_http_administrative_history_model *model, char *output,
    size_t capacity, laghu_http_administrative_response *response) {
  size_t length = 0U;
  unsigned int index;
  char row[256];
  int written;
  if (model == NULL || output == NULL || response == NULL || capacity == 0U) {
    return false;
  }
  written = snprintf(
      output, capacity,
      "<!doctype html><meta charset=utf-8><title>Laghu history</title>"
      "<h1>Laghu history</h1><p>Displaying up to %u slot records.</p>"
      "<table><thead><tr><th>Slot</th><th>Surface</th><th>Process</th>"
      "<th>Active</th><th>Healthy</th><th>Required</th>"
      "<th>Requests</th><th>Original bytes</th><th>Selected bytes</th>"
      "</tr></thead><tbody>",
      model->limit);
  if (written < 0 || (size_t)written >= capacity) return false;
  length = (size_t)written;
  for (index = 0U; index < model->count; ++index) {
    const laghu_http_administrative_history_record *record = &model->records[index];
    written = snprintf(
        row, sizeof(row),
        "<tr><td>%u</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td>"
        "<td>%s</td><td>%llu</td><td>%llu</td><td>%llu</td></tr>",
        record->index, record->surface, record->process,
        record->active ? "yes" : "no", record->healthy ? "yes" : "no",
        record->required ? "yes" : "no", (unsigned long long)record->requests,
        (unsigned long long)record->original_bytes,
        (unsigned long long)record->selected_bytes);
    if (written < 0 || (size_t)written >= sizeof(row)) return false;
    if (length + (size_t)written + 16U > capacity) return false;
    (void)memcpy(output + length, row, (size_t)written);
    length += (size_t)written;
  }
  {
    static const char tail[] = "</tbody></table>";
    size_t tail_length = strlen(tail);
    if (length + tail_length + 1U > capacity) return false;
    (void)memcpy(output + length, tail, tail_length);
    length += tail_length;
    output[length] = '\0';
  }
  response->status = 200U;
  response->content = LAGHU_HTTP_ADMINISTRATIVE_CONTENT_HTML;
  response->length = length;
  return true;
}

bool laghu_http_administrative_build_explain_model(
    const laghu_http_administrative_explain_query *query,
    const laghu_operational_readiness *ready, const laghu_cache_stats *stats,
    laghu_http_administrative_explain_model *model) {
  if (query == NULL || model == NULL || query->target[0] == '\0' ||
      stats == NULL || ready == NULL) {
    return false;
  }
  memset(model, 0, sizeof(*model));
  (void)snprintf(model->target, sizeof(model->target), "%s", query->target);
  model->has_target = true;
  model->json = query->json;
  model->runtime_ready = ready->runtime_ready;
  model->cache_ready = ready->cache_ready;
  model->workers_ready = ready->workers_ready;
  model->hits_ratio_ppm = laghu_http_administrative_hit_ratio_ppm(stats);
  if (!laghu_cache_source_hash(model->target, model->source_hash)) {
    model->source_hash[0] = '\0';
  }
  (void)snprintf(model->status, sizeof(model->status),
                 "%s", model->runtime_ready ? "runtime-ready"
                           : model->cache_ready ? "cache-ready"
                                              : "not-ready");
  (void)snprintf(
      model->recommendation, sizeof(model->recommendation),
      "Per-URL explainability is limited without dedicated request event "
      "storage. Enable request-level logging for full evidence on future requests.");
  return true;
}

bool laghu_http_administrative_render_explain(
    const laghu_http_administrative_explain_model *model, char *output,
    size_t capacity, laghu_http_administrative_response *response) {
  int written;
  if (model == NULL || output == NULL || response == NULL || capacity == 0U) {
    return false;
  }
  if (model->json) {
    written = snprintf(
        output, capacity,
        "{\"schema\":\"laghu-explain-v1\","
        "\"target\":\"%s\","
        "\"status\":\"%s\","
        "\"source_hash\":\"%s\","
        "\"readiness\":{\"runtime\":\"%s\",\"cache\":\"%s\","
        "\"workers\":\"%s\"},"
        "\"hit_ratio_ppm\":%llu,"
        "\"recommendation\":\"%s\"}",
        model->target, model->status, model->source_hash,
        model->runtime_ready ? "ready" : "unavailable",
        model->cache_ready ? "ready" : "unavailable",
        model->workers_ready ? "ready" : "unavailable",
        (unsigned long long)model->hits_ratio_ppm, model->recommendation);
    if (written < 0 || (size_t)written >= capacity) return false;
    response->status = 200U;
    response->content = LAGHU_HTTP_ADMINISTRATIVE_CONTENT_JSON;
    response->length = (size_t)written;
    return true;
  }
  written = snprintf(
      output, capacity,
      "<!doctype html><meta charset=utf-8><title>Laghu explain</title>"
      "<h1>Laghu explain</h1><p>Target: %s</p>"
      "<p>Status: %s</p><p>Target hash: %s</p>"
      "<p>Runtime: %s · Cache: %s · Workers: %s</p>"
      "<p>Cache hit ratio ppm: %llu</p>"
      "<p>Recommendation: %s</p>",
      model->target, model->status, model->source_hash,
      model->runtime_ready ? "ready" : "unavailable",
      model->cache_ready ? "ready" : "unavailable",
      model->workers_ready ? "ready" : "unavailable",
      (unsigned long long)model->hits_ratio_ppm, model->recommendation);
  if (written < 0 || (size_t)written >= capacity) return false;
  response->status = 200U;
  response->content = LAGHU_HTTP_ADMINISTRATIVE_CONTENT_HTML;
  response->length = (size_t)written;
  return true;
}

bool laghu_http_administrative_render_console(
    const laghu_cache_stats *stats, const laghu_operational_readiness *ready,
    char *output, size_t capacity,
    laghu_http_administrative_response *response) {
  laghu_http_administrative_console_model model;
  if (!laghu_http_administrative_build_console_model(stats, ready, &model)) {
    return false;
  }
  if (!laghu_http_administrative_render_console_model(&model, output, capacity,
                                                     response)) {
    return false;
  }
  return true;
}
