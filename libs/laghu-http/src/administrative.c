// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <string.h>

#include "laghu/http.h"

static unsigned char laghu_http_administrative_ascii_lower(
    unsigned char value) {
  return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A'))
                                      : value;
}

static bool laghu_http_administrative_view_valid(laghu_buffer value) {
  return value.length == 0U || value.data != NULL;
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
  bool normalized;
  bool purge_control = false;
  bool method_purge;
  if (plan == NULL || options == NULL ||
      !laghu_http_administrative_view_valid(method)) {
    return false;
  }
  memset(plan, 0, sizeof(*plan));
  method_purge = laghu_http_administrative_method_is(method, "PURGE");
  normalized = laghu_http_administrative_target_copy(target, raw_target,
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
  } else if (method_purge || purge_control) {
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
    case LAGHU_HTTP_ADMINISTRATIVE_ROUTE_PURGE:
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
