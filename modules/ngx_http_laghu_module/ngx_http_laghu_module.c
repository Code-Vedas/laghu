// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "laghu/core.h"
#include "laghu/http.h"
#include "laghu/image.h"
#include "laghu/runtime.h"

#ifdef _WIN32
#define LAGHU_NGINX_DEFAULT_QUEUE "C:/ProgramData/Laghu/jobs.queue"
#define LAGHU_NGINX_DEFAULT_CACHE "C:/ProgramData/Laghu/images"
#define LAGHU_NGINX_DEFAULT_FONT_QUEUE "C:/ProgramData/Laghu/fonts.queue"
#define LAGHU_NGINX_DEFAULT_JAVASCRIPT_QUEUE \
  "C:/ProgramData/Laghu/javascript.queue"
#else
#define LAGHU_NGINX_DEFAULT_QUEUE "/run/laghu/jobs.queue"
#define LAGHU_NGINX_DEFAULT_CACHE "/var/cache/laghu/images"
#define LAGHU_NGINX_DEFAULT_FONT_QUEUE "/run/laghu/fonts.queue"
#define LAGHU_NGINX_DEFAULT_JAVASCRIPT_QUEUE "/run/laghu/javascript.queue"
#endif

typedef struct {
  laghu_config core;
  laghu_runtime_queue runtime_queue;
  laghu_runtime_queue font_fetch_runtime_queue;
  laghu_runtime_queue javascript_runtime_queue;
  laghu_font_provider_set font_providers;
  laghu_javascript_observation_set javascript_observations;
  laghu_javascript_defer_set javascript_defer;
  ngx_str_t worker_queue;
  ngx_str_t font_fetch_queue;
  ngx_str_t font_provider_config;
  ngx_str_t javascript_queue;
  ngx_str_t javascript_target;
  ngx_str_t javascript_observation_config;
  ngx_str_t javascript_defer_config;
  ngx_str_t file_cache_backend;
  ngx_str_t image_cache;
  size_t file_cache_size;
  size_t file_cache_inode_limit;
  size_t file_cache_metadata_size;
  ngx_uint_t file_cache_clean_interval;
  bool image_cache_set;
  ngx_flag_t purge_method;
  ngx_flag_t purge_query;
  ngx_flag_t statistics;
  ngx_flag_t metrics;
  ngx_flag_t readiness;
  ngx_flag_t readiness_strict;
  ngx_str_t purge_token_file;
  ngx_str_t cache_flush_file;
  ngx_array_t *purge_allow;
  ngx_array_t *trusted_proxy;
  ngx_str_t asset_offload_config;
  ngx_str_t asset_upload_queue;
  laghu_asset_config asset_offload;
  bool asset_offload_loaded;
  laghu_source_policy source_policy;
  bool source_mode_set;
  bool font_providers_loaded;
  bool javascript_observations_loaded;
  bool javascript_defer_loaded;
} ngx_http_laghu_loc_conf_t;

typedef struct {
  ngx_str_t store_uri;
  ngx_str_t snapshot_path;
  ngx_str_t client_library;
  size_t memory_limit;
  size_t pending_limit;
  ngx_uint_t ttl_seconds;
  ngx_uint_t sync_interval_seconds;
  ngx_uint_t timeout_ms;
  ngx_uint_t retry_limit;
  ngx_flag_t required;
  uint32_t set_mask;
  ngx_str_t operational_cache;
} ngx_http_laghu_main_conf_t;

typedef struct {
  laghu_http_request request;
  laghu_http_response response;
  laghu_http_environment environment;
  laghu_http_header request_headers[LAGHU_HTTP_MAX_REQUEST_HEADERS];
  laghu_http_header response_headers[LAGHU_HTTP_MAX_RESPONSE_HEADERS];
  laghu_http_transaction transaction;
  laghu_policy policy;
  laghu_runtime_cache_entry cache_entry;
  char policy_key[LAGHU_SHA256_HEX_SIZE];
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  unsigned char *capture;
  size_t capture_length;
  size_t capture_capacity;
  ngx_chain_t *cached_output;
  unsigned char *cached_body;
  bool accept_webp;
  bool capture_enabled;
  bool cache_hit;
  bool cache_sent;
  unsigned int target_count;
  unsigned int target_width[LAGHU_RUNTIME_MAX_TARGETS];
  unsigned int target_height[LAGHU_RUNTIME_MAX_TARGETS];
  uint64_t resize_filter[LAGHU_RUNTIME_MAX_TARGETS];
  bool html_capture;
  bool css_capture;
  bool header_deferred;
} ngx_http_laghu_request_ctx_t;

static ngx_http_output_header_filter_pt ngx_http_laghu_next_header_filter;
static ngx_http_output_body_filter_pt ngx_http_laghu_next_body_filter;
static ngx_uint_t ngx_http_laghu_backend_warning_emitted;
static time_t ngx_http_laghu_last_queue_warning;
static time_t ngx_http_laghu_last_defer_recommendation;
static time_t ngx_http_laghu_beacon_window;
static ngx_uint_t ngx_http_laghu_beacon_count;
static laghu_rum_engine *ngx_http_laghu_rum;
static laghu_operational_registry ngx_http_laghu_operational;

static void ngx_http_laghu_log_defer_recommendation(
    ngx_http_request_t *request, const laghu_http_transaction_result *result) {
  time_t now;
  if (!result->javascript_defer_recommended &&
      !result->javascript_defer_rollback_recommended)
    return;
  now = ngx_time();
  if (ngx_http_laghu_last_defer_recommendation != 0 &&
      now - ngx_http_laghu_last_defer_recommendation < 3600)
    return;
  ngx_http_laghu_last_defer_recommendation = now;
  ngx_log_error(NGX_LOG_NOTICE, request->connection->log, 0,
                "laghu event=%s path=\"%s\" "
                "template=%s bucket=%ui observations=%uL approval=\"defer %s\"",
                result->javascript_defer_rollback_recommended
                    ? "javascript_defer_rollback_recommendation"
                    : "javascript_defer_recommendation",
                result->javascript_defer_path,
                result->javascript_defer_template,
                (ngx_uint_t)result->javascript_defer_bucket,
                (uint64_t)result->javascript_defer_observations,
                result->javascript_defer_path);
}

ngx_int_t ngx_http_laghu_header_filter(ngx_http_request_t *request);
ngx_int_t ngx_http_laghu_body_filter(ngx_http_request_t *request,
                                     ngx_chain_t *chain);
static ngx_int_t ngx_http_laghu_filter_init(ngx_conf_t *configuration);
static ngx_int_t ngx_http_laghu_init_process(ngx_cycle_t *cycle);
static void ngx_http_laghu_exit_process(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_laghu_variant_handler(ngx_http_request_t *request);
static void ngx_http_laghu_beacon_body(ngx_http_request_t *request);
static void *ngx_http_laghu_create_loc_conf(ngx_conf_t *configuration);
static void *ngx_http_laghu_create_main_conf(ngx_conf_t *configuration);
static char *ngx_http_laghu_merge_loc_conf(ngx_conf_t *configuration,
                                           void *parent, void *child);
static char *ngx_http_laghu_command(ngx_conf_t *configuration,
                                    ngx_command_t *command, void *conf);

static void ngx_http_laghu_queue_cleanup(void *data) {
  laghu_runtime_queue_close(data);
}

static bool ngx_http_laghu_font_queue_refresh(ngx_http_laghu_loc_conf_t *conf) {
  laghu_runtime_queue *queue = &conf->font_fetch_runtime_queue;
  if (!conf->font_providers_loaded || conf->font_fetch_queue.len == 0U)
    return false;
  if (queue->mapping == NULL &&
      !laghu_runtime_queue_open(queue,
                                (const char *)conf->font_fetch_queue.data))
    return false;
  return laghu_runtime_queue_refresh(queue);
}

static bool ngx_http_laghu_javascript_queue_refresh(
    ngx_http_laghu_loc_conf_t *conf) {
  laghu_runtime_queue *queue = &conf->javascript_runtime_queue;
  if (conf->javascript_queue.len == 0U) return false;
  if (queue->mapping == NULL &&
      !laghu_runtime_queue_open(queue,
                                (const char *)conf->javascript_queue.data))
    return false;
  return laghu_runtime_queue_refresh(queue);
}

static ngx_command_t ngx_http_laghu_commands[] = {
    {ngx_string("laghu"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1 | NGX_CONF_TAKE2 | NGX_CONF_TAKE3,
     ngx_http_laghu_command, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL},
    ngx_null_command};

static ngx_http_module_t ngx_http_laghu_module_context = {
    NULL,
    ngx_http_laghu_filter_init,
    ngx_http_laghu_create_main_conf,
    NULL,
    NULL,
    NULL,
    ngx_http_laghu_create_loc_conf,
    ngx_http_laghu_merge_loc_conf};

ngx_module_t ngx_http_laghu_module = {NGX_MODULE_V1,
                                      &ngx_http_laghu_module_context,
                                      ngx_http_laghu_commands,
                                      NGX_HTTP_MODULE,
                                      NULL,
                                      NULL,
                                      ngx_http_laghu_init_process,
                                      NULL,
                                      NULL,
                                      ngx_http_laghu_exit_process,
                                      NULL,
                                      NGX_MODULE_V1_PADDING};

static ngx_int_t ngx_http_laghu_init_process(ngx_cycle_t *cycle) {
  ngx_http_laghu_main_conf_t *conf;
  ngx_uint_t failure_level;
  laghu_rum_options options;
  char snapshot[LAGHU_RUNTIME_PATH_SIZE];
  char error[160U];
  int length;
  laghu_rum_options_init(&options);
  conf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_laghu_module);
  laghu_operational_registry_init(&ngx_http_laghu_operational);
  (void)laghu_operational_registry_open(
      &ngx_http_laghu_operational,
      conf != NULL && conf->operational_cache.len != 0U
          ? (const char *)conf->operational_cache.data
          : LAGHU_NGINX_DEFAULT_CACHE,
      LAGHU_OPERATIONAL_SURFACE_NGINX, LAGHU_OPERATIONAL_PROCESS_ADAPTER, true,
      (uint64_t)ngx_time());
  length = conf != NULL && conf->snapshot_path.len != 0U
               ? snprintf(snapshot, sizeof(snapshot), "%s",
                          (const char *)conf->snapshot_path.data)
               : snprintf(snapshot, sizeof(snapshot), "%s/rum.snapshot",
                          LAGHU_NGINX_DEFAULT_CACHE);
  if (length <= 0 || (size_t)length >= sizeof(snapshot)) return NGX_ERROR;
  options.snapshot_path = snapshot;
  if (conf != NULL) {
    options.store_uri = (const char *)conf->store_uri.data;
    options.client_library = conf->client_library.len == 0U
                                 ? NULL
                                 : (const char *)conf->client_library.data;
    options.memory_limit = conf->memory_limit;
    options.pending_limit = conf->pending_limit;
    options.ttl_seconds = (unsigned int)conf->ttl_seconds;
    options.sync_interval_seconds = (unsigned int)conf->sync_interval_seconds;
    options.timeout_ms = (unsigned int)conf->timeout_ms;
    options.retry_limit = (unsigned int)conf->retry_limit;
    options.required = conf->required == 1;
  }
  ngx_http_laghu_rum = laghu_rum_engine_create(&options, error, sizeof(error));
  if (ngx_http_laghu_rum == NULL) {
    failure_level = options.required ? NGX_LOG_EMERG : NGX_LOG_WARN;
    ngx_log_error(failure_level, cycle->log, 0,
                  "laghu RUM engine unavailable: %s", error);
    if (options.required) return NGX_ERROR;
    options.store_uri = "local:";
    options.client_library = NULL;
    options.required = false;
    ngx_http_laghu_rum =
        laghu_rum_engine_create(&options, error, sizeof(error));
    if (ngx_http_laghu_rum == NULL)
      ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                    "laghu local RUM fallback unavailable: %s", error);
  }
  return NGX_OK;
}

static void *ngx_http_laghu_create_main_conf(ngx_conf_t *configuration) {
  ngx_http_laghu_main_conf_t *conf =
      ngx_pcalloc(configuration->pool, sizeof(*conf));
  if (conf == NULL) return NULL;
  ngx_str_set(&conf->store_uri, "local:");
  conf->memory_limit = LAGHU_RUM_DEFAULT_MEMORY_BYTES;
  conf->pending_limit = LAGHU_RUM_DEFAULT_PENDING_BYTES;
  conf->ttl_seconds = LAGHU_IMAGE_METADATA_TTL_DEFAULT;
  conf->sync_interval_seconds = LAGHU_RUM_DEFAULT_SYNC_SECONDS;
  conf->timeout_ms = LAGHU_RUM_DEFAULT_TIMEOUT_MS;
  conf->retry_limit = LAGHU_RUM_DEFAULT_RETRY_LIMIT;
  conf->required = 0;
  return conf;
}

static void ngx_http_laghu_exit_process(ngx_cycle_t *cycle) {
  (void)cycle;
  laghu_operational_registry_close(&ngx_http_laghu_operational);
  laghu_rum_engine_destroy(ngx_http_laghu_rum);
  ngx_http_laghu_rum = NULL;
}

static char *ngx_http_laghu_copy_string(ngx_http_request_t *request,
                                        const ngx_str_t *value) {
  u_char *copy;

  if (value == NULL || value->len == 0) {
    return NULL;
  }

  copy = ngx_pnalloc(request->pool, value->len + 1);
  if (copy == NULL) {
    return NULL;
  }

  ngx_memcpy(copy, value->data, value->len);
  copy[value->len] = '\0';
  return (char *)copy;
}

#if nginx_version >= 1023000
static char *ngx_http_laghu_copy_header_chain(ngx_http_request_t *request,
                                              ngx_table_elt_t *header) {
  ngx_table_elt_t *current;
  size_t count = 0;
  size_t length = 0;
  u_char *copy;
  u_char *cursor;

  for (current = header; current != NULL; current = current->next) {
    if (current->hash == 0) {
      continue;
    }
    length += current->value.len;
    ++count;
  }

  if (count == 0) {
    return NULL;
  }

  copy = ngx_pnalloc(request->pool, length + count);
  if (copy == NULL) {
    return NULL;
  }

  cursor = copy;
  count = 0;
  for (current = header; current != NULL; current = current->next) {
    if (current->hash == 0) {
      continue;
    }
    if (count != 0) {
      *cursor++ = ',';
    }
    cursor = ngx_cpymem(cursor, current->value.data, current->value.len);
    ++count;
  }
  *cursor = '\0';
  return (char *)copy;
}
#endif

#if nginx_version < 1023000
static char *ngx_http_laghu_copy_header_array(ngx_http_request_t *request,
                                              ngx_array_t *array) {
  ngx_table_elt_t *headers;
  size_t index;
  size_t count = 0;
  size_t length = 0;
  u_char *copy;
  u_char *cursor;

  if (array->elts == NULL || array->nelts == 0U) {
    return NULL;
  }
  headers = array->elts;
  for (index = 0; index < array->nelts; ++index) {
    if (headers[index].hash != 0U) {
      length += headers[index].value.len;
      ++count;
    }
  }
  if (count == 0U) {
    return NULL;
  }
  copy = ngx_pnalloc(request->pool, length + count);
  if (copy == NULL) {
    return NULL;
  }
  cursor = copy;
  count = 0U;
  for (index = 0; index < array->nelts; ++index) {
    if (headers[index].hash == 0U) {
      continue;
    }
    if (count != 0U) {
      *cursor++ = ',';
    }
    cursor =
        ngx_cpymem(cursor, headers[index].value.data, headers[index].value.len);
    ++count;
  }
  *cursor = '\0';
  return (char *)copy;
}
#endif

static ngx_int_t ngx_http_laghu_add_status_header(ngx_http_request_t *request,
                                                  laghu_decision decision) {
  const char *status;
  const char *cache = "bypass";
  const char *transform = "pass";
  ngx_table_elt_t *header;
  (void)laghu_operational_registry_heartbeat(
      &ngx_http_laghu_operational, (uint64_t)ngx_time(), true, 0U, 0U);
  laghu_operational_registry_record(
      &ngx_http_laghu_operational,
      decision == LAGHU_DECISION_IMAGE_HIT
          ? LAGHU_OPERATIONAL_DECISION_CACHED
          : (decision == LAGHU_DECISION_PASS
                 ? LAGHU_OPERATIONAL_DECISION_ORIGINAL
                 : LAGHU_OPERATIONAL_DECISION_BYPASS),
      request->headers_out.content_length_n > 0
          ? (size_t)request->headers_out.content_length_n
          : 0U,
      request->headers_out.content_length_n > 0
          ? (size_t)request->headers_out.content_length_n
          : 0U,
      0U);

  header = ngx_list_push(&request->headers_out.headers);
  if (header == NULL) {
    return NGX_ERROR;
  }

  status = laghu_decision_name(decision);
  header->hash = 1;
  ngx_str_set(&header->key, "X-Laghu");
  header->value.len = ngx_strlen(status);
  header->value.data = (u_char *)status;
  if (decision == LAGHU_DECISION_IMAGE_HIT) {
    cache = "hit";
    transform = "optimized";
  } else if (decision == LAGHU_DECISION_PASS) {
    cache = "miss";
    transform = "queued";
  } else if (decision == LAGHU_DECISION_BYPASS_ERROR) {
    cache = "error";
    transform = "failed-open";
  }
  header = ngx_list_push(&request->headers_out.headers);
  if (header == NULL) return NGX_ERROR;
  header->hash = 1;
  ngx_str_set(&header->key, "X-Laghu-Cache");
  header->value.len = ngx_strlen(cache);
  header->value.data = (u_char *)cache;
  header = ngx_list_push(&request->headers_out.headers);
  if (header == NULL) return NGX_ERROR;
  header->hash = 1;
  ngx_str_set(&header->key, "X-Laghu-Transform");
  header->value.len = ngx_strlen(transform);
  header->value.data = (u_char *)transform;
  return NGX_OK;
}

static bool ngx_http_laghu_accepts_webp(ngx_table_elt_t *header) {
  static const u_char media_type[] = "image/webp";

  while (header != NULL) {
    size_t start = 0U;
    while (start < header->value.len) {
      size_t end = start;
      size_t type_end;
      size_t parameter;
      bool accepted = true;

      while (end < header->value.len && header->value.data[end] != ',') {
        ++end;
      }
      while (start < end && (header->value.data[start] == ' ' ||
                             header->value.data[start] == '\t')) {
        ++start;
      }
      type_end = start;
      while (type_end < end && header->value.data[type_end] != ';' &&
             header->value.data[type_end] != ' ' &&
             header->value.data[type_end] != '\t') {
        ++type_end;
      }
      if (type_end - start == sizeof(media_type) - 1U &&
          ngx_strncasecmp(header->value.data + start, (u_char *)media_type,
                          sizeof(media_type) - 1U) == 0) {
        for (parameter = type_end; parameter + 2U < end; ++parameter) {
          if ((header->value.data[parameter] == 'q' ||
               header->value.data[parameter] == 'Q') &&
              header->value.data[parameter + 1U] == '=' &&
              (parameter == type_end ||
               header->value.data[parameter - 1U] == ';' ||
               header->value.data[parameter - 1U] == ' ' ||
               header->value.data[parameter - 1U] == '\t')) {
            size_t quality = parameter + 2U;
            accepted = false;
            while (quality < end && header->value.data[quality] != ';' &&
                   header->value.data[quality] != ' ' &&
                   header->value.data[quality] != '\t') {
              if (header->value.data[quality] >= '1' &&
                  header->value.data[quality] <= '9') {
                accepted = true;
              }
              ++quality;
            }
            break;
          }
        }
        if (accepted) {
          return true;
        }
      }
      start = end + 1U;
    }
#if nginx_version >= 1023000
    header = header->next;
#else
    break;
#endif
  }
  return false;
}

static bool ngx_http_laghu_request_accepts_webp(ngx_http_request_t *request) {
  ngx_list_part_t *part = &request->headers_in.headers.part;
  ngx_table_elt_t *headers = part->elts;
  ngx_uint_t index;

  for (;;) {
    for (index = 0U; index < part->nelts; ++index) {
      if (headers[index].hash != 0U && headers[index].key.len == 6U &&
          ngx_strncasecmp(headers[index].key.data, (u_char *)"Accept", 6U) ==
              0 &&
          ngx_http_laghu_accepts_webp(&headers[index])) {
        return true;
      }
    }
    if (part->next == NULL) {
      break;
    }
    part = part->next;
    headers = part->elts;
  }
  return false;
}

static bool ngx_http_laghu_is_image_type(const ngx_str_t *content_type) {
  return content_type != NULL && content_type->len >= 6U &&
         ngx_strncasecmp(content_type->data, (u_char *)"image/", 6U) == 0;
}

static laghu_image_filter_mask ngx_http_laghu_image_filters(
    const laghu_policy *policy) {
  laghu_image_filter_mask filters =
      LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_RECOMPRESS_IMAGES |
      LAGHU_IMAGE_RECOMPRESS_JPEG | LAGHU_IMAGE_RECOMPRESS_PNG |
      LAGHU_IMAGE_RECOMPRESS_WEBP;

  if ((policy->filter_families & LAGHU_FILTER_IMAGE_METADATA) != 0U) {
    filters |= LAGHU_IMAGE_STRIP_METADATA | LAGHU_IMAGE_STRIP_COLOR_PROFILE;
  }
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_MODERN) != 0U) {
    filters |= LAGHU_IMAGE_JPEG_PROGRESSIVE | LAGHU_IMAGE_JPEG_TO_WEBP |
               LAGHU_IMAGE_PNG_TO_JPEG | LAGHU_IMAGE_GIF_TO_PNG |
               LAGHU_IMAGE_TO_WEBP_LOSSLESS | LAGHU_IMAGE_TO_WEBP_ANIMATED |
               LAGHU_IMAGE_JPEG_SAMPLING | LAGHU_IMAGE_IN_PLACE_BROWSER;
  }
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_RESPONSIVE) != 0U) {
    filters |= LAGHU_IMAGE_RESIZE_ATTRIBUTE | LAGHU_IMAGE_RESIZE_RENDERED |
               LAGHU_IMAGE_RESIZE_MOBILE | LAGHU_IMAGE_RESPONSIVE |
               LAGHU_IMAGE_RESPONSIVE_ZOOM;
  }
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_DIMENSIONS) != 0U) {
    filters |= LAGHU_IMAGE_INSERT_DIMENSIONS;
  }
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_LAZYLOAD) != 0U) {
    filters |= LAGHU_IMAGE_LAZYLOAD | LAGHU_IMAGE_INLINE_PREVIEW;
  }
  if ((policy->filter_families & LAGHU_FILTER_RESOURCE_INLINE) != 0U) {
    filters |= LAGHU_IMAGE_INLINE | LAGHU_IMAGE_DEDUP_INLINE;
  }
  return filters;
}

static laghu_html_planner_mask ngx_http_laghu_html_plan(
    const laghu_policy *policy) {
  laghu_html_planner_mask plan = 0U;
  bool html = (policy->filter_families & LAGHU_FILTER_HTML_MINIFY) != 0U;
  bool css = (policy->filter_families & LAGHU_FILTER_CSS_MINIFY) != 0U;
  if (html) {
    plan |= LAGHU_HTML_PLAN_LEXICAL | LAGHU_HTML_PLAN_CONVERT_META_TAGS;
  }
  if ((policy->filter_families & LAGHU_FILTER_RESOURCE_HINTS) != 0U) {
    plan |= LAGHU_HTML_PLAN_RESOURCE_HINTS;
  }
  if (html && policy->allow_structural_rewrite) {
    plan |= LAGHU_HTML_PLAN_ADD_COMBINE_HEAD;
  }
  if (html && css && policy->allow_structural_rewrite) {
    plan |= LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD;
    if (policy->allow_script_reordering) {
      plan |= LAGHU_HTML_PLAN_MOVE_CSS_ABOVE_SCRIPTS;
    }
  }
  return plan;
}

static void ngx_http_laghu_response_header_values(ngx_http_request_t *request,
                                                  const char *name,
                                                  char *output,
                                                  size_t capacity) {
  ngx_list_part_t *part = &request->headers_out.headers.part;
  ngx_table_elt_t *headers = part->elts;
  ngx_uint_t index;
  size_t used = 0U;
  size_t name_length = ngx_strlen(name);
  output[0] = '\0';
  for (index = 0U;; ++index) {
    if (index >= part->nelts) {
      if (part->next == NULL) {
        break;
      }
      part = part->next;
      headers = part->elts;
      index = 0U;
    }
    if (headers[index].hash == 0U || headers[index].key.len != name_length ||
        ngx_strncasecmp(headers[index].key.data, (u_char *)name, name_length) !=
            0) {
      continue;
    }
    if (used != 0U) {
      if (used + 2U >= capacity) {
        output[0] = '\0';
        return;
      }
      output[used++] = ',';
      output[used++] = ' ';
    }
    if (headers[index].value.len >= capacity - used) {
      output[0] = '\0';
      return;
    }
    ngx_memcpy(output + used, headers[index].value.data,
               headers[index].value.len);
    used += headers[index].value.len;
    output[used] = '\0';
  }
}

static bool ngx_http_laghu_apply_html_headers(
    ngx_http_request_t *request, const laghu_runtime_html_result *result) {
  ngx_table_elt_t *allocated[LAGHU_HTML_MAX_LINK_HEADERS + 1U];
  u_char *values[LAGHU_HTML_MAX_LINK_HEADERS + 1U];
  unsigned int count = result->link_header_count;
  unsigned int index;
  if (result->set_content_language) {
    ++count;
  }
  for (index = 0U; index < count; ++index) {
    const char *value = index < result->link_header_count
                            ? result->link_headers[index]
                            : result->content_language;
    size_t length = ngx_strlen(value);
    allocated[index] = ngx_list_push(&request->headers_out.headers);
    if (allocated[index] == NULL) {
      while (index > 0U) {
        allocated[--index]->hash = 0U;
      }
      return false;
    }
    allocated[index]->hash = 0U;
    values[index] = ngx_pnalloc(request->pool, length);
    if (values[index] == NULL) {
      do {
        allocated[index]->hash = 0U;
      } while (index-- > 0U);
      return false;
    }
    ngx_memcpy(values[index], value, length);
    allocated[index]->value.data = values[index];
    allocated[index]->value.len = length;
  }
  for (index = 0U; index < count; ++index) {
    allocated[index]->hash = 1U;
    if (index < result->link_header_count) {
      ngx_str_set(&allocated[index]->key, "Link");
    } else {
      ngx_str_set(&allocated[index]->key, "Content-Language");
    }
  }
  return true;
}

static bool ngx_http_laghu_validator(
    ngx_http_request_t *request, char output[LAGHU_RUNTIME_VALIDATOR_SIZE]) {
  size_t length;

  if (request->headers_out.etag != NULL &&
      request->headers_out.etag->value.len > 0U &&
      !(request->headers_out.etag->value.len >= 2U &&
        (request->headers_out.etag->value.data[0] == 'W' ||
         request->headers_out.etag->value.data[0] == 'w') &&
        request->headers_out.etag->value.data[1] == '/') &&
      request->headers_out.etag->value.len < LAGHU_RUNTIME_VALIDATOR_SIZE) {
    length = request->headers_out.etag->value.len;
    ngx_memcpy(output, request->headers_out.etag->value.data, length);
    output[length] = '\0';
    return true;
  }
  output[0] = '\0';
  return false;
}

static bool ngx_http_laghu_type_equals(const ngx_str_t *content_type,
                                       const char *expected) {
  size_t length = ngx_strlen(expected);

  return content_type != NULL && content_type->len == length &&
         ngx_strncasecmp(content_type->data, (u_char *)expected, length) == 0;
}

static bool ngx_http_laghu_is_html_type(const ngx_str_t *content_type) {
  static const char html[] = "text/html";
  return content_type != NULL && content_type->len >= sizeof(html) - 1U &&
         ngx_strncasecmp(content_type->data, (u_char *)html,
                         sizeof(html) - 1U) == 0;
}

static bool ngx_http_laghu_is_css_type(const ngx_str_t *content_type) {
  static const char css[] = "text/css";
  return content_type != NULL && content_type->len >= sizeof(css) - 1U &&
         ngx_strncasecmp(content_type->data, (u_char *)css, sizeof(css) - 1U) ==
             0;
}

static bool ngx_http_laghu_csp_allows_data(ngx_http_request_t *request) {
  ngx_list_part_t *part = &request->headers_out.headers.part;
  ngx_table_elt_t *headers = part->elts;
  ngx_uint_t index;
  for (index = 0U;; ++index) {
    if (index >= part->nelts) {
      if (part->next == NULL) {
        return true;
      }
      part = part->next;
      headers = part->elts;
      index = 0U;
    }
    if (headers[index].hash != 0U &&
        headers[index].key.len == sizeof("Content-Security-Policy") - 1U &&
        ngx_strncasecmp(headers[index].key.data,
                        (u_char *)"Content-Security-Policy",
                        sizeof("Content-Security-Policy") - 1U) == 0) {
      size_t offset;
      for (offset = 0U;
           offset + sizeof("data:") - 1U <= headers[index].value.len;
           ++offset) {
        if (ngx_strncasecmp(headers[index].value.data + offset,
                            (u_char *)"data:", sizeof("data:") - 1U) == 0) {
          return true;
        }
      }
      return false;
    }
  }
}

static bool ngx_http_laghu_csp_allows_inline_style(
    ngx_http_request_t *request) {
  ngx_list_part_t *part = &request->headers_out.headers.part;
  ngx_table_elt_t *headers = part->elts;
  ngx_uint_t index;
  for (index = 0U;; ++index) {
    if (index >= part->nelts) {
      if (part->next == NULL) {
        return true;
      }
      part = part->next;
      headers = part->elts;
      index = 0U;
    }
    if (headers[index].hash != 0U &&
        headers[index].key.len == sizeof("Content-Security-Policy") - 1U &&
        ngx_strncasecmp(headers[index].key.data,
                        (u_char *)"Content-Security-Policy",
                        sizeof("Content-Security-Policy") - 1U) == 0) {
      size_t offset;
      for (offset = 0U;
           offset + sizeof("'unsafe-inline'") - 1U <= headers[index].value.len;
           ++offset) {
        if (ngx_strncasecmp(headers[index].value.data + offset,
                            (u_char *)"'unsafe-inline'",
                            sizeof("'unsafe-inline'") - 1U) == 0) {
          return true;
        }
      }
      return false;
    }
  }
}

static bool ngx_http_laghu_csp_allows_self_style(ngx_http_request_t *request,
                                                 const char *page_origin) {
  ngx_list_part_t *part = &request->headers_out.headers.part;
  ngx_table_elt_t *headers = part->elts;
  ngx_uint_t index;
  for (index = 0U;; ++index) {
    if (index >= part->nelts) {
      if (part->next == NULL) {
        return true;
      }
      part = part->next;
      headers = part->elts;
      index = 0U;
    }
    if (headers[index].hash != 0U &&
        headers[index].key.len == sizeof("Content-Security-Policy") - 1U &&
        ngx_strncasecmp(headers[index].key.data,
                        (u_char *)"Content-Security-Policy",
                        sizeof("Content-Security-Policy") - 1U) == 0) {
      char *value = ngx_pnalloc(request->pool, headers[index].value.len + 1U);
      if (value == NULL) {
        return false;
      }
      ngx_memcpy(value, headers[index].value.data, headers[index].value.len);
      value[headers[index].value.len] = '\0';
      return laghu_runtime_csp_allows_self_styles(value, page_origin);
    }
  }
}

static bool ngx_http_laghu_csp_allows_self_script(ngx_http_request_t *request,
                                                  const char *page_origin) {
  ngx_list_part_t *part = &request->headers_out.headers.part;
  ngx_table_elt_t *headers = part->elts;
  ngx_uint_t index;
  for (index = 0U;; ++index) {
    if (index >= part->nelts) {
      if (part->next == NULL) return true;
      part = part->next;
      headers = part->elts;
      index = 0U;
    }
    if (headers[index].hash != 0U &&
        headers[index].key.len == sizeof("Content-Security-Policy") - 1U &&
        ngx_strncasecmp(headers[index].key.data,
                        (u_char *)"Content-Security-Policy",
                        sizeof("Content-Security-Policy") - 1U) == 0) {
      char *value = ngx_pnalloc(request->pool, headers[index].value.len + 1U);
      if (value == NULL) return false;
      ngx_memcpy(value, headers[index].value.data, headers[index].value.len);
      value[headers[index].value.len] = '\0';
      return laghu_runtime_csp_allows_self_scripts(value, page_origin);
    }
  }
}

static bool ngx_http_laghu_same_origin(ngx_http_request_t *request) {
  ngx_list_part_t *part = &request->headers_in.headers.part;
  ngx_table_elt_t *headers = part->elts;
  ngx_uint_t index;
  for (index = 0U;; ++index) {
    if (index >= part->nelts) {
      if (part->next == NULL) {
        return false;
      }
      part = part->next;
      headers = part->elts;
      index = 0U;
    }
    if (headers[index].hash != 0U &&
        headers[index].key.len == sizeof("Sec-Fetch-Site") - 1U &&
        ngx_strncasecmp(headers[index].key.data, (u_char *)"Sec-Fetch-Site",
                        sizeof("Sec-Fetch-Site") - 1U) == 0 &&
        headers[index].value.len == sizeof("same-origin") - 1U &&
        ngx_strncasecmp(headers[index].value.data, (u_char *)"same-origin",
                        sizeof("same-origin") - 1U) == 0) {
      return true;
    }
  }
}

static unsigned int ngx_http_laghu_unsigned_header(ngx_http_request_t *request,
                                                   const char *name,
                                                   size_t name_length,
                                                   unsigned int minimum,
                                                   unsigned int maximum) {
  ngx_list_part_t *part = &request->headers_in.headers.part;
  ngx_table_elt_t *headers = part->elts;
  ngx_uint_t index;
  for (index = 0U;; ++index) {
    ngx_int_t parsed;
    if (index >= part->nelts) {
      if (part->next == NULL) {
        return 0U;
      }
      part = part->next;
      headers = part->elts;
      index = 0U;
    }
    if (headers[index].hash == 0U || headers[index].key.len != name_length ||
        ngx_strncasecmp(headers[index].key.data, (u_char *)name, name_length) !=
            0) {
      continue;
    }
    parsed = ngx_atoi(headers[index].value.data, headers[index].value.len);
    if (parsed < (ngx_int_t)minimum || parsed > (ngx_int_t)maximum) {
      return 0U;
    }
    return (unsigned int)parsed;
  }
}

static unsigned int ngx_http_laghu_dpr_header(ngx_http_request_t *request) {
  ngx_list_part_t *part = &request->headers_in.headers.part;
  ngx_table_elt_t *headers = part->elts;
  ngx_uint_t index;
  for (index = 0U;; ++index) {
    unsigned int whole = 0U;
    unsigned int fraction = 0U;
    size_t cursor;
    if (index >= part->nelts) {
      if (part->next == NULL) {
        return 100U;
      }
      part = part->next;
      headers = part->elts;
      index = 0U;
    }
    if (headers[index].hash == 0U || headers[index].key.len != 3U ||
        ngx_strncasecmp(headers[index].key.data, (u_char *)"DPR", 3U) != 0) {
      continue;
    }
    for (cursor = 0U; cursor < headers[index].value.len &&
                      headers[index].value.data[cursor] >= '0' &&
                      headers[index].value.data[cursor] <= '9';
         ++cursor) {
      whole = whole * 10U + (headers[index].value.data[cursor] - '0');
    }
    if (cursor < headers[index].value.len &&
        headers[index].value.data[cursor++] == '.') {
      if (cursor < headers[index].value.len &&
          headers[index].value.data[cursor] >= '0' &&
          headers[index].value.data[cursor] <= '9') {
        fraction = (headers[index].value.data[cursor++] - '0') * 10U;
      }
      if (cursor < headers[index].value.len &&
          headers[index].value.data[cursor] >= '0' &&
          headers[index].value.data[cursor] <= '9') {
        fraction += headers[index].value.data[cursor++] - '0';
      }
    }
    if (cursor != headers[index].value.len || whole < 1U || whole > 4U ||
        (whole == 4U && fraction != 0U)) {
      return 100U;
    }
    return whole * 100U + fraction;
  }
}

static unsigned int ngx_http_laghu_viewport_header(
    ngx_http_request_t *request) {
  unsigned int width = ngx_http_laghu_unsigned_header(
      request, "Sec-CH-Viewport-Width", sizeof("Sec-CH-Viewport-Width") - 1U,
      1U, LAGHU_IMAGE_MAX_DIMENSION);
  return width != 0U
             ? width
             : ngx_http_laghu_unsigned_header(request, "Viewport-Width",
                                              sizeof("Viewport-Width") - 1U, 1U,
                                              LAGHU_IMAGE_MAX_DIMENSION);
}

static bool ngx_http_laghu_backend_supports(
    const ngx_str_t *content_type, laghu_image_filter_mask filters,
    laghu_image_capability_mask capabilities, bool allow_lossy) {
  if (ngx_http_laghu_type_equals(content_type, "image/jpeg")) {
    return allow_lossy && (capabilities & LAGHU_IMAGE_CAP_JPEG_LOAD) != 0U &&
           (((capabilities & LAGHU_IMAGE_CAP_JPEG_SAVE) != 0U &&
             (filters &
              (LAGHU_IMAGE_RECOMPRESS_IMAGES | LAGHU_IMAGE_RECOMPRESS_JPEG |
               LAGHU_IMAGE_JPEG_PROGRESSIVE | LAGHU_IMAGE_JPEG_SAMPLING)) !=
                 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U &&
             (filters & LAGHU_IMAGE_JPEG_TO_WEBP) != 0U));
  }
  if (ngx_http_laghu_type_equals(content_type, "image/png")) {
    return (capabilities & LAGHU_IMAGE_CAP_PNG_LOAD) != 0U &&
           (((capabilities & LAGHU_IMAGE_CAP_PNG_SAVE) != 0U &&
             (filters & (LAGHU_IMAGE_RECOMPRESS_IMAGES |
                         LAGHU_IMAGE_RECOMPRESS_PNG)) != 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_JPEG_SAVE) != 0U && allow_lossy &&
             (filters & LAGHU_IMAGE_PNG_TO_JPEG) != 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U &&
             (filters & LAGHU_IMAGE_TO_WEBP_LOSSLESS) != 0U));
  }
  if (ngx_http_laghu_type_equals(content_type, "image/gif")) {
    return (capabilities & LAGHU_IMAGE_CAP_GIF_LOAD) != 0U &&
           (((capabilities & LAGHU_IMAGE_CAP_PNG_SAVE) != 0U &&
             (filters & LAGHU_IMAGE_GIF_TO_PNG) != 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U &&
             (filters & (LAGHU_IMAGE_TO_WEBP_LOSSLESS |
                         LAGHU_IMAGE_TO_WEBP_ANIMATED)) != 0U));
  }
  if (ngx_http_laghu_type_equals(content_type, "image/webp")) {
    return (capabilities & LAGHU_IMAGE_CAP_WEBP_LOAD) != 0U &&
           (capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U;
  }
  return false;
}

static bool ngx_http_laghu_queue_available(ngx_http_laghu_loc_conf_t *conf,
                                           const ngx_str_t *content_type,
                                           laghu_image_filter_mask filters,
                                           bool allow_lossy) {
  laghu_runtime_queue *queue = &conf->runtime_queue;
  bool available = false;
  uint64_t now;

  if (conf->worker_queue.len == 0U) {
    return false;
  }
  available = queue->mapping != NULL && laghu_runtime_queue_refresh(queue);
  if (available) {
    now = (uint64_t)ngx_time();
    available = queue->worker_heartbeat != 0U &&
                queue->worker_heartbeat <= now &&
                now - queue->worker_heartbeat <= 45U &&
                ngx_http_laghu_backend_supports(
                    content_type, filters, queue->capabilities, allow_lossy);
    if (queue->worker_heartbeat <= now && now - queue->worker_heartbeat > 45U) {
      laghu_runtime_queue_close(queue);
    }
  }
  if (queue->mapping == NULL &&
      laghu_runtime_queue_open(queue, (const char *)conf->worker_queue.data) &&
      laghu_runtime_queue_refresh(queue)) {
    now = (uint64_t)ngx_time();
    available = queue->worker_heartbeat != 0U &&
                queue->worker_heartbeat <= now &&
                now - queue->worker_heartbeat <= 45U &&
                ngx_http_laghu_backend_supports(
                    content_type, filters, queue->capabilities, allow_lossy);
  }
  return available;
}

static bool ngx_http_laghu_queue_refresh(ngx_http_laghu_loc_conf_t *conf) {
  laghu_runtime_queue *queue = &conf->runtime_queue;
  uint64_t now = (uint64_t)ngx_time();
  if (queue->mapping == NULL &&
      !laghu_runtime_queue_open(queue, (const char *)conf->worker_queue.data)) {
    return false;
  }
  return laghu_runtime_queue_refresh(queue) && queue->capabilities != 0U &&
         queue->worker_heartbeat != 0U && queue->worker_heartbeat <= now &&
         now - queue->worker_heartbeat <= 45U;
}

static bool ngx_http_laghu_load_cache(ngx_http_request_t *request,
                                      const ngx_http_laghu_loc_conf_t *conf,
                                      ngx_http_laghu_request_ctx_t *context) {
  ngx_buf_t *buffer;

  if (context->validator[0] == '\0' ||
      !laghu_runtime_cache_lookup((const char *)conf->image_cache.data,
                                  context->index_key, context->validator,
                                  &context->cache_entry) ||
      context->cache_entry.length == 0U ||
      context->cache_entry.length > LAGHU_IMAGE_MAX_INPUT_BYTES) {
    return false;
  }
  context->cached_body =
      ngx_pnalloc(request->pool, context->cache_entry.length);
  if (context->cached_body == NULL ||
      !laghu_runtime_cache_read(&context->cache_entry, context->cached_body,
                                context->cache_entry.length)) {
    return false;
  }
  buffer = ngx_calloc_buf(request->pool);
  context->cached_output = ngx_alloc_chain_link(request->pool);
  if (buffer == NULL || context->cached_output == NULL) {
    context->cached_output = NULL;
    return false;
  }
  buffer->pos = context->cached_body;
  buffer->last = context->cached_body + context->cache_entry.length;
  buffer->memory = 1;
  buffer->last_in_chain = 1;
  buffer->last_buf = request == request->main;
  context->cached_output->buf = buffer;
  context->cached_output->next = NULL;
  return true;
}

static bool ngx_http_laghu_prepare_variant_headers(
    ngx_http_request_t *request, ngx_http_laghu_request_ctx_t *context) {
  static const char prefix[] = "\"laghu-";
  ngx_table_elt_t *etag = request->headers_out.etag;
  ngx_table_elt_t *vary;
  ngx_list_part_t *part;
  ngx_table_elt_t *headers;
  ngx_uint_t index;
  u_char *etag_value;
  u_char *cursor;
  size_t key_length = ngx_strlen(context->cache_entry.payload_hash);

  etag_value =
      ngx_pnalloc(request->pool, sizeof(prefix) - 1U + key_length + 2U);
  if (etag_value == NULL) {
    return false;
  }
  vary = ngx_list_push(&request->headers_out.headers);
  if (vary == NULL) {
    return false;
  }
  vary->hash = 0;
  if (etag == NULL) {
    etag = ngx_list_push(&request->headers_out.headers);
    if (etag == NULL) {
      return false;
    }
    etag->hash = 0;
  }
  cursor = ngx_cpymem(etag_value, prefix, sizeof(prefix) - 1U);
  cursor = ngx_cpymem(cursor, context->cache_entry.payload_hash, key_length);
  *cursor++ = '"';
  *cursor = '\0';

  etag->hash = 1;
  ngx_str_set(&etag->key, "ETag");
  etag->value.data = etag_value;
  etag->value.len = (size_t)(cursor - etag_value);
  request->headers_out.etag = etag;
  vary->hash = 1;
  ngx_str_set(&vary->key, "Vary");
  ngx_str_set(&vary->value, "Accept");

  part = &request->headers_out.headers.part;
  headers = part->elts;
  for (index = 0U;; ++index) {
    if (index >= part->nelts) {
      if (part->next == NULL) {
        break;
      }
      part = part->next;
      headers = part->elts;
      index = 0U;
    }
    if (headers[index].hash != 0U &&
        ((headers[index].key.len == sizeof("Content-MD5") - 1U &&
          ngx_strncasecmp(headers[index].key.data, (u_char *)"Content-MD5",
                          sizeof("Content-MD5") - 1U) == 0) ||
         (headers[index].key.len == sizeof("Digest") - 1U &&
          ngx_strncasecmp(headers[index].key.data, (u_char *)"Digest",
                          sizeof("Digest") - 1U) == 0))) {
      headers[index].hash = 0;
    }
  }
  return true;
}

static laghu_buffer ngx_http_laghu_view(const ngx_str_t *value) {
  return (laghu_buffer){value == NULL ? NULL : value->data,
                        value == NULL ? 0U : value->len};
}

static bool ngx_http_laghu_collect_headers(ngx_list_t *list,
                                           laghu_http_header *output,
                                           size_t capacity, size_t *count) {
  ngx_list_part_t *part = &list->part;
  ngx_table_elt_t *headers = part->elts;
  ngx_uint_t index;
  *count = 0U;
  for (index = 0U;; ++index) {
    if (index >= part->nelts) {
      if (part->next == NULL) {
        break;
      }
      part = part->next;
      headers = part->elts;
      index = 0U;
    }
    if (headers[index].hash == 0U) {
      continue;
    }
    if (*count >= capacity) {
      return false;
    }
    output[*count].name = ngx_http_laghu_view(&headers[index].key);
    output[*count].value = ngx_http_laghu_view(&headers[index].value);
    ++*count;
  }
  return true;
}

static bool ngx_http_laghu_has_header(const laghu_http_header *headers,
                                      size_t count, const char *name) {
  size_t index;
  size_t length = strlen(name);
  for (index = 0U; index < count; ++index) {
    if (headers[index].name.length == length &&
        ngx_strncasecmp((u_char *)headers[index].name.data, (u_char *)name,
                        length) == 0) {
      return true;
    }
  }
  return false;
}

static bool ngx_http_laghu_add_normalized_header(laghu_http_header *headers,
                                                 size_t *count, size_t capacity,
                                                 const char *name,
                                                 const ngx_str_t *value) {
  if (value == NULL || value->len == 0U ||
      ngx_http_laghu_has_header(headers, *count, name)) {
    return true;
  }
  if (*count >= capacity) {
    return false;
  }
  headers[*count].name =
      (laghu_buffer){(const unsigned char *)name, strlen(name)};
  headers[*count].value = ngx_http_laghu_view(value);
  ++*count;
  return true;
}

static void ngx_http_laghu_remove_header(ngx_http_request_t *request,
                                         const char *name) {
  ngx_list_part_t *part = &request->headers_out.headers.part;
  ngx_table_elt_t *headers = part->elts;
  size_t length = strlen(name);
  ngx_uint_t index;
  for (index = 0U;; ++index) {
    if (index >= part->nelts) {
      if (part->next == NULL) {
        break;
      }
      part = part->next;
      headers = part->elts;
      index = 0U;
    }
    if (headers[index].hash != 0U && headers[index].key.len == length &&
        ngx_strncasecmp(headers[index].key.data, (u_char *)name, length) == 0) {
      headers[index].hash = 0U;
    }
  }
  if (ngx_strcasecmp((u_char *)name, (u_char *)"Content-Length") == 0) {
    request->headers_out.content_length_n = -1;
    request->headers_out.content_length = NULL;
  } else if (ngx_strcasecmp((u_char *)name, (u_char *)"ETag") == 0) {
    request->headers_out.etag = NULL;
  }
}

static ngx_int_t ngx_http_laghu_apply_result(
    ngx_http_request_t *request, const laghu_http_transaction_result *result) {
  ngx_table_elt_t **staged;
  size_t index;
  staged = ngx_pcalloc(request->pool,
                       result->header_operation_count * sizeof(*staged));
  if (result->header_operation_count != 0U && staged == NULL) {
    return NGX_ERROR;
  }
  for (index = 0U; index < result->header_operation_count; ++index) {
    const laghu_http_header_operation *operation =
        &result->header_operations[index];
    if (operation->kind == LAGHU_HTTP_HEADER_REMOVE) {
      continue;
    }
    staged[index] = ngx_list_push(&request->headers_out.headers);
    if (staged[index] == NULL) {
      return NGX_ERROR;
    }
    ngx_memzero(staged[index], sizeof(**staged));
    staged[index]->key.len = strlen(operation->name);
    staged[index]->key.data =
        ngx_pnalloc(request->pool, staged[index]->key.len + 1U);
    staged[index]->value.len = strlen(operation->value);
    staged[index]->value.data =
        ngx_pnalloc(request->pool, staged[index]->value.len + 1U);
    if (staged[index]->key.data == NULL || staged[index]->value.data == NULL) {
      return NGX_ERROR;
    }
    ngx_memcpy(staged[index]->key.data, operation->name,
               staged[index]->key.len + 1U);
    ngx_memcpy(staged[index]->value.data, operation->value,
               staged[index]->value.len + 1U);
  }
  for (index = 0U; index < result->header_operation_count; ++index) {
    const laghu_http_header_operation *operation =
        &result->header_operations[index];
    if (operation->kind != LAGHU_HTTP_HEADER_APPEND) {
      ngx_http_laghu_remove_header(request, operation->name);
    }
    if (operation->kind == LAGHU_HTTP_HEADER_REMOVE) {
      continue;
    }
    {
      ngx_table_elt_t *header = staged[index];
      header->hash = 1U;
      if (ngx_strcasecmp(header->key.data, (u_char *)"Content-Type") == 0) {
        request->headers_out.content_type = header->value;
        request->headers_out.content_type_len = header->value.len;
      } else if (ngx_strcasecmp(header->key.data, (u_char *)"Content-Length") ==
                 0) {
        request->headers_out.content_length_n =
            ngx_atoof(header->value.data, header->value.len);
        request->headers_out.content_length = header;
      } else if (ngx_strcasecmp(header->key.data, (u_char *)"ETag") == 0) {
        request->headers_out.etag = header;
      }
    }
  }
  return NGX_OK;
}

static bool ngx_http_laghu_normalize(ngx_http_request_t *request,
                                     ngx_http_laghu_loc_conf_t *conf,
                                     ngx_http_laghu_request_ctx_t *context) {
  const ngx_str_t *authority = request->headers_in.host == NULL
                                   ? &request->headers_in.server
                                   : &request->headers_in.host->value;
  laghu_http_transaction_init(&context->transaction);
  if (!ngx_http_laghu_collect_headers(
          &request->headers_in.headers, context->request_headers,
          LAGHU_HTTP_MAX_REQUEST_HEADERS, &context->request.header_count) ||
      !ngx_http_laghu_collect_headers(
          &request->headers_out.headers, context->response_headers,
          LAGHU_HTTP_MAX_RESPONSE_HEADERS, &context->response.header_count) ||
      !ngx_http_laghu_add_normalized_header(
          context->response_headers, &context->response.header_count,
          LAGHU_HTTP_MAX_RESPONSE_HEADERS, "Content-Type",
          &request->headers_out.content_type)) {
    return false;
  }
  context->request.version = LAGHU_HTTP_ABI_VERSION;
  context->request.struct_size = sizeof(context->request);
  context->request.method = ngx_http_laghu_view(&request->method_name);
#if (NGX_HTTP_SSL)
  context->request.scheme =
      request->connection->ssl == NULL
          ? (laghu_buffer){(const unsigned char *)"http", 4U}
          : (laghu_buffer){(const unsigned char *)"https", 5U};
#else
  context->request.scheme = (laghu_buffer){(const unsigned char *)"http", 4U};
#endif
  if (conf->core.respect_x_forwarded_proto == LAGHU_MODE_ON &&
      conf->trusted_proxy != NULL &&
      ngx_cidr_match(request->connection->sockaddr, conf->trusted_proxy) ==
          NGX_OK) {
    ngx_list_part_t *part = &request->headers_in.headers.part;
    ngx_table_elt_t *headers = part->elts;
    ngx_table_elt_t *forwarded = NULL;
    ngx_uint_t index;
    for (index = 0U;; ++index) {
      if (index >= part->nelts) {
        if (part->next == NULL) break;
        part = part->next;
        headers = part->elts;
        index = 0U;
      }
      if (headers[index].hash != 0U &&
          headers[index].key.len == sizeof("X-Forwarded-Proto") - 1U &&
          ngx_strcasecmp(headers[index].key.data,
                         (u_char *)"X-Forwarded-Proto") == 0) {
        if (forwarded != NULL) {
          forwarded = NULL;
          break;
        }
        forwarded = &headers[index];
      }
    }
    if (forwarded != NULL && forwarded->value.len == 5U &&
        ngx_strncasecmp(forwarded->value.data, (u_char *)"https", 5U) == 0)
      context->request.scheme =
          (laghu_buffer){(const unsigned char *)"https", 5U};
    else if (forwarded != NULL && forwarded->value.len == 4U &&
             ngx_strncasecmp(forwarded->value.data, (u_char *)"http", 4U) == 0)
      context->request.scheme =
          (laghu_buffer){(const unsigned char *)"http", 4U};
  }
  context->request.authority = ngx_http_laghu_view(authority);
  if (request->args.len != 0U) {
    u_char *path =
        ngx_pnalloc(request->pool, request->uri.len + 1U + request->args.len);
    if (path == NULL) return false;
    ngx_memcpy(path, request->uri.data, request->uri.len);
    path[request->uri.len] = '?';
    ngx_memcpy(path + request->uri.len + 1U, request->args.data,
               request->args.len);
    context->request.normalized_path =
        (laghu_buffer){path, request->uri.len + 1U + request->args.len};
  } else {
    context->request.normalized_path = ngx_http_laghu_view(&request->uri);
  }
  context->request.headers = context->request_headers;
  context->response.version = LAGHU_HTTP_ABI_VERSION;
  context->response.struct_size = sizeof(context->response);
  context->response.status = (unsigned int)request->headers_out.status;
  context->response.headers = context->response_headers;
  context->response.has_declared_length =
      request->headers_out.content_length_n >= 0;
  context->response.declared_length =
      context->response.has_declared_length
          ? (size_t)request->headers_out.content_length_n
          : 0U;
  context->response.complete = true;
  context->response.partial =
      request->headers_out.status == NGX_HTTP_PARTIAL_CONTENT;
  context->environment.version = LAGHU_HTTP_ABI_VERSION;
  context->environment.struct_size = sizeof(context->environment);
  context->environment.config = conf->core;
  context->environment.cache_path = (const char *)conf->image_cache.data;
  context->environment.rum = ngx_http_laghu_rum;
  context->environment.worker_queue_path =
      (const char *)conf->worker_queue.data;
  context->environment.queue = &conf->runtime_queue;
  context->environment.font_fetch_queue_path =
      conf->font_fetch_queue.len == 0U
          ? NULL
          : (const char *)conf->font_fetch_queue.data;
  context->environment.font_fetch_queue =
      ngx_http_laghu_font_queue_refresh(conf) ? &conf->font_fetch_runtime_queue
                                              : NULL;
  context->environment.font_providers =
      conf->font_providers_loaded ? &conf->font_providers : NULL;
  context->environment.javascript_queue_path =
      (const char *)conf->javascript_queue.data;
  context->environment.javascript_queue =
      ngx_http_laghu_javascript_queue_refresh(conf)
          ? &conf->javascript_runtime_queue
          : NULL;
  context->environment.javascript_target =
      (const char *)conf->javascript_target.data;
  context->environment.javascript_observations =
      conf->javascript_observations_loaded ? &conf->javascript_observations
                                           : NULL;
  context->environment.javascript_defer =
      conf->javascript_defer_loaded ? &conf->javascript_defer : NULL;
  context->environment.asset_offload =
      conf->asset_offload_loaded ? &conf->asset_offload : NULL;
  context->environment.now = (uint64_t)ngx_time();
  return true;
}

ngx_int_t ngx_http_laghu_header_filter(ngx_http_request_t *request) {
  ngx_http_laghu_loc_conf_t *conf;
  ngx_http_laghu_request_ctx_t *context;
  laghu_response response;
  laghu_decision decision;
  laghu_image_filter_mask image_filters;

  conf = ngx_http_get_module_loc_conf(request, ngx_http_laghu_module);
  if (conf->cache_flush_file.len != 0U)
    (void)laghu_cache_flush_file_poll((char *)conf->image_cache.data,
                                      (char *)conf->cache_flush_file.data,
                                      (uint64_t)ngx_time(), NULL);
  if (request->uri.len >= sizeof("/.laghu/") - 1U &&
      ngx_strncmp(request->uri.data, "/.laghu/", sizeof("/.laghu/") - 1U) ==
          0) {
    return ngx_http_laghu_next_header_filter(request);
  }
  if (conf->core.mode != LAGHU_MODE_ON) {
    return ngx_http_laghu_next_header_filter(request);
  }
  if (conf->core.mode != LAGHU_MODE_ON) {
    return ngx_http_laghu_next_header_filter(request);
  }

  if (conf->core.rewrite_level == LAGHU_REWRITE_LEVEL_PASSTHROUGH) {
    decision = LAGHU_DECISION_BYPASS_PASSTHROUGH;
    if (ngx_http_laghu_add_status_header(request, decision) != NGX_OK) {
      ngx_log_error(
          NGX_LOG_WARN, request->connection->log, 0,
          "laghu could not allocate its response status header; serving the "
          "original response");
    }
    return ngx_http_laghu_next_header_filter(request);
  }

  response.status = (unsigned int)request->headers_out.status;
  response.request_path = ngx_http_laghu_copy_string(request, &request->uri);
  response.content_type =
      ngx_http_laghu_copy_string(request, &request->headers_out.content_type);
#if nginx_version >= 1023000
  response.cache_control = ngx_http_laghu_copy_header_chain(
      request, request->headers_out.cache_control);
#else
  response.cache_control = ngx_http_laghu_copy_header_array(
      request, &request->headers_out.cache_control);
#endif
  response.has_authorization = request->headers_in.authorization != NULL;

  if ((request->uri.len != 0 && response.request_path == NULL) ||
      (request->headers_out.content_type.len != 0 &&
       response.content_type == NULL) ||
#if nginx_version >= 1023000
      (request->headers_out.cache_control != NULL &&
       response.cache_control == NULL)
#else
      (request->headers_out.cache_control.nelts != 0U &&
       response.cache_control == NULL)
#endif
  ) {
    decision = LAGHU_DECISION_BYPASS_ERROR;
  } else {
    decision = laghu_decide(&conf->core, &response);
  }

  if (decision == LAGHU_DECISION_PASS &&
      ngx_http_laghu_is_image_type(&request->headers_out.content_type) &&
      request->headers_out.content_encoding != NULL &&
      request->headers_out.content_encoding->value.len != 0U) {
    decision = LAGHU_DECISION_BYPASS_ENCODED;
  } else if (decision == LAGHU_DECISION_PASS &&
             ngx_http_laghu_is_html_type(&request->headers_out.content_type) &&
             request->headers_out.content_encoding == NULL &&
             request->headers_out.content_length_n > 0 &&
             request->headers_out.content_length_n <=
                 (off_t)LAGHU_IMAGE_MAX_INPUT_BYTES) {
    context = ngx_pcalloc(request->pool, sizeof(*context));
    if (context != NULL &&
        laghu_resolve_config_policy(&conf->core, &context->policy) &&
        laghu_variant_key((laghu_buffer){NULL, 0U}, &context->policy,
                          context->policy_key)) {
      context->capture_capacity = (size_t)request->headers_out.content_length_n;
      (void)ngx_http_laghu_queue_refresh(conf);
      context->capture = ngx_pnalloc(request->pool, context->capture_capacity);
      if (context->capture != NULL) {
        context->capture_enabled = true;
        context->html_capture = true;
        context->header_deferred = true;
        request->filter_need_in_memory = 1U;
        ngx_http_set_ctx(request, context, ngx_http_laghu_module);
      }
    }
  } else if (decision == LAGHU_DECISION_PASS &&
             ngx_http_laghu_is_css_type(&request->headers_out.content_type) &&
             request->headers_out.content_encoding == NULL &&
             request->headers_out.content_length_n > 0 &&
             request->headers_out.content_length_n <=
                 (off_t)LAGHU_CSS_MAX_INPUT_BYTES) {
    context = ngx_pcalloc(request->pool, sizeof(*context));
    (void)ngx_http_laghu_queue_refresh(conf);
    if (context != NULL &&
        laghu_resolve_config_policy(&conf->core, &context->policy) &&
        laghu_variant_key((laghu_buffer){NULL, 0U}, &context->policy,
                          context->policy_key) &&
        ((context->policy.filter_families & LAGHU_FILTER_CSS_MINIFY) != 0U ||
         context->policy.allow_structural_rewrite)) {
      context->capture_capacity = (size_t)request->headers_out.content_length_n;
      context->capture = ngx_pnalloc(request->pool, context->capture_capacity);
      if (context->capture != NULL) {
        context->capture_enabled = true;
        context->css_capture = true;
        context->header_deferred = true;
        request->filter_need_in_memory = 1U;
        ngx_http_set_ctx(request, context, ngx_http_laghu_module);
      }
    }
  } else if (decision == LAGHU_DECISION_PASS &&
             ngx_http_laghu_is_image_type(&request->headers_out.content_type)) {
    context = ngx_pcalloc(request->pool, sizeof(*context));
    if (context == NULL ||
        !laghu_resolve_config_policy(&conf->core, &context->policy) ||
        !laghu_variant_key((laghu_buffer){NULL, 0U}, &context->policy,
                           context->policy_key)) {
      decision = LAGHU_DECISION_BYPASS_ERROR;
    } else {
      context->accept_webp = ngx_http_laghu_request_accepts_webp(request);
      (void)ngx_http_laghu_validator(request, context->validator);
      if (!laghu_runtime_index_key(
              response.request_path != NULL ? response.request_path : "",
              context->validator, context->policy_key, context->accept_webp,
              context->index_key)) {
        decision = LAGHU_DECISION_BYPASS_ERROR;
      } else {
        laghu_catalog_record catalog;
        if (laghu_catalog_lookup_url(
                (const char *)conf->image_cache.data,
                response.request_path != NULL ? response.request_path : "",
                context->policy_key, conf->runtime_queue.capabilities,
                (uint64_t)ngx_time(), conf->core.image_metadata_ttl,
                &catalog)) {
          unsigned int index;
          for (index = 0U; index < catalog.variant_count &&
                           context->target_count < LAGHU_RUNTIME_MAX_TARGETS;
               ++index) {
            if (!catalog.variants[index].ready &&
                !catalog.variants[index].terminally_excluded &&
                catalog.variants[index].width > 0U) {
              unsigned int target = context->target_count++;
              context->target_width[target] = catalog.variants[index].width;
              context->target_height[target] = catalog.variants[index].height;
              context->resize_filter[target] = LAGHU_IMAGE_RESIZE_ATTRIBUTE;
            }
          }
        }
      }
      if (decision == LAGHU_DECISION_PASS && context->target_count == 0U &&
          ngx_http_laghu_load_cache(request, conf, context) &&
          ngx_http_laghu_prepare_variant_headers(request, context)) {
        context->cache_hit = true;
        request->headers_out.content_type.data =
            (u_char *)context->cache_entry.content_type;
        request->headers_out.content_type.len =
            ngx_strlen(context->cache_entry.content_type);
        request->headers_out.content_type_len =
            request->headers_out.content_type.len;
        request->headers_out.content_length_n =
            (off_t)context->cache_entry.length;
        if (request->headers_out.content_length != NULL) {
          request->headers_out.content_length->hash = 0;
          request->headers_out.content_length = NULL;
        }
        decision = LAGHU_DECISION_IMAGE_HIT;
        ngx_http_set_ctx(request, context, ngx_http_laghu_module);
      } else if (decision == LAGHU_DECISION_PASS &&
                 (((image_filters = ngx_http_laghu_image_filters(
                        &context->policy)) == 0U) ||
                  !ngx_http_laghu_queue_available(
                      conf, &request->headers_out.content_type, image_filters,
                      context->policy.allow_lossy))) {
        decision = LAGHU_DECISION_BYPASS_IMAGE_BACKEND;
        if (ngx_http_laghu_backend_warning_emitted == 0U) {
          ngx_http_laghu_backend_warning_emitted = 1U;
          ngx_log_error(NGX_LOG_WARN, request->connection->log, 0,
                        "laghu image worker or selected codec is unavailable; "
                        "preserving image originals");
        }
      } else if (request->headers_out.content_length_n > 0 &&
                 request->headers_out.content_length_n <=
                     (off_t)LAGHU_IMAGE_MAX_INPUT_BYTES) {
        context->capture_capacity =
            (size_t)request->headers_out.content_length_n;
        context->capture =
            ngx_pnalloc(request->pool, context->capture_capacity);
        if (context->capture == NULL) {
          decision = LAGHU_DECISION_BYPASS_ERROR;
        } else {
          context->capture_enabled = true;
          ngx_http_set_ctx(request, context, ngx_http_laghu_module);
        }
      }
    }
  }
  if (ngx_http_laghu_add_status_header(request, decision) != NGX_OK) {
    ngx_log_error(
        NGX_LOG_WARN, request->connection->log, 0,
        "laghu could not allocate its response status header; serving the "
        "original response");
  }

  context = ngx_http_get_module_ctx(request, ngx_http_laghu_module);
  if (context != NULL && context->header_deferred) {
    return NGX_OK;
  }

  return ngx_http_laghu_next_header_filter(request);
}

ngx_int_t ngx_http_laghu_body_filter(ngx_http_request_t *request,
                                     ngx_chain_t *chain) {
  ngx_http_laghu_loc_conf_t *conf;
  ngx_http_laghu_request_ctx_t *context;
  ngx_chain_t *current;
  bool final_buffer = false;

  context = ngx_http_get_module_ctx(request, ngx_http_laghu_module);
  if (context == NULL) {
    return ngx_http_laghu_next_body_filter(request, chain);
  }

  if (context->cache_hit) {
    if (context->cache_sent || chain == NULL) {
      return NGX_OK;
    }
    context->cache_sent = true;
    return ngx_http_laghu_next_body_filter(request, context->cached_output);
  }

  if (!context->capture_enabled || chain == NULL) {
    return ngx_http_laghu_next_body_filter(request, chain);
  }
  for (current = chain; current != NULL; current = current->next) {
    ngx_buf_t *buffer = current->buf;
    size_t length = (size_t)ngx_buf_size(buffer);

    if (length > context->capture_capacity - context->capture_length) {
      context->capture_enabled = false;
      break;
    }
    if (length != 0U && ngx_buf_in_memory(buffer)) {
      ngx_memcpy(context->capture + context->capture_length, buffer->pos,
                 length);
    } else if (length != 0U && buffer->in_file) {
      if (ngx_read_file(buffer->file,
                        context->capture + context->capture_length, length,
                        buffer->file_pos) != (ssize_t)length) {
        context->capture_enabled = false;
        break;
      }
    } else if (length != 0U) {
      context->capture_enabled = false;
      break;
    }
    context->capture_length += length;
    if (context->html_capture || context->css_capture) {
      if (ngx_buf_in_memory(buffer)) {
        buffer->pos = buffer->last;
      }
      if (buffer->in_file) {
        buffer->file_pos = buffer->file_last;
      }
    }
    if (buffer->last_buf || buffer->last_in_chain) {
      final_buffer = true;
    }
  }

  if (context->css_capture) {
    if (!final_buffer) {
      return NGX_OK;
    }
    if (context->capture_enabled) {
      laghu_runtime_css_result rewritten;
      char origin[LAGHU_RUNTIME_PATH_SIZE];
      char css_path[LAGHU_RUNTIME_PATH_SIZE];
      const char *origin_value = NULL;
      unsigned char *selected = context->capture;
      size_t selected_length = context->capture_length;
      ngx_buf_t *buffer;
      ngx_chain_t output;
      conf = ngx_http_get_module_loc_conf(request, ngx_http_laghu_module);
      if (request->uri.len < sizeof(css_path)) {
        ngx_memcpy(css_path, request->uri.data, request->uri.len);
        css_path[request->uri.len] = '\0';
      } else {
        css_path[0] = '\0';
      }
      if (request->headers_in.host != NULL &&
          request->headers_in.host->value.len + sizeof("https://") <
              sizeof(origin)) {
        const char *scheme = "http://";
#if (NGX_HTTP_SSL)
        if (request->connection->ssl != NULL) {
          scheme = "https://";
        }
#endif
        (void)ngx_snprintf((u_char *)origin, sizeof(origin), "%s%V%Z", scheme,
                           &request->headers_in.host->value);
        origin_value = origin;
      }
      if (laghu_runtime_rewrite_css(
              &conf->runtime_queue, (const char *)conf->image_cache.data,
              (laghu_buffer){context->capture, context->capture_length},
              css_path, origin_value, context->policy_key,
              conf->runtime_queue.capabilities, (uint64_t)ngx_time(),
              conf->core.image_metadata_ttl,
              (context->policy.filter_families & LAGHU_FILTER_CSS_MINIFY) != 0U,
              context->policy.allow_structural_rewrite,
              conf->core.css_inline_limit, conf->core.css_outline_threshold,
              &rewritten)) {
        if (rewritten.rewritten) {
          unsigned char *copy = ngx_pnalloc(request->pool, rewritten.length);
          if (copy != NULL) {
            ngx_table_elt_t *etag = request->headers_out.etag;
            ngx_memcpy(copy, rewritten.data, rewritten.length);
            selected = copy;
            selected_length = rewritten.length;
            if (etag == NULL) {
              etag = ngx_list_push(&request->headers_out.headers);
            }
            if (etag != NULL) {
              u_char *value =
                  ngx_pnalloc(request->pool, sizeof("\"laghu-css-\"") +
                                                 LAGHU_SHA256_HEX_LENGTH);
              if (value != NULL) {
                etag->hash = 1U;
                ngx_str_set(&etag->key, "ETag");
                etag->value.data = value;
                etag->value.len =
                    (size_t)(ngx_snprintf(value,
                                          sizeof("\"laghu-css-\"") +
                                              LAGHU_SHA256_HEX_LENGTH,
                                          "\"laghu-css-%s\"",
                                          rewritten.dependency_key) -
                             value);
                request->headers_out.etag = etag;
              }
            }
          }
        }
        laghu_runtime_css_result_release(&rewritten);
      }
      request->headers_out.content_length_n = (off_t)selected_length;
      {
        ngx_list_part_t *part = &request->headers_out.headers.part;
        ngx_table_elt_t *headers = part->elts;
        ngx_uint_t index;
        for (index = 0U;; ++index) {
          if (index >= part->nelts) {
            if (part->next == NULL) {
              break;
            }
            part = part->next;
            headers = part->elts;
            index = 0U;
          }
          if (headers[index].hash != 0U &&
              ((headers[index].key.len == sizeof("Content-MD5") - 1U &&
                ngx_strncasecmp(headers[index].key.data,
                                (u_char *)"Content-MD5",
                                sizeof("Content-MD5") - 1U) == 0) ||
               (headers[index].key.len == sizeof("Digest") - 1U &&
                ngx_strncasecmp(headers[index].key.data, (u_char *)"Digest",
                                sizeof("Digest") - 1U) == 0))) {
            headers[index].hash = 0U;
          }
        }
      }
      if (request->headers_out.content_length != NULL) {
        request->headers_out.content_length->hash = 0U;
        request->headers_out.content_length = NULL;
      }
      context->header_deferred = false;
      if (ngx_http_laghu_next_header_filter(request) == NGX_ERROR) {
        return NGX_ERROR;
      }
      buffer = ngx_calloc_buf(request->pool);
      if (buffer == NULL) {
        return NGX_ERROR;
      }
      buffer->pos = selected;
      buffer->last = selected + selected_length;
      buffer->memory = 1U;
      buffer->last_buf = request == request->main;
      buffer->last_in_chain = 1U;
      output.buf = buffer;
      output.next = NULL;
      context->capture_enabled = false;
      return ngx_http_laghu_next_body_filter(request, &output);
    }
    context->header_deferred = false;
    if (ngx_http_laghu_next_header_filter(request) == NGX_ERROR) {
      return NGX_ERROR;
    }
    return ngx_http_laghu_next_body_filter(request, chain);
  }

  if (context->html_capture) {
    if (!final_buffer) {
      return NGX_OK;
    }
    if (context->capture_enabled) {
      laghu_runtime_html_result rewritten;
      laghu_image_filter_mask filters =
          ngx_http_laghu_image_filters(&context->policy);
      char origin[LAGHU_RUNTIME_PATH_SIZE];
      char page_path[LAGHU_RUNTIME_PATH_SIZE];
      const char *origin_value = NULL;
      unsigned char *selected = context->capture;
      size_t selected_length = context->capture_length;
      ngx_buf_t *buffer;
      ngx_chain_t output;
      ngx_table_elt_t *etag;
      char existing_language[LAGHU_HTML_LANGUAGE_SIZE];
      char existing_links[8192U];
      conf = ngx_http_get_module_loc_conf(request, ngx_http_laghu_module);
      if (request->uri.len < sizeof(page_path)) {
        ngx_memcpy(page_path, request->uri.data, request->uri.len);
        page_path[request->uri.len] = '\0';
      } else {
        page_path[0] = '\0';
      }
      if (request->headers_in.host != NULL &&
          request->headers_in.host->value.len + sizeof("https://") <
              sizeof(origin)) {
        const char *scheme = "http://";
#if (NGX_HTTP_SSL)
        if (request->connection->ssl != NULL) {
          scheme = "https://";
        }
#endif
        (void)ngx_snprintf((u_char *)origin, sizeof(origin), "%s%V%Z", scheme,
                           &request->headers_in.host->value);
        origin_value = origin;
      }
      ngx_http_laghu_response_header_values(request, "Content-Language",
                                            existing_language,
                                            sizeof(existing_language));
      ngx_http_laghu_response_header_values(request, "Link", existing_links,
                                            sizeof(existing_links));
      if (laghu_runtime_rewrite_html(
              ngx_http_laghu_rum, (const char *)conf->image_cache.data,
              (laghu_buffer){context->capture, context->capture_length},
              page_path, origin_value, context->policy_key,
              conf->runtime_queue.capabilities, (uint64_t)ngx_time(),
              conf->core.image_metadata_ttl, filters,
              context->policy.allow_resource_inlining,
              (context->policy.filter_families &
               LAGHU_FILTER_RESOURCE_INLINE) != 0U &&
                  context->policy.allow_resource_inlining,
              context->policy.allow_structural_rewrite,
              (context->policy.filter_families & LAGHU_FILTER_CSS_MINIFY) !=
                      0U &&
                  context->policy.allow_structural_rewrite,
              ngx_http_laghu_html_plan(&context->policy),
              ngx_http_laghu_csp_allows_data(request),
              ngx_http_laghu_csp_allows_inline_style(request),
              ngx_http_laghu_csp_allows_self_style(request, origin_value),
              conf->core.image_beacon == LAGHU_MODE_ON,
              conf->core.image_inline_limit, conf->core.css_inline_limit,
              conf->core.css_outline_threshold,
              ngx_http_laghu_viewport_header(request),
              ngx_http_laghu_dpr_header(request), &rewritten)) {
        laghu_runtime_html_result critical = {0};
        bool base_rewritten = rewritten.rewritten;
        char base_dependency[LAGHU_RUNTIME_KEY_SIZE];
        laghu_runtime_html_result finalized;
        laghu_runtime_html_result hinted;
        bool finalized_ok;
        bool hinted_ok;
        memcpy(base_dependency, rewritten.dependency_key,
               sizeof(base_dependency));
        if (base_rewritten) {
          selected = ngx_pnalloc(request->pool, rewritten.length);
          if (selected != NULL) {
            ngx_memcpy(selected, rewritten.data, rewritten.length);
            selected_length = rewritten.length;
          }
        }
        if ((context->policy.filter_families & LAGHU_FILTER_CRITICAL_CSS) !=
                0U &&
            laghu_runtime_prioritize_critical_css(
                ngx_http_laghu_rum, (const char *)conf->image_cache.data,
                (laghu_buffer){selected, selected_length}, page_path,
                origin_value, context->policy_key,
                conf->runtime_queue.capabilities, (uint64_t)ngx_time(),
                conf->core.image_metadata_ttl, conf->core.css_inline_limit,
                conf->core.css_outline_threshold,
                ngx_http_laghu_viewport_header(request),
                conf->core.critical_css_beacon == LAGHU_MODE_ON,
                ngx_http_laghu_csp_allows_inline_style(request),
                ngx_http_laghu_csp_allows_self_style(request, origin_value),
                ngx_http_laghu_csp_allows_self_script(request, origin_value),
                &critical) &&
            critical.rewritten) {
          unsigned char *copy = ngx_pnalloc(request->pool, critical.length);
          if (copy != NULL) {
            ngx_memcpy(copy, critical.data, critical.length);
            selected = copy;
            selected_length = critical.length;
            base_rewritten = true;
            {
              char material[LAGHU_RUNTIME_KEY_SIZE * 2U + 2U];
              int length = snprintf(material, sizeof(material), "%s\n%s",
                                    base_dependency, critical.dependency_key);
              if (length > 0 && (size_t)length < sizeof(material))
                (void)laghu_sha256_hex(
                    (laghu_buffer){(const unsigned char *)material,
                                   (size_t)length},
                    base_dependency);
            }
          }
        }
        laghu_runtime_html_result_release(&critical);
        laghu_runtime_html_result_release(&rewritten);
        hinted_ok = laghu_runtime_finalize_html_headers(
            (const char *)conf->image_cache.data,
            (laghu_buffer){context->capture, context->capture_length},
            page_path, origin_value, context->policy_key,
            conf->runtime_queue.capabilities, (uint64_t)ngx_time(),
            conf->core.image_metadata_ttl,
            ngx_http_laghu_html_plan(&context->policy) &
                LAGHU_HTML_PLAN_RESOURCE_HINTS,
            existing_language, existing_links, conf->core.css_inline_limit,
            conf->core.css_outline_threshold, base_rewritten, &hinted);
        finalized_ok = laghu_runtime_finalize_html_headers(
            (const char *)conf->image_cache.data,
            (laghu_buffer){selected, selected_length}, page_path, origin_value,
            context->policy_key, conf->runtime_queue.capabilities,
            (uint64_t)ngx_time(), conf->core.image_metadata_ttl,
            ngx_http_laghu_html_plan(&context->policy) &
                ~LAGHU_HTML_PLAN_RESOURCE_HINTS,
            existing_language, existing_links, conf->core.css_inline_limit,
            conf->core.css_outline_threshold, base_rewritten, &finalized);
        if (hinted_ok && finalized_ok) {
          const char *dependency = base_dependency;
          if (hinted.invalid || hinted.dependencies_pending ||
              finalized.invalid || finalized.dependencies_pending) {
            selected = context->capture;
            selected_length = context->capture_length;
            base_rewritten = false;
          } else {
            unsigned int header_index;
            if (finalized.rewritten) {
              unsigned char *copy =
                  ngx_pnalloc(request->pool, finalized.length);
              if (copy == NULL) {
                selected = context->capture;
                selected_length = context->capture_length;
                base_rewritten = false;
              } else {
                ngx_memcpy(copy, finalized.data, finalized.length);
                selected = copy;
                selected_length = finalized.length;
                base_rewritten = true;
              }
            }
            for (header_index = 0U; header_index < hinted.link_header_count;
                 ++header_index) {
              memcpy(finalized.link_headers[header_index],
                     hinted.link_headers[header_index],
                     LAGHU_HTML_HEADER_VALUE_SIZE);
            }
            finalized.link_header_count = hinted.link_header_count;
            {
              char material[LAGHU_RUNTIME_KEY_SIZE * 3U + 4U];
              int material_length = snprintf(
                  material, sizeof(material), "%s\n%s\n%s", base_dependency,
                  hinted.dependency_key, finalized.dependency_key);
              if (material_length > 0 &&
                  (size_t)material_length < sizeof(material) &&
                  laghu_sha256_hex(
                      (laghu_buffer){(const unsigned char *)material,
                                     (size_t)material_length},
                      finalized.dependency_key)) {
                dependency = finalized.dependency_key;
              }
            }
            if ((finalized.link_header_count != 0U ||
                 finalized.set_content_language) &&
                !ngx_http_laghu_apply_html_headers(request, &finalized)) {
              selected = context->capture;
              selected_length = context->capture_length;
              base_rewritten = false;
            }
          }
          if ((base_rewritten || finalized.link_header_count != 0U ||
               finalized.set_content_language) &&
              dependency[0] != '\0') {
            etag = request->headers_out.etag;
            if (etag == NULL) {
              etag = ngx_list_push(&request->headers_out.headers);
            }
            if (etag != NULL) {
              u_char *value =
                  ngx_pnalloc(request->pool, sizeof("\"laghu-html-\"") +
                                                 LAGHU_SHA256_HEX_LENGTH);
              if (value != NULL) {
                etag->hash = 1U;
                ngx_str_set(&etag->key, "ETag");
                etag->value.data = value;
                etag->value.len =
                    (size_t)(ngx_snprintf(value,
                                          sizeof("\"laghu-html-\"") +
                                              LAGHU_SHA256_HEX_LENGTH,
                                          "\"laghu-html-%s\"", dependency) -
                             value);
                request->headers_out.etag = etag;
              }
            }
          }
          laghu_runtime_html_result_release(&hinted);
          laghu_runtime_html_result_release(&finalized);
        } else {
          laghu_runtime_html_result_release(&hinted);
          laghu_runtime_html_result_release(&finalized);
          selected = context->capture;
          selected_length = context->capture_length;
        }
      }
      request->headers_out.content_length_n = (off_t)selected_length;
      if (request->headers_out.content_length != NULL) {
        request->headers_out.content_length->hash = 0U;
        request->headers_out.content_length = NULL;
      }
      context->header_deferred = false;
      if (ngx_http_laghu_next_header_filter(request) == NGX_ERROR) {
        return NGX_ERROR;
      }
      buffer = ngx_calloc_buf(request->pool);
      if (buffer == NULL) {
        return NGX_ERROR;
      }
      buffer->pos = selected;
      buffer->last = selected + selected_length;
      buffer->memory = 1U;
      buffer->last_buf = request == request->main;
      buffer->last_in_chain = 1U;
      output.buf = buffer;
      output.next = NULL;
      context->capture_enabled = false;
      return ngx_http_laghu_next_body_filter(request, &output);
    }
    context->header_deferred = false;
    if (ngx_http_laghu_next_header_filter(request) == NGX_ERROR) {
      return NGX_ERROR;
    }
    return ngx_http_laghu_next_body_filter(request, chain);
  }

  if (context->capture_enabled && final_buffer &&
      context->capture_length != 0U) {
    laghu_runtime_job job;

    conf = ngx_http_get_module_loc_conf(request, ngx_http_laghu_module);
    memset(&job, 0, sizeof(job));
    if (request->uri.len < sizeof(job.request_path) &&
        request->headers_out.content_type.len < sizeof(job.content_type) &&
        conf->runtime_queue.mapping != NULL) {
      ngx_memcpy(job.request_path, request->uri.data, request->uri.len);
      job.request_path[request->uri.len] = '\0';
      ngx_memcpy(job.content_type, request->headers_out.content_type.data,
                 request->headers_out.content_type.len);
      job.content_type[request->headers_out.content_type.len] = '\0';
      ngx_cpystrn((u_char *)job.index_key, (u_char *)context->index_key,
                  sizeof(job.index_key));
      ngx_cpystrn((u_char *)job.validator, (u_char *)context->validator,
                  sizeof(job.validator));
      ngx_cpystrn((u_char *)job.policy_key, (u_char *)context->policy_key,
                  sizeof(job.policy_key));
      job.filters = ngx_http_laghu_image_filters(&context->policy);
      job.quality = context->policy.image_quality != 0U
                        ? context->policy.image_quality
                        : 100U;
      job.metadata_limit = conf->core.image_metadata_limit;
      job.metadata_ttl = conf->core.image_metadata_ttl;
      job.allow_lossy = context->policy.allow_lossy;
      job.accept_webp = context->accept_webp;
      job.target_count = context->target_count;
      ngx_memcpy(job.target_width, context->target_width,
                 sizeof(job.target_width));
      ngx_memcpy(job.target_height, context->target_height,
                 sizeof(job.target_height));
      ngx_memcpy(job.resize_filter, context->resize_filter,
                 sizeof(job.resize_filter));
      job.payload = (laghu_buffer){context->capture, context->capture_length};
      if (!laghu_runtime_queue_try_publish(&conf->runtime_queue, &job)) {
        time_t now = ngx_time();
        if (ngx_http_laghu_last_queue_warning == 0 ||
            now - ngx_http_laghu_last_queue_warning >= 60) {
          ngx_http_laghu_last_queue_warning = now;
          ngx_log_error(NGX_LOG_WARN, request->connection->log, 0,
                        "laghu image queue is full; preserving the original");
        }
      }
    }
    context->capture_enabled = false;
  }
  return ngx_http_laghu_next_body_filter(request, chain);
}

static ngx_int_t ngx_http_laghu_transaction_header_filter(
    ngx_http_request_t *request) {
  ngx_http_laghu_loc_conf_t *conf =
      ngx_http_get_module_loc_conf(request, ngx_http_laghu_module);
  ngx_http_laghu_request_ctx_t *context;
  laghu_http_transaction_result result;
  bool prepared;
  if (request->uri.len >= sizeof("/.laghu/") - 1U &&
      ngx_strncmp(request->uri.data, "/.laghu/", sizeof("/.laghu/") - 1U) ==
          0) {
    return ngx_http_laghu_next_header_filter(request);
  }
  if (conf->core.mode != LAGHU_MODE_ON) {
    return ngx_http_laghu_next_header_filter(request);
  }
  context = ngx_pcalloc(request->pool, sizeof(*context));
  if (context == NULL || !ngx_http_laghu_normalize(request, conf, context)) {
    return ngx_http_laghu_next_header_filter(request);
  }
  prepared = laghu_http_transaction_prepare(
      &context->transaction, &context->request, &context->response,
      &context->environment, &result);
  if (ngx_http_laghu_apply_result(request, &result) != NGX_OK) {
    laghu_http_transaction_result_release(&result);
    return ngx_http_laghu_next_header_filter(request);
  }
  if (!prepared || result.action == LAGHU_HTTP_ACTION_BYPASS) {
    laghu_http_transaction_result_release(&result);
    return ngx_http_laghu_next_header_filter(request);
  }
  {
    laghu_cache_limits limits = {
        .size_limit = conf->file_cache_size,
        .inode_limit = conf->file_cache_inode_limit,
        .metadata_size = conf->file_cache_metadata_size,
        .clean_interval = (unsigned int)conf->file_cache_clean_interval};
    if (!laghu_cache_backend_register_path((const char *)conf->image_cache.data,
                                           &limits)) {
      ngx_log_error(NGX_LOG_WARN, request->connection->log, 0,
                    "laghu file cache backend unavailable; serving origin");
      laghu_http_transaction_result_release(&result);
      return ngx_http_laghu_next_header_filter(request);
    }
  }
  if (result.action == LAGHU_HTTP_ACTION_SERVE_CACHED) {
    ngx_buf_t *buffer;
    context->cached_body = ngx_pnalloc(request->pool, result.selected.length);
    context->cached_output = ngx_alloc_chain_link(request->pool);
    buffer = ngx_calloc_buf(request->pool);
    if (context->cached_body == NULL || context->cached_output == NULL ||
        buffer == NULL) {
      laghu_http_transaction_result_release(&result);
      return ngx_http_laghu_next_header_filter(request);
    }
    ngx_memcpy(context->cached_body, result.selected.data,
               result.selected.length);
    buffer->pos = context->cached_body;
    buffer->last = context->cached_body + result.selected.length;
    buffer->memory = 1U;
    buffer->last_buf = request == request->main;
    buffer->last_in_chain = 1U;
    context->cached_output->buf = buffer;
    context->cached_output->next = NULL;
    context->cache_hit = true;
    ngx_http_set_ctx(request, context, ngx_http_laghu_module);
    laghu_http_transaction_result_release(&result);
    return ngx_http_laghu_next_header_filter(request);
  }
  context->capture_capacity = result.capture_limit;
  if (context->response.has_declared_length &&
      context->response.declared_length < context->capture_capacity) {
    context->capture_capacity = context->response.declared_length;
  }
  context->capture = ngx_pnalloc(request->pool, context->capture_capacity);
  if (context->capture == NULL) {
    laghu_http_transaction_result_release(&result);
    return ngx_http_laghu_next_header_filter(request);
  }
  context->capture_enabled = true;
  context->html_capture = result.action == LAGHU_HTTP_ACTION_CAPTURE_HTML;
  context->css_capture = result.action == LAGHU_HTTP_ACTION_CAPTURE_CSS;
  context->header_deferred =
      context->html_capture || context->css_capture ||
      result.action == LAGHU_HTTP_ACTION_CAPTURE_JAVASCRIPT;
  request->filter_need_in_memory = 1U;
  ngx_http_set_ctx(request, context, ngx_http_laghu_module);
  laghu_http_transaction_result_release(&result);
  return context->header_deferred ? NGX_OK
                                  : ngx_http_laghu_next_header_filter(request);
}

static ngx_int_t ngx_http_laghu_transaction_body_filter(
    ngx_http_request_t *request, ngx_chain_t *chain) {
  ngx_http_laghu_request_ctx_t *context =
      ngx_http_get_module_ctx(request, ngx_http_laghu_module);
  ngx_chain_t *current;
  bool final_buffer = false;
  if (context == NULL) {
    return ngx_http_laghu_next_body_filter(request, chain);
  }
  if (context->cache_hit) {
    if (context->cache_sent || chain == NULL) {
      return NGX_OK;
    }
    context->cache_sent = true;
    return ngx_http_laghu_next_body_filter(request, context->cached_output);
  }
  if (!context->capture_enabled || chain == NULL) {
    return ngx_http_laghu_next_body_filter(request, chain);
  }
  for (current = chain; current != NULL; current = current->next) {
    ngx_buf_t *buffer = current->buf;
    size_t length = (size_t)ngx_buf_size(buffer);
    if (length > context->capture_capacity - context->capture_length) {
      context->capture_enabled = false;
      break;
    }
    if (length != 0U && ngx_buf_in_memory(buffer)) {
      ngx_memcpy(context->capture + context->capture_length, buffer->pos,
                 length);
    } else if (length != 0U && buffer->in_file) {
      if (ngx_read_file(buffer->file,
                        context->capture + context->capture_length, length,
                        buffer->file_pos) != (ssize_t)length) {
        context->capture_enabled = false;
        break;
      }
    } else if (length != 0U) {
      context->capture_enabled = false;
      break;
    }
    context->capture_length += length;
    if (context->header_deferred) {
      if (ngx_buf_in_memory(buffer)) {
        buffer->pos = buffer->last;
      }
      if (buffer->in_file) {
        buffer->file_pos = buffer->file_last;
      }
    }
    if (buffer->last_buf || buffer->last_in_chain) {
      final_buffer = true;
    }
  }
  if (!final_buffer) {
    return context->header_deferred
               ? NGX_OK
               : ngx_http_laghu_next_body_filter(request, chain);
  }
  if (context->capture_enabled) {
    laghu_http_transaction_result result;
    (void)laghu_http_transaction_finalize(
        &context->transaction,
        (laghu_buffer){context->capture, context->capture_length}, &result);
    ngx_http_laghu_log_defer_recommendation(request, &result);
    if (context->header_deferred) {
      const unsigned char *selected = result.selected.data;
      size_t selected_length = result.selected.length;
      ngx_buf_t *buffer;
      ngx_chain_t output;
      unsigned char *copy = ngx_pnalloc(request->pool, selected_length);
      if (copy == NULL ||
          ngx_http_laghu_apply_result(request, &result) != NGX_OK) {
        laghu_http_transaction_result_release(&result);
        return NGX_ERROR;
      }
      ngx_memcpy(copy, selected, selected_length);
      laghu_http_transaction_result_release(&result);
      context->header_deferred = false;
      context->capture_enabled = false;
      if (ngx_http_laghu_next_header_filter(request) == NGX_ERROR) {
        return NGX_ERROR;
      }
      buffer = ngx_calloc_buf(request->pool);
      if (buffer == NULL) {
        return NGX_ERROR;
      }
      buffer->pos = copy;
      buffer->last = copy + selected_length;
      buffer->memory = 1U;
      buffer->last_buf = request == request->main;
      buffer->last_in_chain = 1U;
      output.buf = buffer;
      output.next = NULL;
      return ngx_http_laghu_next_body_filter(request, &output);
    }
    if (!result.job_published &&
        context->transaction.action == LAGHU_HTTP_ACTION_CAPTURE_IMAGE) {
      time_t now = ngx_time();
      if (ngx_http_laghu_last_queue_warning == 0 ||
          now - ngx_http_laghu_last_queue_warning >= 60) {
        ngx_http_laghu_last_queue_warning = now;
        ngx_log_error(NGX_LOG_WARN, request->connection->log, 0,
                      "laghu image queue is full; preserving the original");
      }
    }
    laghu_http_transaction_result_release(&result);
  }
  context->capture_enabled = false;
  return ngx_http_laghu_next_body_filter(request, chain);
}

static ngx_int_t ngx_http_laghu_filter_init(ngx_conf_t *configuration) {
  ngx_http_core_main_conf_t *core;
  ngx_http_handler_pt *handler;

  core =
      ngx_http_conf_get_module_main_conf(configuration, ngx_http_core_module);
  handler = ngx_array_push(&core->phases[NGX_HTTP_CONTENT_PHASE].handlers);
  if (handler == NULL) {
    return NGX_ERROR;
  }
  *handler = ngx_http_laghu_variant_handler;

  ngx_http_laghu_next_header_filter = ngx_http_top_header_filter;
  ngx_http_top_header_filter = ngx_http_laghu_transaction_header_filter;

  ngx_http_laghu_next_body_filter = ngx_http_top_body_filter;
  ngx_http_top_body_filter = ngx_http_laghu_transaction_body_filter;

  return NGX_OK;
}

static ngx_int_t ngx_http_laghu_admin_json(ngx_http_request_t *request,
                                           ngx_uint_t status,
                                           const char *json) {
  ngx_buf_t *buffer;
  ngx_chain_t output;
  ngx_table_elt_t *header = ngx_list_push(&request->headers_out.headers);
  size_t length = strlen(json);
  if (header == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
  header->hash = 1U;
  ngx_str_set(&header->key, "Cache-Control");
  ngx_str_set(&header->value, "no-store");
  request->headers_out.status = status;
  ngx_str_set(&request->headers_out.content_type, "application/json");
  request->headers_out.content_length_n = (off_t)length;
  if (ngx_http_send_header(request) == NGX_ERROR ||
      request->method == NGX_HTTP_HEAD)
    return NGX_OK;
  buffer = ngx_calloc_buf(request->pool);
  if (buffer == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
  buffer->pos = (u_char *)json;
  buffer->last = (u_char *)json + length;
  buffer->memory = 1U;
  buffer->last_buf = 1U;
  output.buf = buffer;
  output.next = NULL;
  return ngx_http_output_filter(request, &output);
}

static ngx_int_t ngx_http_laghu_admin_text(ngx_http_request_t *request,
                                           const char *text, size_t length) {
  ngx_buf_t *buffer;
  ngx_chain_t output;
  ngx_table_elt_t *header = ngx_list_push(&request->headers_out.headers);
  if (header == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
  header->hash = 1U;
  ngx_str_set(&header->key, "Cache-Control");
  ngx_str_set(&header->value, "no-store");
  request->headers_out.status = NGX_HTTP_OK;
  ngx_str_set(&request->headers_out.content_type,
              "text/plain; version=0.0.4; charset=utf-8");
  request->headers_out.content_length_n = (off_t)length;
  if (ngx_http_send_header(request) == NGX_ERROR ||
      request->method == NGX_HTTP_HEAD)
    return NGX_OK;
  buffer = ngx_calloc_buf(request->pool);
  if (buffer == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
  buffer->pos = (u_char *)text;
  buffer->last = (u_char *)text + length;
  buffer->memory = 1U;
  buffer->last_buf = 1U;
  output.buf = buffer;
  output.next = NULL;
  return ngx_http_output_filter(request, &output);
}

static ngx_int_t ngx_http_laghu_variant_handler(ngx_http_request_t *request) {
  static const char prefix[] = "/.laghu/image/";
  static const char css_prefix[] = "/.laghu/css/";
  static const char javascript_prefix[] = "/.laghu/js/";
  static const char media_prefix[] = "/.laghu/media/";
  static const char beacon_script_path[] = "/.laghu/beacon/images.js";
  static const char beacon_post_path[] = "/.laghu/beacon/images";
  static const char critical_script_path[] = "/.laghu/beacon/critical-css.js";
  static const char critical_post_path[] = "/.laghu/beacon/critical-css";
  static const char instrumentation_script_path[] =
      "/.laghu/beacon/instrumentation.js";
  static const char instrumentation_post_path[] =
      "/.laghu/beacon/instrumentation";
  static const unsigned char beacon_script[] =
      "addEventListener('load',()=>{document.querySelectorAll('img[src]')."
      "forEach"
      "(i=>{const r=i.getBoundingClientRect();if(r.width<1||r.height<1)return;"
      "fetch('/.laghu/beacon/images',{method:'POST',headers:{'Content-Type':"
      "'application/json'},body:JSON.stringify({url:new "
      "URL(i.currentSrc||i.src,"
      "location.href).pathname,width:Math.round(r.width),height:Math.round(r."
      "height),viewport_width:innerWidth,dpr_hundredths:Math.min(400,Math.max("
      "100,Math.round(devicePixelRatio*100))),above_fold:r.top<innerHeight,"
      "mobile:innerWidth<768}),keepalive:true})})});";
  ngx_http_laghu_loc_conf_t *conf;
  laghu_runtime_cache_entry entry;
  ngx_buf_t *buffer;
  ngx_chain_t output;
  ngx_table_elt_t *header;
  unsigned char *body;
  char key[LAGHU_RUNTIME_KEY_SIZE];
  bool css_asset = false;
  bool javascript_asset = false;
  bool javascript_map = false;
  bool media_asset = false;

  conf = ngx_http_get_module_loc_conf(request, ngx_http_laghu_module);
  {
    char target[LAGHU_RUNTIME_PATH_SIZE];
    char normalized[LAGHU_RUNTIME_PATH_SIZE];
    bool control = false;
    bool normalized_ok = request->unparsed_uri.len < sizeof(target);
    bool purge_request;
    bool stats_request;
    bool metrics_request;
    bool readiness_request;
    if (normalized_ok) {
      ngx_memcpy(target, request->unparsed_uri.data, request->unparsed_uri.len);
      target[request->unparsed_uri.len] = '\0';
      normalized_ok = laghu_cache_source_normalize(
          target, normalized, sizeof(normalized), &control);
    } else {
      target[0] = '\0';
    }
    purge_request = request->method_name.len == sizeof("PURGE") - 1U &&
                    ngx_strncmp(request->method_name.data, "PURGE",
                                sizeof("PURGE") - 1U) == 0;
    purge_request =
        purge_request || control ||
        (target[0] != '\0' && strstr(target, "laghu=purge") != NULL);
    stats_request = normalized_ok && strcmp(normalized, "/.laghu/stats") == 0;
    metrics_request =
        normalized_ok && strcmp(normalized, "/.laghu/metrics") == 0;
    readiness_request =
        normalized_ok && strcmp(normalized, "/.laghu/ready") == 0;
    if ((metrics_request && !conf->metrics) ||
        (readiness_request && !conf->readiness))
      return NGX_HTTP_NOT_FOUND;
    if (purge_request || stats_request || metrics_request ||
        readiness_request) {
      ngx_table_elt_t *token = NULL;
      ngx_list_part_t *part = &request->headers_in.headers.part;
      ngx_table_elt_t *headers = part->elts;
      ngx_uint_t header_index;
      bool authorized = false;
      for (header_index = 0U;; ++header_index) {
        if (header_index >= part->nelts) {
          if (part->next == NULL) break;
          part = part->next;
          headers = part->elts;
          header_index = 0U;
        }
        if (headers[header_index].key.len ==
                sizeof("X-Laghu-Purge-Token") - 1U &&
            ngx_strncasecmp(headers[header_index].key.data,
                            (u_char *)"X-Laghu-Purge-Token",
                            sizeof("X-Laghu-Purge-Token") - 1U) == 0) {
          token = &headers[header_index];
          break;
        }
      }
      if (token != NULL && conf->purge_token_file.len != 0U &&
          conf->purge_allow != NULL &&
          ngx_cidr_match(request->connection->sockaddr, conf->purge_allow) ==
              NGX_OK) {
        ngx_file_t file;
        u_char expected[257U];
        ssize_t length;
        size_t supplied = token->value.len, maximum, compare_index;
        u_char difference;
        ngx_memzero(&file, sizeof(file));
        file.name = conf->purge_token_file;
        file.fd = ngx_open_file(conf->purge_token_file.data, NGX_FILE_RDONLY,
                                NGX_FILE_OPEN, 0U);
        length = file.fd == NGX_INVALID_FILE
                     ? -1
                     : ngx_read_file(&file, expected, sizeof(expected), 0U);
        if (file.fd != NGX_INVALID_FILE) ngx_close_file(file.fd);
        while (length > 0 &&
               (expected[length - 1] == '\n' || expected[length - 1] == '\r'))
          --length;
        maximum =
            length > 0 && (size_t)length > supplied ? (size_t)length : supplied;
        difference = (u_char)((length > 0 ? (size_t)length : 0U) ^ supplied);
        for (compare_index = 0U; compare_index < maximum; ++compare_index)
          difference |=
              (u_char)((compare_index < (size_t)(length > 0 ? length : 0)
                            ? expected[compare_index]
                            : 0U) ^
                       (compare_index < supplied
                            ? token->value.data[compare_index]
                            : 0U));
        authorized = length >= 16 && length < (ssize_t)sizeof(expected) &&
                     difference == 0U;
      }
      if (!normalized_ok)
        return ngx_http_laghu_admin_json(request, NGX_HTTP_BAD_REQUEST,
                                         "{\"status\":\"malformed\"}");
      if (!authorized)
        return ngx_http_laghu_admin_json(request, NGX_HTTP_FORBIDDEN,
                                         "{\"status\":\"forbidden\"}");
      {
        laghu_cache_limits limits = {
            .size_limit = conf->file_cache_size,
            .inode_limit = conf->file_cache_inode_limit,
            .metadata_size = conf->file_cache_metadata_size,
            .clean_interval = (unsigned int)conf->file_cache_clean_interval};
        if (!laghu_cache_backend_register_path(
                (const char *)conf->image_cache.data, &limits))
          return ngx_http_laghu_admin_json(request,
                                           NGX_HTTP_SERVICE_UNAVAILABLE,
                                           "{\"status\":\"unavailable\"}");
      }
      if (stats_request) {
        laghu_cache_stats stats = {0};
        char *json;
        if (!conf->statistics || (request->method != NGX_HTTP_GET &&
                                  request->method != NGX_HTTP_HEAD))
          return NGX_HTTP_NOT_ALLOWED;
        if (!laghu_cache_backend_health_path((char *)conf->image_cache.data,
                                             &stats))
          return NGX_HTTP_SERVICE_UNAVAILABLE;
        json = ngx_pnalloc(request->pool, 1024U);
        if (json == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
        (void)snprintf(
            json, 1024U,
            "{\"schema\":\"laghu-cache-stats-v1\",\"backend\":\"file\","
            "\"bytes\":%llu,\"files\":%llu,\"hits\":%llu,"
            "\"misses\":%llu,\"publications\":%llu,"
            "\"rejected_writes\":%llu,\"evictions\":%llu,"
            "\"url_purges\":%llu,\"full_purges\":%llu,"
            "\"generation\":%llu}",
            (unsigned long long)stats.bytes, (unsigned long long)stats.files,
            (unsigned long long)stats.hits, (unsigned long long)stats.misses,
            (unsigned long long)stats.publications,
            (unsigned long long)stats.rejected_publications,
            (unsigned long long)stats.evictions,
            (unsigned long long)stats.url_purges,
            (unsigned long long)stats.full_purges,
            (unsigned long long)stats.cache_generation);
        return ngx_http_laghu_admin_json(request, NGX_HTTP_OK, json);
      }
      if (metrics_request || readiness_request) {
        laghu_operational_snapshot *snapshot;
        char *rendered;
        size_t length = 0U;
        bool enabled = metrics_request ? conf->metrics : conf->readiness;
        (void)laghu_operational_registry_heartbeat(
            &ngx_http_laghu_operational, (uint64_t)ngx_time(), true, 0U, 0U);
        if (!enabled || (request->method != NGX_HTTP_GET &&
                         request->method != NGX_HTTP_HEAD))
          return NGX_HTTP_NOT_ALLOWED;
        snapshot = ngx_pcalloc(request->pool, sizeof(*snapshot));
        rendered = ngx_pnalloc(request->pool, LAGHU_OPERATIONAL_RENDER_SIZE);
        if (snapshot == NULL || rendered == NULL)
          return NGX_HTTP_INTERNAL_SERVER_ERROR;
        if (!laghu_operational_registry_snapshot(&ngx_http_laghu_operational,
                                                 snapshot))
          return ngx_http_laghu_admin_json(request,
                                           NGX_HTTP_SERVICE_UNAVAILABLE,
                                           "{\"status\":\"unavailable\"}");
        if (metrics_request) {
          if (!laghu_operational_render_prometheus(
                  snapshot, (uint64_t)ngx_time(), rendered,
                  LAGHU_OPERATIONAL_RENDER_SIZE, &length))
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
          return ngx_http_laghu_admin_text(request, rendered, length);
        } else {
          laghu_operational_readiness readiness;
          laghu_cache_stats stats = {0};
          bool cache_ready = laghu_cache_backend_health_path(
              (char *)conf->image_cache.data, &stats);
          ngx_uint_t status;
          laghu_operational_registry_cache(&ngx_http_laghu_operational, &stats);
          if (!laghu_operational_readiness_evaluate(
                  snapshot, (uint64_t)ngx_time(), true, cache_ready,
                  conf->readiness_strict, &readiness) ||
              !laghu_operational_render_readiness(
                  &readiness, conf->readiness_strict, rendered,
                  LAGHU_OPERATIONAL_RENDER_SIZE, &length))
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
          status = readiness.runtime_ready && readiness.cache_ready &&
                           readiness.workers_ready
                       ? NGX_HTTP_OK
                       : NGX_HTTP_SERVICE_UNAVAILABLE;
          return ngx_http_laghu_admin_json(request, status, rendered);
        }
      }
      if ((!conf->purge_method && request->method_name.len == 5U) ||
          (!conf->purge_query && control))
        return NGX_HTTP_NOT_ALLOWED;
      {
        uint64_t matched = 0U;
        laghu_cache_purge_result result = laghu_cache_backend_purge_url_path(
            (char *)conf->image_cache.data, normalized, (uint64_t)ngx_time(),
            &matched);
        char *json = ngx_pnalloc(request->pool, 192U);
        ngx_uint_t status = result == LAGHU_CACHE_PURGE_ACCEPTED
                                ? NGX_HTTP_ACCEPTED
                                : (result == LAGHU_CACHE_PURGE_SATURATED
                                       ? NGX_HTTP_TOO_MANY_REQUESTS
                                       : NGX_HTTP_SERVICE_UNAVAILABLE);
        if (json == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
        (void)snprintf(
            json, 192U, "{\"status\":\"%s\",\"matched_artifacts\":%llu}",
            result == LAGHU_CACHE_PURGE_ACCEPTED ? "accepted" : "rejected",
            (unsigned long long)matched);
        return ngx_http_laghu_admin_json(request, status, json);
      }
    }
  }
  if (request->uri.len == sizeof(beacon_script_path) - 1U &&
      ngx_strncmp(request->uri.data, beacon_script_path,
                  sizeof(beacon_script_path) - 1U) == 0) {
    if (conf->core.mode != LAGHU_MODE_ON ||
        conf->core.image_beacon != LAGHU_MODE_ON) {
      return NGX_HTTP_NOT_FOUND;
    }
    request->headers_out.status = NGX_HTTP_OK;
    ngx_str_set(&request->headers_out.content_type, "application/javascript");
    request->headers_out.content_length_n = sizeof(beacon_script) - 1U;
    if (ngx_http_send_header(request) == NGX_ERROR ||
        request->method == NGX_HTTP_HEAD) {
      return NGX_OK;
    }
    buffer = ngx_calloc_buf(request->pool);
    if (buffer == NULL) {
      return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    buffer->pos = (u_char *)beacon_script;
    buffer->last = (u_char *)beacon_script + sizeof(beacon_script) - 1U;
    buffer->memory = 1U;
    buffer->last_buf = 1U;
    output.buf = buffer;
    output.next = NULL;
    return ngx_http_output_filter(request, &output);
  }
  if (request->uri.len == sizeof(beacon_post_path) - 1U &&
      ngx_strncmp(request->uri.data, beacon_post_path,
                  sizeof(beacon_post_path) - 1U) == 0) {
    time_t now = ngx_time();
    if (conf->core.mode != LAGHU_MODE_ON ||
        conf->core.image_beacon != LAGHU_MODE_ON) {
      return NGX_HTTP_NOT_FOUND;
    }
    if (request->method != NGX_HTTP_POST ||
        request->headers_in.content_type == NULL ||
        request->headers_in.content_type->value.len <
            sizeof("application/json") - 1U ||
        ngx_strncasecmp(request->headers_in.content_type->value.data,
                        (u_char *)"application/json",
                        sizeof("application/json") - 1U) != 0 ||
        request->headers_in.content_length_n <= 0 ||
        request->headers_in.content_length_n > 16384 ||
        !ngx_http_laghu_same_origin(request)) {
      return NGX_HTTP_BAD_REQUEST;
    }
    if (ngx_http_laghu_beacon_window != now) {
      ngx_http_laghu_beacon_window = now;
      ngx_http_laghu_beacon_count = 0U;
    }
    if (++ngx_http_laghu_beacon_count > 32U) {
      return NGX_HTTP_TOO_MANY_REQUESTS;
    }
    if (ngx_http_read_client_request_body(
            request, ngx_http_laghu_beacon_body) >= NGX_HTTP_SPECIAL_RESPONSE) {
      return NGX_HTTP_BAD_REQUEST;
    }
    return NGX_DONE;
  }
  if (request->uri.len == sizeof(critical_script_path) - 1U &&
      ngx_strncmp(request->uri.data, critical_script_path,
                  sizeof(critical_script_path) - 1U) == 0) {
    const char *script = laghu_runtime_critical_css_beacon_script();
    size_t script_length = strlen(script);
    if (conf->core.mode != LAGHU_MODE_ON ||
        conf->core.critical_css_beacon != LAGHU_MODE_ON ||
        request->method != NGX_HTTP_GET)
      return NGX_HTTP_NOT_FOUND;
    request->headers_out.status = NGX_HTTP_OK;
    ngx_str_set(&request->headers_out.content_type, "application/javascript");
    request->headers_out.content_length_n = (off_t)script_length;
    if (ngx_http_send_header(request) == NGX_ERROR) return NGX_OK;
    buffer = ngx_calloc_buf(request->pool);
    if (buffer == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
    buffer->pos = (u_char *)script;
    buffer->last = (u_char *)script + script_length;
    buffer->memory = 1U;
    buffer->last_buf = 1U;
    output.buf = buffer;
    output.next = NULL;
    return ngx_http_output_filter(request, &output);
  }
  if (request->uri.len == sizeof(critical_post_path) - 1U &&
      ngx_strncmp(request->uri.data, critical_post_path,
                  sizeof(critical_post_path) - 1U) == 0) {
    time_t now = ngx_time();
    if (conf->core.mode != LAGHU_MODE_ON ||
        conf->core.critical_css_beacon != LAGHU_MODE_ON)
      return NGX_HTTP_NOT_FOUND;
    if (request->method != NGX_HTTP_POST ||
        request->headers_in.content_type == NULL ||
        request->headers_in.content_type->value.len <
            sizeof("application/json") - 1U ||
        ngx_strncasecmp(request->headers_in.content_type->value.data,
                        (u_char *)"application/json",
                        sizeof("application/json") - 1U) != 0 ||
        request->headers_in.content_length_n <= 0 ||
        request->headers_in.content_length_n > 16384 ||
        !ngx_http_laghu_same_origin(request))
      return NGX_HTTP_BAD_REQUEST;
    if (ngx_http_laghu_beacon_window != now) {
      ngx_http_laghu_beacon_window = now;
      ngx_http_laghu_beacon_count = 0U;
    }
    if (++ngx_http_laghu_beacon_count > 32U) return NGX_HTTP_TOO_MANY_REQUESTS;
    if (ngx_http_read_client_request_body(
            request, ngx_http_laghu_beacon_body) >= NGX_HTTP_SPECIAL_RESPONSE)
      return NGX_HTTP_BAD_REQUEST;
    return NGX_DONE;
  }
  if (request->uri.len == sizeof(instrumentation_script_path) - 1U &&
      ngx_strncmp(request->uri.data, instrumentation_script_path,
                  sizeof(instrumentation_script_path) - 1U) == 0) {
    const char *script = laghu_runtime_instrumentation_script();
    size_t script_length = strlen(script);
    if (conf->core.mode != LAGHU_MODE_ON ||
        conf->core.instrumentation_beacon != LAGHU_MODE_ON ||
        request->method != NGX_HTTP_GET)
      return NGX_HTTP_NOT_FOUND;
    request->headers_out.status = NGX_HTTP_OK;
    ngx_str_set(&request->headers_out.content_type, "application/javascript");
    request->headers_out.content_length_n = (off_t)script_length;
    if (ngx_http_send_header(request) == NGX_ERROR) return NGX_OK;
    buffer = ngx_calloc_buf(request->pool);
    if (buffer == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
    buffer->pos = (u_char *)script;
    buffer->last = (u_char *)script + script_length;
    buffer->memory = 1U;
    buffer->last_buf = 1U;
    output.buf = buffer;
    output.next = NULL;
    return ngx_http_output_filter(request, &output);
  }
  if (request->uri.len == sizeof(instrumentation_post_path) - 1U &&
      ngx_strncmp(request->uri.data, instrumentation_post_path,
                  sizeof(instrumentation_post_path) - 1U) == 0) {
    time_t now = ngx_time();
    if (conf->core.mode != LAGHU_MODE_ON ||
        conf->core.instrumentation_beacon != LAGHU_MODE_ON)
      return NGX_HTTP_NOT_FOUND;
    if (request->method != NGX_HTTP_POST ||
        request->headers_in.content_type == NULL ||
        request->headers_in.content_type->value.len <
            sizeof("application/json") - 1U ||
        ngx_strncasecmp(request->headers_in.content_type->value.data,
                        (u_char *)"application/json",
                        sizeof("application/json") - 1U) != 0 ||
        request->headers_in.content_length_n <= 0 ||
        request->headers_in.content_length_n > 16384 ||
        !ngx_http_laghu_same_origin(request))
      return NGX_HTTP_BAD_REQUEST;
    if (ngx_http_laghu_beacon_window != now) {
      ngx_http_laghu_beacon_window = now;
      ngx_http_laghu_beacon_count = 0U;
    }
    if (++ngx_http_laghu_beacon_count > 32U) return NGX_HTTP_TOO_MANY_REQUESTS;
    if (ngx_http_read_client_request_body(
            request, ngx_http_laghu_beacon_body) >= NGX_HTTP_SPECIAL_RESPONSE)
      return NGX_HTTP_BAD_REQUEST;
    return NGX_DONE;
  }
  css_asset =
      request->uri.len == sizeof(css_prefix) - 1U + LAGHU_SHA256_HEX_LENGTH &&
      ngx_strncmp(request->uri.data, css_prefix, sizeof(css_prefix) - 1U) == 0;
  javascript_asset = request->uri.len == sizeof(javascript_prefix) - 1U +
                                             LAGHU_SHA256_HEX_LENGTH &&
                     ngx_strncmp(request->uri.data, javascript_prefix,
                                 sizeof(javascript_prefix) - 1U) == 0;
  javascript_map =
      request->uri.len ==
          sizeof(javascript_prefix) - 1U + LAGHU_SHA256_HEX_LENGTH + 4U &&
      ngx_strncmp(request->uri.data, javascript_prefix,
                  sizeof(javascript_prefix) - 1U) == 0 &&
      ngx_strncmp(request->uri.data + request->uri.len - 4U, ".map", 4U) == 0;
  media_asset =
      request->uri.len == sizeof(media_prefix) - 1U + LAGHU_SHA256_HEX_LENGTH &&
      ngx_strncmp(request->uri.data, media_prefix, sizeof(media_prefix) - 1U) ==
          0;
  if (!css_asset && !javascript_asset && !javascript_map && !media_asset &&
      (request->uri.len != sizeof(prefix) - 1U + LAGHU_SHA256_HEX_LENGTH ||
       ngx_strncmp(request->uri.data, prefix, sizeof(prefix) - 1U) != 0)) {
    return NGX_DECLINED;
  }
  if (!(request->method & (NGX_HTTP_GET | NGX_HTTP_HEAD))) {
    return NGX_HTTP_NOT_ALLOWED;
  }
  if (conf->core.mode != LAGHU_MODE_ON) {
    return NGX_HTTP_NOT_FOUND;
  }
  ngx_memcpy(key,
             request->uri.data + (css_asset ? sizeof(css_prefix) - 1U
                                  : (javascript_asset || javascript_map)
                                      ? sizeof(javascript_prefix) - 1U
                                  : media_asset ? sizeof(media_prefix) - 1U
                                                : sizeof(prefix) - 1U),
             LAGHU_SHA256_HEX_LENGTH);
  key[LAGHU_SHA256_HEX_LENGTH] = '\0';
  {
    size_t offset;
    for (offset = 0U; offset < LAGHU_SHA256_HEX_LENGTH; ++offset) {
      if (!((key[offset] >= '0' && key[offset] <= '9') ||
            (key[offset] >= 'a' && key[offset] <= 'f'))) {
        return NGX_HTTP_NOT_FOUND;
      }
    }
  }
  {
    ngx_http_laghu_request_ctx_t transaction_context;
    laghu_http_transaction_result result;
    size_t operation_index;
    ngx_memzero(&transaction_context, sizeof(transaction_context));
    ngx_memzero(&result, sizeof(result));
    request->headers_out.status = NGX_HTTP_OK;
    request->headers_out.content_length_n = 0;
    if (!ngx_http_laghu_normalize(request, conf, &transaction_context) ||
        !laghu_http_transaction_prepare(
            &transaction_context.transaction, &transaction_context.request,
            &transaction_context.response, &transaction_context.environment,
            &result) ||
        result.action != LAGHU_HTTP_ACTION_SERVE_CACHED ||
        result.selected.length == 0U ||
        result.selected.length > LAGHU_IMAGE_MAX_INPUT_BYTES) {
      laghu_http_transaction_result_release(&result);
      return NGX_HTTP_NOT_FOUND;
    }
    body = ngx_pnalloc(request->pool, result.selected.length);
    if (body == NULL ||
        ngx_http_laghu_apply_result(request, &result) != NGX_OK) {
      laghu_http_transaction_result_release(&result);
      return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ngx_memcpy(body, result.selected.data, result.selected.length);
    entry.length = result.selected.length;
    entry.content_type[0] = '\0';
    for (operation_index = 0U; operation_index < result.header_operation_count;
         ++operation_index) {
      if (strcmp(result.header_operations[operation_index].name,
                 "Content-Type") == 0 &&
          result.header_operations[operation_index].value != NULL) {
        ngx_cpystrn((u_char *)entry.content_type,
                    (u_char *)result.header_operations[operation_index].value,
                    sizeof(entry.content_type));
      }
    }
    laghu_http_transaction_result_release(&result);
  }
  request->headers_out.status = NGX_HTTP_OK;
  request->headers_out.content_length_n = (off_t)entry.length;
  request->headers_out.content_type.data =
      (u_char *)(css_asset        ? "text/css"
                 : javascript_map ? "application/json"
                                  : entry.content_type);
  request->headers_out.content_type.len =
      ngx_strlen(request->headers_out.content_type.data);
  header = ngx_list_push(&request->headers_out.headers);
  if (header == NULL) {
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }
  header->hash = 1U;
  ngx_str_set(&header->key, "Cache-Control");
  ngx_str_set(&header->value, "public, max-age=31536000, immutable");
  header = ngx_list_push(&request->headers_out.headers);
  if (header == NULL) {
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }
  header->hash = 1U;
  ngx_str_set(&header->key, "ETag");
  header->value.data = ngx_pnalloc(request->pool, LAGHU_SHA256_HEX_LENGTH + 3U);
  if (header->value.data == NULL) {
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }
  header->value.len = LAGHU_SHA256_HEX_LENGTH + 2U;
  header->value.data[0] = '"';
  ngx_memcpy(header->value.data + 1U, key, LAGHU_SHA256_HEX_LENGTH);
  header->value.data[LAGHU_SHA256_HEX_LENGTH + 1U] = '"';
  header->value.data[LAGHU_SHA256_HEX_LENGTH + 2U] = '\0';
  if (ngx_http_send_header(request) == NGX_ERROR ||
      request->method == NGX_HTTP_HEAD) {
    return NGX_OK;
  }
  buffer = ngx_calloc_buf(request->pool);
  if (buffer == NULL) {
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }
  buffer->pos = body;
  buffer->last = body + entry.length;
  buffer->memory = 1U;
  buffer->last_buf = 1U;
  output.buf = buffer;
  output.next = NULL;
  return ngx_http_output_filter(request, &output);
}

static void ngx_http_laghu_beacon_body(ngx_http_request_t *request) {
  ngx_http_laghu_loc_conf_t *conf =
      ngx_http_get_module_loc_conf(request, ngx_http_laghu_module);
  laghu_image_beacon_record beacon;
  laghu_critical_css_beacon critical;
  laghu_instrumentation_beacon instrumentation;
  laghu_policy policy;
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned char *body;
  size_t length = (size_t)request->headers_in.content_length_n;
  size_t offset = 0U;
  ngx_chain_t *chain;
  bool valid = false;
  body = ngx_pnalloc(request->pool, length);
  if (body != NULL && request->request_body != NULL) {
    for (chain = request->request_body->bufs; chain != NULL;
         chain = chain->next) {
      size_t part = (size_t)ngx_buf_size(chain->buf);
      if (!ngx_buf_in_memory(chain->buf) || part > length - offset) {
        offset = 0U;
        break;
      }
      ngx_memcpy(body + offset, chain->buf->pos, part);
      offset += part;
    }
    valid = offset == length &&
            laghu_resolve_config_policy(&conf->core, &policy) &&
            laghu_variant_key((laghu_buffer){NULL, 0U}, &policy, policy_key);
    if (valid &&
        request->uri.len == sizeof("/.laghu/beacon/instrumentation") - 1U)
      valid = laghu_runtime_parse_instrumentation_beacon(
                  (laghu_buffer){body, length}, &instrumentation) &&
              laghu_instrumentation_apply_beacon(
                  ngx_http_laghu_rum, (const char *)conf->image_cache.data,
                  (uint64_t)ngx_time(), conf->core.image_metadata_ttl,
                  &instrumentation);
    else if (valid &&
             request->uri.len == sizeof("/.laghu/beacon/critical-css") - 1U)
      valid = laghu_runtime_parse_critical_css_beacon(
                  (laghu_buffer){body, length}, &critical) &&
              laghu_critical_css_apply_beacon(
                  ngx_http_laghu_rum, (const char *)conf->image_cache.data,
                  policy_key, (uint64_t)ngx_time(),
                  conf->core.image_metadata_ttl, &critical);
    else if (valid)
      valid = laghu_runtime_parse_image_beacon((laghu_buffer){body, length},
                                               &beacon) &&
              ngx_http_laghu_queue_refresh(conf) &&
              laghu_catalog_apply_beacon(
                  ngx_http_laghu_rum, (const char *)conf->image_cache.data,
                  policy_key, conf->runtime_queue.capabilities,
                  (uint64_t)ngx_time(), conf->core.image_metadata_ttl, &beacon);
  }
  request->headers_out.status =
      valid ? NGX_HTTP_NO_CONTENT : NGX_HTTP_BAD_REQUEST;
  request->headers_out.content_length_n = 0;
  ngx_http_finalize_request(request, ngx_http_send_header(request));
}

static void *ngx_http_laghu_create_loc_conf(ngx_conf_t *configuration) {
  ngx_http_laghu_loc_conf_t *conf;
  ngx_pool_cleanup_t *cleanup;

  conf = ngx_pcalloc(configuration->pool, sizeof(ngx_http_laghu_loc_conf_t));
  if (conf == NULL) {
    return NULL;
  }

  laghu_config_init(&conf->core);
  laghu_source_policy_init(&conf->source_policy);
  laghu_runtime_queue_init(&conf->runtime_queue);
  laghu_runtime_queue_init(&conf->font_fetch_runtime_queue);
  laghu_runtime_queue_init(&conf->javascript_runtime_queue);
  conf->file_cache_size = NGX_CONF_UNSET_SIZE;
  conf->file_cache_inode_limit = NGX_CONF_UNSET_SIZE;
  conf->file_cache_metadata_size = NGX_CONF_UNSET_SIZE;
  conf->file_cache_clean_interval = NGX_CONF_UNSET_UINT;
  conf->purge_method = NGX_CONF_UNSET;
  conf->purge_query = NGX_CONF_UNSET;
  conf->statistics = NGX_CONF_UNSET;
  conf->metrics = NGX_CONF_UNSET;
  conf->readiness = NGX_CONF_UNSET;
  conf->readiness_strict = NGX_CONF_UNSET;
  cleanup = ngx_pool_cleanup_add(configuration->pool, 0);
  if (cleanup == NULL) {
    return NULL;
  }
  cleanup->handler = ngx_http_laghu_queue_cleanup;
  cleanup->data = &conf->runtime_queue;
  cleanup = ngx_pool_cleanup_add(configuration->pool, 0);
  if (cleanup == NULL) {
    return NULL;
  }
  cleanup->handler = ngx_http_laghu_queue_cleanup;
  cleanup->data = &conf->font_fetch_runtime_queue;
  cleanup = ngx_pool_cleanup_add(configuration->pool, 0);
  if (cleanup == NULL) return NULL;
  cleanup->handler = ngx_http_laghu_queue_cleanup;
  cleanup->data = &conf->javascript_runtime_queue;
  return conf;
}

static char *ngx_http_laghu_merge_loc_conf(ngx_conf_t *configuration,
                                           void *parent, void *child) {
  ngx_http_laghu_loc_conf_t *parent_conf = parent;
  ngx_http_laghu_loc_conf_t *child_conf = child;
  laghu_config merged;
  laghu_policy policy;
  laghu_source_policy source;
  char source_error[160U];
  ngx_http_laghu_main_conf_t *main_conf;

  (void)configuration;

  if (!laghu_resource_rules_merge_valid(&parent_conf->core, &child_conf->core))
    return "invalid, duplicate, conflicting, or excessive inherited resource "
           "rules";
  laghu_config_merge(&merged, &parent_conf->core, &child_conf->core);
  if (!laghu_resolve_config_policy(&merged, &policy))
    return "invalid or conflicting inherited laghu filter policy";
  child_conf->core = merged;
  ngx_conf_merge_str_value(child_conf->worker_queue, parent_conf->worker_queue,
                           LAGHU_NGINX_DEFAULT_QUEUE);
  ngx_conf_merge_str_value(child_conf->font_fetch_queue,
                           parent_conf->font_fetch_queue,
                           LAGHU_NGINX_DEFAULT_FONT_QUEUE);
  ngx_conf_merge_str_value(child_conf->font_provider_config,
                           parent_conf->font_provider_config, "");
  ngx_conf_merge_str_value(child_conf->javascript_queue,
                           parent_conf->javascript_queue,
                           LAGHU_NGINX_DEFAULT_JAVASCRIPT_QUEUE);
  ngx_conf_merge_str_value(child_conf->javascript_target,
                           parent_conf->javascript_target,
                           "defaults and supports es6-module and not dead");
  ngx_conf_merge_str_value(child_conf->javascript_observation_config,
                           parent_conf->javascript_observation_config, "");
  ngx_conf_merge_str_value(child_conf->javascript_defer_config,
                           parent_conf->javascript_defer_config, "");
  if (!child_conf->font_providers_loaded &&
      parent_conf->font_providers_loaded) {
    child_conf->font_providers = parent_conf->font_providers;
    child_conf->font_providers_loaded = true;
  }
  if (!child_conf->javascript_observations_loaded &&
      parent_conf->javascript_observations_loaded) {
    child_conf->javascript_observations = parent_conf->javascript_observations;
    child_conf->javascript_observations_loaded = true;
  }
  if (!child_conf->javascript_defer_loaded &&
      parent_conf->javascript_defer_loaded) {
    child_conf->javascript_defer = parent_conf->javascript_defer;
    child_conf->javascript_defer_loaded = true;
  }
  if (child_conf->file_cache_backend.len == 0U &&
      !child_conf->image_cache_set) {
    child_conf->file_cache_backend = parent_conf->file_cache_backend;
    child_conf->image_cache = parent_conf->image_cache;
    child_conf->image_cache_set = parent_conf->image_cache_set;
  }
  if (child_conf->image_cache.len == 0U) {
    ngx_str_set(&child_conf->image_cache, LAGHU_NGINX_DEFAULT_CACHE);
  }
  ngx_conf_merge_size_value(child_conf->file_cache_size,
                            parent_conf->file_cache_size,
                            (size_t)LAGHU_CACHE_DEFAULT_SIZE_BYTES);
  ngx_conf_merge_size_value(child_conf->file_cache_inode_limit,
                            parent_conf->file_cache_inode_limit,
                            LAGHU_CACHE_DEFAULT_INODE_LIMIT);
  ngx_conf_merge_size_value(child_conf->file_cache_metadata_size,
                            parent_conf->file_cache_metadata_size,
                            LAGHU_CACHE_DEFAULT_METADATA_BYTES);
  ngx_conf_merge_uint_value(child_conf->file_cache_clean_interval,
                            parent_conf->file_cache_clean_interval,
                            LAGHU_CACHE_DEFAULT_CLEAN_INTERVAL);
  ngx_conf_merge_value(child_conf->purge_method, parent_conf->purge_method, 0);
  ngx_conf_merge_value(child_conf->purge_query, parent_conf->purge_query, 0);
  ngx_conf_merge_value(child_conf->statistics, parent_conf->statistics, 0);
  ngx_conf_merge_value(child_conf->metrics, parent_conf->metrics, 0);
  ngx_conf_merge_value(child_conf->readiness, parent_conf->readiness, 0);
  ngx_conf_merge_value(child_conf->readiness_strict,
                       parent_conf->readiness_strict, 0);
  main_conf =
      ngx_http_conf_get_module_main_conf(configuration, ngx_http_laghu_module);
  if (child_conf->metrics || child_conf->readiness) {
    if (main_conf->operational_cache.len == 0U)
      main_conf->operational_cache = child_conf->image_cache;
    else if (main_conf->operational_cache.len != child_conf->image_cache.len ||
             ngx_strncmp(main_conf->operational_cache.data,
                         child_conf->image_cache.data,
                         child_conf->image_cache.len) != 0)
      return "metrics and readiness locations must share one file cache";
  }
  ngx_conf_merge_str_value(child_conf->purge_token_file,
                           parent_conf->purge_token_file, "");
  ngx_conf_merge_str_value(child_conf->cache_flush_file,
                           parent_conf->cache_flush_file, "");
  if (child_conf->purge_allow == NULL)
    child_conf->purge_allow = parent_conf->purge_allow;
  if ((child_conf->purge_method || child_conf->purge_query ||
       child_conf->statistics || child_conf->metrics ||
       child_conf->readiness) &&
      (child_conf->purge_token_file.len == 0U ||
       child_conf->purge_allow == NULL || child_conf->purge_allow->nelts == 0U))
    return "network administration requires purge_token_file and purge_allow";
  if (child_conf->trusted_proxy == NULL)
    child_conf->trusted_proxy = parent_conf->trusted_proxy;
  if (child_conf->core.respect_x_forwarded_proto == LAGHU_MODE_ON &&
      (child_conf->trusted_proxy == NULL ||
       child_conf->trusted_proxy->nelts == 0U))
    return "respect_x_forwarded_proto requires trusted_proxy";
  ngx_conf_merge_str_value(child_conf->asset_offload_config,
                           parent_conf->asset_offload_config, "");
  ngx_conf_merge_str_value(child_conf->asset_upload_queue,
                           parent_conf->asset_upload_queue, "");
  if (!child_conf->asset_offload_loaded && parent_conf->asset_offload_loaded) {
    child_conf->asset_offload = parent_conf->asset_offload;
    child_conf->asset_offload_loaded = true;
  }
  if (child_conf->asset_offload_loaded &&
      (child_conf->asset_upload_queue.len == 0U ||
       ngx_strcmp(child_conf->asset_upload_queue.data,
                  child_conf->asset_offload.queue_path) != 0))
    return "asset_upload_queue must match the loaded asset offload config";
  if (!child_conf->asset_offload_loaded &&
      child_conf->asset_upload_queue.len != 0U)
    return "asset_upload_queue requires asset_offload_config";
  if (!laghu_source_policy_merge(&source, &parent_conf->source_policy,
                                 &child_conf->source_policy))
    return "invalid, duplicate, or excessive inherited file source mapping";
  if (child_conf->source_mode_set) source.mode = child_conf->source_policy.mode;
  if (source.mode == LAGHU_SOURCE_FILE_OFF) {
    source.mapping_count = 0U;
    source.native_root[0] = '\0';
  }
  if (source.mode == LAGHU_SOURCE_FILE_NATIVE ||
      source.mode == LAGHU_SOURCE_FILE_BOTH) {
    ngx_http_core_loc_conf_t *core_location =
        ngx_http_conf_get_module_loc_conf(configuration, ngx_http_core_module);
    if (core_location == NULL || core_location->alias != 0U ||
        core_location->root.len == 0U ||
        core_location->root.len >= sizeof(source.native_root) ||
        core_location->root_lengths != NULL)
      return "native file loading requires one static non-alias NGINX root";
    ngx_memcpy(source.native_root, core_location->root.data,
               core_location->root.len);
    source.native_root[core_location->root.len] = '\0';
  }
  if (!laghu_source_policy_validate(&source, true, source_error,
                                    sizeof(source_error)))
    return "invalid direct file loading configuration";
  child_conf->source_policy = source;
  if (source.mode != LAGHU_SOURCE_FILE_OFF) {
    if (!child_conf->asset_offload_loaded)
      return "direct file loading requires asset_offload_config";
    if (!laghu_source_registry_publish(
            (const char *)child_conf->asset_upload_queue.data, &source))
      return "unable to publish direct file source registry";
  }
  return NGX_CONF_OK;
}

static char *ngx_http_laghu_command(ngx_conf_t *configuration,
                                    ngx_command_t *command, void *conf) {
  ngx_http_laghu_loc_conf_t *location = conf;
  ngx_http_laghu_main_conf_t *main_conf =
      ngx_http_conf_get_module_main_conf(configuration, ngx_http_laghu_module);
  ngx_str_t *values = configuration->args->elts;

  (void)command;

  if (configuration->args->nelts == 4 &&
      ngx_strcmp(values[1].data, "file_source_map") == 0) {
    if (!laghu_source_mapping_add(&location->source_policy,
                                  (const char *)values[2].data,
                                  (const char *)values[3].data))
      return "file source mapping is invalid, duplicate, or excessive";
    return NGX_CONF_OK;
  }

  if (configuration->args->nelts == 3 && values[1].len >= 9U &&
      ngx_strncmp(values[1].data, "rum_store", 9U) == 0) {
    ngx_int_t parsed;
    uint32_t setting_bit = 0U;
    if (configuration->cmd_type != NGX_HTTP_MAIN_CONF)
      return "laghu rum_store settings are allowed only in the http context";
    if (ngx_strcmp(values[1].data, "rum_store") == 0)
      setting_bit = 1U << 0;
    else if (ngx_strcmp(values[1].data, "rum_store_local_snapshot") == 0)
      setting_bit = 1U << 1;
    else if (ngx_strcmp(values[1].data, "rum_store_client_library") == 0)
      setting_bit = 1U << 2;
    else if (ngx_strcmp(values[1].data, "rum_store_required") == 0)
      setting_bit = 1U << 3;
    else if (ngx_strcmp(values[1].data, "rum_store_timeout") == 0)
      setting_bit = 1U << 4;
    else if (ngx_strcmp(values[1].data, "rum_store_ttl") == 0)
      setting_bit = 1U << 5;
    else if (ngx_strcmp(values[1].data, "rum_store_retry_limit") == 0)
      setting_bit = 1U << 6;
    else if (ngx_strcmp(values[1].data, "rum_store_sync_interval") == 0)
      setting_bit = 1U << 7;
    else if (ngx_strcmp(values[1].data, "rum_store_memory_limit") == 0)
      setting_bit = 1U << 8;
    else if (ngx_strcmp(values[1].data, "rum_store_pending_limit") == 0)
      setting_bit = 1U << 9;
    if (setting_bit == 0U) return "unknown laghu rum_store setting";
    if ((main_conf->set_mask & setting_bit) != 0U)
      return "duplicate laghu rum_store setting";
    main_conf->set_mask |= setting_bit;
    if (ngx_strcmp(values[1].data, "rum_store") == 0) {
      if (!laghu_rum_store_validate((const char *)values[2].data, NULL, 0U))
        return "laghu rum_store URI is invalid or unsupported";
      main_conf->store_uri = values[2];
      return NGX_CONF_OK;
    }
    if (ngx_strcmp(values[1].data, "rum_store_local_snapshot") == 0) {
      main_conf->snapshot_path = values[2];
      return NGX_CONF_OK;
    }
    if (ngx_strcmp(values[1].data, "rum_store_client_library") == 0) {
      main_conf->client_library = values[2];
      return NGX_CONF_OK;
    }
    if (ngx_strcmp(values[1].data, "rum_store_required") == 0) {
      if (ngx_strcmp(values[2].data, "on") == 0)
        main_conf->required = 1;
      else if (ngx_strcmp(values[2].data, "off") == 0)
        main_conf->required = 0;
      else
        return "laghu rum_store_required expects on or off";
      return NGX_CONF_OK;
    }
    if (ngx_strcmp(values[1].data, "rum_store_memory_limit") == 0 ||
        ngx_strcmp(values[1].data, "rum_store_pending_limit") == 0) {
      ssize_t size = ngx_parse_size(&values[2]);
      if (size < (ssize_t)LAGHU_RUM_MAX_RECORD_BYTES)
        return "laghu RUM size limit is too small or invalid";
      if (ngx_strcmp(values[1].data, "rum_store_memory_limit") == 0)
        main_conf->memory_limit = (size_t)size;
      else
        main_conf->pending_limit = (size_t)size;
      return NGX_CONF_OK;
    }
    parsed = ngx_atoi(values[2].data, values[2].len);
    if (parsed == NGX_ERROR) return "laghu RUM setting expects an integer";
    if (ngx_strcmp(values[1].data, "rum_store_timeout") == 0 && parsed >= 10 &&
        parsed <= 10000)
      main_conf->timeout_ms = (ngx_uint_t)parsed;
    else if (ngx_strcmp(values[1].data, "rum_store_ttl") == 0 &&
             parsed >= 3600 && parsed <= 2592000)
      main_conf->ttl_seconds = (ngx_uint_t)parsed;
    else if (ngx_strcmp(values[1].data, "rum_store_retry_limit") == 0 &&
             parsed >= 0 && parsed <= 10)
      main_conf->retry_limit = (ngx_uint_t)parsed;
    else if (ngx_strcmp(values[1].data, "rum_store_sync_interval") == 0 &&
             parsed >= 1 && parsed <= 300)
      main_conf->sync_interval_seconds = (ngx_uint_t)parsed;
    else
      return "unknown or out-of-range laghu rum_store setting";
    return NGX_CONF_OK;
  }

  if (configuration->args->nelts == 2) {
    if (location->core.mode != LAGHU_MODE_UNSET) {
      return "is duplicate";
    }

    if (ngx_strcmp(values[1].data, "on") == 0) {
      location->core.mode = LAGHU_MODE_ON;
      return NGX_CONF_OK;
    }

    if (ngx_strcmp(values[1].data, "off") == 0) {
      location->core.mode = LAGHU_MODE_OFF;
      return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                       "laghu expects 'on', 'off', 'preset <name>', or "
                       "'rewrite_level <name>', or 'allow_api on|off'");
    return NGX_CONF_ERROR;
  }

  if (ngx_strcmp(values[1].data, "preset") == 0) {
    laghu_preset preset;

    if (location->core.preset != LAGHU_PRESET_UNSET) {
      return "is duplicate";
    }
    if (location->core.rewrite_level != LAGHU_REWRITE_LEVEL_UNSET) {
      return "cannot combine 'laghu preset' and 'laghu rewrite_level' in the "
             "same context";
    }

    if (laghu_parse_preset((const char *)values[2].data, &preset)) {
      location->core.preset = preset;
      return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                       "unknown laghu preset \"%V\"", &values[2]);
    return NGX_CONF_ERROR;
  }

  if (ngx_strcmp(values[1].data, "rewrite_level") == 0) {
    laghu_rewrite_level rewrite_level;

    if (location->core.rewrite_level != LAGHU_REWRITE_LEVEL_UNSET) {
      return "is duplicate";
    }
    if (location->core.preset != LAGHU_PRESET_UNSET) {
      return "cannot combine 'laghu preset' and 'laghu rewrite_level' in the "
             "same context";
    }

    if (laghu_parse_rewrite_level((const char *)values[2].data,
                                  &rewrite_level)) {
      location->core.rewrite_level = rewrite_level;
      return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                       "unknown laghu rewrite level \"%V\"", &values[2]);
    return NGX_CONF_ERROR;
  }

  if (ngx_strcmp(values[1].data, "enable_filter") == 0 ||
      ngx_strcmp(values[1].data, "disable_filter") == 0 ||
      ngx_strcmp(values[1].data, "forbid_filter") == 0) {
    uint32_t filter;
    uint32_t declared;

    if (!laghu_parse_filter((const char *)values[2].data, &filter)) {
      ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                         "unknown laghu filter \"%V\"", &values[2]);
      return NGX_CONF_ERROR;
    }
    declared = location->core.enabled_filters |
               location->core.disabled_filters |
               location->core.forbidden_filters;
    if ((declared & filter) != 0U)
      return "laghu filter is duplicated or conflicting in this context";
    if (ngx_strcmp(values[1].data, "enable_filter") == 0)
      location->core.enabled_filters |= filter;
    else if (ngx_strcmp(values[1].data, "disable_filter") == 0)
      location->core.disabled_filters |= filter;
    else
      location->core.forbidden_filters |= filter;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "allow_resources") == 0 ||
      ngx_strcmp(values[1].data, "disallow") == 0) {
    if (!laghu_resource_rule_add(
            &location->core, ngx_strcmp(values[1].data, "allow_resources") == 0,
            (const char *)values[2].data))
      return "resource rule is invalid, duplicate, conflicting, or excessive";
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "respect_vary") == 0 ||
      ngx_strcmp(values[1].data, "respect_x_forwarded_proto") == 0 ||
      ngx_strcmp(values[1].data, "query_filter_overrides") == 0) {
    laghu_mode *target =
        ngx_strcmp(values[1].data, "respect_vary") == 0
            ? &location->core.respect_vary
        : ngx_strcmp(values[1].data, "respect_x_forwarded_proto") == 0
            ? &location->core.respect_x_forwarded_proto
            : &location->core.query_filter_overrides;
    if (*target != LAGHU_MODE_UNSET) return "is duplicate";
    if (ngx_strcmp(values[2].data, "on") == 0)
      *target = LAGHU_MODE_ON;
    else if (ngx_strcmp(values[2].data, "off") == 0)
      *target = LAGHU_MODE_OFF;
    else
      return "expects on or off";
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "trusted_proxy") == 0) {
    ngx_cidr_t cidr;
    ngx_cidr_t *stored;
    if (ngx_ptocidr(&values[2], &cidr) != NGX_OK)
      return "trusted_proxy expects a canonical CIDR";
    if (location->trusted_proxy == NULL) {
      location->trusted_proxy =
          ngx_array_create(configuration->pool, 4U, sizeof(ngx_cidr_t));
      if (location->trusted_proxy == NULL) return NGX_CONF_ERROR;
    }
    stored = ngx_array_push(location->trusted_proxy);
    if (stored == NULL) return NGX_CONF_ERROR;
    *stored = cidr;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "allow_api") == 0) {
    if (location->core.allow_api != LAGHU_MODE_UNSET) {
      return "is duplicate";
    }

    if (ngx_strcmp(values[2].data, "on") == 0) {
      location->core.allow_api = LAGHU_MODE_ON;
      return NGX_CONF_OK;
    }

    if (ngx_strcmp(values[2].data, "off") == 0) {
      location->core.allow_api = LAGHU_MODE_OFF;
      return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                       "laghu allow_api expects 'on' or 'off'");
    return NGX_CONF_ERROR;
  }

  if (ngx_strcmp(values[1].data, "image_quality") == 0) {
    ngx_int_t quality;

    if (location->core.image_quality != LAGHU_IMAGE_QUALITY_UNSET) {
      return "is duplicate";
    }
    quality = ngx_atoi(values[2].data, values[2].len);
    if (quality < 1 || quality > 100) {
      ngx_conf_log_error(
          NGX_LOG_EMERG, configuration, 0,
          "laghu image_quality expects an integer from 1 to 100");
      return NGX_CONF_ERROR;
    }
    location->core.image_quality = (unsigned int)quality;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "image_beacon") == 0) {
    if (location->core.image_beacon != LAGHU_MODE_UNSET) {
      return "is duplicate";
    }
    if (ngx_strcmp(values[2].data, "on") == 0) {
      location->core.image_beacon = LAGHU_MODE_ON;
      return NGX_CONF_OK;
    }
    if (ngx_strcmp(values[2].data, "off") == 0) {
      location->core.image_beacon = LAGHU_MODE_OFF;
      return NGX_CONF_OK;
    }
    return "laghu image_beacon expects 'on' or 'off'";
  }
  if (ngx_strcmp(values[1].data, "critical_css_beacon") == 0) {
    if (location->core.critical_css_beacon != LAGHU_MODE_UNSET)
      return "duplicate laghu critical_css_beacon";
    if (ngx_strcmp(values[2].data, "on") == 0)
      location->core.critical_css_beacon = LAGHU_MODE_ON;
    else if (ngx_strcmp(values[2].data, "off") == 0)
      location->core.critical_css_beacon = LAGHU_MODE_OFF;
    else
      return "laghu critical_css_beacon expects 'on' or 'off'";
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "instrumentation_beacon") == 0) {
    if (location->core.instrumentation_beacon != LAGHU_MODE_UNSET)
      return "duplicate laghu instrumentation_beacon";
    if (ngx_strcmp(values[2].data, "on") == 0)
      location->core.instrumentation_beacon = LAGHU_MODE_ON;
    else if (ngx_strcmp(values[2].data, "off") == 0)
      location->core.instrumentation_beacon = LAGHU_MODE_OFF;
    else
      return "laghu instrumentation_beacon expects 'on' or 'off'";
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "instrumentation_sample_rate") == 0) {
    ngx_int_t rate = ngx_atoi(values[2].data, values[2].len);
    if (location->core.instrumentation_sample_rate !=
        LAGHU_INSTRUMENTATION_SAMPLE_RATE_UNSET)
      return "duplicate laghu instrumentation_sample_rate";
    if (rate < 0 || rate > 100)
      return "laghu instrumentation_sample_rate expects 0 to 100";
    location->core.instrumentation_sample_rate = (unsigned int)rate;
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "javascript_defer_suggestions") == 0) {
    if (location->core.javascript_defer_suggestions != LAGHU_MODE_UNSET)
      return "duplicate laghu javascript_defer_suggestions";
    if (ngx_strcmp(values[2].data, "on") == 0)
      location->core.javascript_defer_suggestions = LAGHU_MODE_ON;
    else if (ngx_strcmp(values[2].data, "off") == 0)
      location->core.javascript_defer_suggestions = LAGHU_MODE_OFF;
    else
      return "laghu javascript_defer_suggestions expects 'on' or 'off'";
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "include_js_source_maps") == 0) {
    if (location->core.include_js_source_maps != LAGHU_MODE_UNSET)
      return "duplicate laghu include_js_source_maps";
    if (ngx_strcmp(values[2].data, "on") == 0)
      location->core.include_js_source_maps = LAGHU_MODE_ON;
    else if (ngx_strcmp(values[2].data, "off") == 0)
      location->core.include_js_source_maps = LAGHU_MODE_OFF;
    else
      return "laghu include_js_source_maps expects 'on' or 'off'";
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "image_inline_limit") == 0) {
    ngx_int_t limit = ngx_atoi(values[2].data, values[2].len);
    if (location->core.image_inline_limit != LAGHU_IMAGE_INLINE_LIMIT_UNSET) {
      return "is duplicate";
    }
    if (limit < 0 || limit > 16384) {
      return "laghu image_inline_limit expects an integer from 0 to 16384";
    }
    location->core.image_inline_limit = (unsigned int)limit;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "image_metadata_limit") == 0) {
    ngx_int_t limit = ngx_atoi(values[2].data, values[2].len);
    if (location->core.image_metadata_limit !=
        LAGHU_IMAGE_METADATA_LIMIT_UNSET) {
      return "is duplicate";
    }
    if (limit < 1 || limit > 100000) {
      return "laghu image_metadata_limit expects an integer from 1 to 100000";
    }
    location->core.image_metadata_limit = (unsigned int)limit;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "image_metadata_ttl") == 0) {
    time_t ttl;
    if (location->core.image_metadata_ttl != LAGHU_IMAGE_METADATA_TTL_UNSET) {
      return "is duplicate";
    }
    ttl = ngx_parse_time(&values[2], 1);
    if (ttl == (time_t)NGX_ERROR || ttl < 3600 || ttl > 2592000) {
      return "laghu image_metadata_ttl expects a duration from 1h to 30d";
    }
    location->core.image_metadata_ttl = (unsigned int)ttl;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "css_inline_limit") == 0) {
    ngx_int_t limit = ngx_atoi(values[2].data, values[2].len);
    if (location->core.css_inline_limit != LAGHU_CSS_INLINE_LIMIT_UNSET) {
      return "is duplicate";
    }
    if (limit < 0 || limit > 65536) {
      return "laghu css_inline_limit expects an integer from 0 to 65536";
    }
    location->core.css_inline_limit = (unsigned int)limit;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "css_outline_threshold") == 0) {
    ngx_int_t threshold = ngx_atoi(values[2].data, values[2].len);
    if (location->core.css_outline_threshold !=
        LAGHU_CSS_OUTLINE_THRESHOLD_UNSET) {
      return "is duplicate";
    }
    if (threshold < 1024 || threshold > 1048576) {
      return "laghu css_outline_threshold expects an integer from 1024 to "
             "1048576";
    }
    location->core.css_outline_threshold = (unsigned int)threshold;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "javascript_inline_limit") == 0) {
    ngx_int_t limit = ngx_atoi(values[2].data, values[2].len);
    if (location->core.javascript_inline_limit !=
        LAGHU_JAVASCRIPT_INLINE_LIMIT_UNSET) {
      return "is duplicate";
    }
    if (limit < 0 || limit > 65536) {
      return "laghu javascript_inline_limit expects an integer from 0 to 65536";
    }
    location->core.javascript_inline_limit = (unsigned int)limit;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "javascript_outline_threshold") == 0) {
    ngx_int_t threshold = ngx_atoi(values[2].data, values[2].len);
    if (location->core.javascript_outline_threshold !=
        LAGHU_JAVASCRIPT_OUTLINE_THRESHOLD_UNSET) {
      return "is duplicate";
    }
    if (threshold < 1024 || threshold > 1048576) {
      return "laghu javascript_outline_threshold expects an integer from 1024 "
             "to 1048576";
    }
    location->core.javascript_outline_threshold = (unsigned int)threshold;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "worker_queue") == 0) {
    if (location->worker_queue.len != 0U) {
      return "is duplicate";
    }
    location->worker_queue = values[2];
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "asset_offload_config") == 0) {
    char error[256] = "configuration appears more than once";
    if (location->asset_offload_config.len != 0U ||
        !laghu_asset_config_load((const char *)values[2].data,
                                 &location->asset_offload, error,
                                 sizeof(error))) {
      ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                         "invalid asset offload config \"%V\": %s", &values[2],
                         error);
      return NGX_CONF_ERROR;
    }
    location->asset_offload_config = values[2];
    location->asset_offload_loaded = true;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "asset_upload_queue") == 0) {
    if (location->asset_upload_queue.len != 0U) return "is duplicate";
    location->asset_upload_queue = values[2];
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "load_from_file") == 0) {
    if (location->source_mode_set ||
        !laghu_source_mode_parse((const char *)values[2].data, true,
                                 &location->source_policy.mode))
      return "load_from_file expects off, mapped, native, or both once";
    location->source_mode_set = true;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "font_fetch_queue") == 0) {
    if (location->font_fetch_queue.len != 0U) {
      return "is duplicate";
    }
    location->font_fetch_queue = values[2];
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "font_provider_config") == 0) {
    char error[256] = "configuration appears more than once";
    if (location->font_provider_config.len != 0U ||
        !laghu_font_providers_load((const char *)values[2].data,
                                   &location->font_providers, error,
                                   sizeof(error))) {
      ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                         "invalid Laghu font provider config \"%V\": %s",
                         &values[2], error);
      return NGX_CONF_ERROR;
    }
    location->font_provider_config = values[2];
    location->font_providers_loaded = true;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "javascript_queue") == 0) {
    if (location->javascript_queue.len != 0U) return "is duplicate";
    location->javascript_queue = values[2];
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "javascript_target") == 0) {
    char normalized[LAGHU_JAVASCRIPT_TARGET_SIZE];
    if (location->javascript_target.len != 0U ||
        !laghu_javascript_target_normalize((const char *)values[2].data,
                                           normalized))
      return "laghu javascript_target expects a bounded Browserslist query";
    location->javascript_target = values[2];
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "javascript_observation_config") == 0) {
    char error[256] = "configuration appears more than once";
    if (location->javascript_observation_config.len != 0U ||
        !laghu_javascript_observations_load((const char *)values[2].data,
                                            &location->javascript_observations,
                                            error, sizeof(error))) {
      ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                         "invalid JavaScript observation config \"%V\": %s",
                         &values[2], error);
      return NGX_CONF_ERROR;
    }
    location->javascript_observation_config = values[2];
    location->javascript_observations_loaded = true;
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "javascript_defer_config") == 0) {
    char error[256U];
    if (location->javascript_defer_config.len != 0U ||
        !laghu_javascript_defer_load((const char *)values[2].data,
                                     &location->javascript_defer, error,
                                     sizeof(error))) {
      ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                         "invalid laghu javascript_defer_config: %s", error);
      return NGX_CONF_ERROR;
    }
    location->javascript_defer_config = values[2];
    location->javascript_defer_loaded = true;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "image_cache") == 0) {
    if (location->image_cache.len != 0U ||
        location->file_cache_backend.len != 0U) {
      return "is duplicate";
    }
    location->image_cache = values[2];
    location->image_cache_set = true;
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "file_cache_backend") == 0) {
    char backend_path[LAGHU_RUNTIME_PATH_SIZE];
    u_char *path;
    if (location->file_cache_backend.len != 0U || location->image_cache_set ||
        !laghu_cache_backend_uri_parse((const char *)values[2].data,
                                       backend_path, sizeof(backend_path)))
      return "file_cache_backend expects one local absolute file: URI";
    path = ngx_pnalloc(configuration->pool, strlen(backend_path) + 1U);
    if (path == NULL) return NGX_CONF_ERROR;
    ngx_memcpy(path, backend_path, strlen(backend_path) + 1U);
    location->file_cache_backend = values[2];
    location->image_cache.data = path;
    location->image_cache.len = strlen(backend_path);
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "file_cache_size") == 0 ||
      ngx_strcmp(values[1].data, "file_cache_inode_limit") == 0 ||
      ngx_strcmp(values[1].data, "file_cache_metadata_size") == 0) {
    ssize_t parsed = ngx_strcmp(values[1].data, "file_cache_inode_limit") == 0
                         ? ngx_atoi(values[2].data, values[2].len)
                         : ngx_parse_size(&values[2]);
    size_t *target =
        ngx_strcmp(values[1].data, "file_cache_size") == 0
            ? &location->file_cache_size
            : (ngx_strcmp(values[1].data, "file_cache_inode_limit") == 0
                   ? &location->file_cache_inode_limit
                   : &location->file_cache_metadata_size);
    size_t minimum =
        target == &location->file_cache_size
            ? 1024U * 1024U
            : (target == &location->file_cache_inode_limit ? 16U : 16384U);
    if (*target != NGX_CONF_UNSET_SIZE || parsed < 0 ||
        (size_t)parsed < minimum)
      return "invalid or duplicate file cache limit";
    *target = (size_t)parsed;
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "file_cache_clean_interval") == 0) {
    time_t parsed = ngx_parse_time(&values[2], 1);
    if (location->file_cache_clean_interval != NGX_CONF_UNSET_UINT ||
        parsed < 1 || parsed > 86400)
      return "file_cache_clean_interval expects 1s through 24h";
    location->file_cache_clean_interval = (ngx_uint_t)parsed;
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "purge_method") == 0) {
    if (location->purge_method != NGX_CONF_UNSET ||
        ngx_strcmp(values[2].data, "PURGE") != 0)
      return "purge_method accepts PURGE once";
    location->purge_method = 1;
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "purge_query") == 0 ||
      ngx_strcmp(values[1].data, "statistics") == 0 ||
      ngx_strcmp(values[1].data, "metrics") == 0 ||
      ngx_strcmp(values[1].data, "readiness") == 0) {
    ngx_flag_t *target =
        ngx_strcmp(values[1].data, "purge_query") == 0
            ? &location->purge_query
            : (ngx_strcmp(values[1].data, "statistics") == 0
                   ? &location->statistics
                   : (ngx_strcmp(values[1].data, "metrics") == 0
                          ? &location->metrics
                          : &location->readiness));
    if (*target != NGX_CONF_UNSET) return "is duplicate";
    if (ngx_strcmp(values[2].data, "on") == 0)
      *target = 1;
    else if (ngx_strcmp(values[2].data, "off") == 0)
      *target = 0;
    else
      return "expects on or off";
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "readiness_policy") == 0) {
    if (location->readiness_strict != NGX_CONF_UNSET)
      return "readiness_policy is duplicate";
    if (ngx_strcmp(values[2].data, "strict") == 0)
      location->readiness_strict = 1;
    else if (ngx_strcmp(values[2].data, "degraded") == 0)
      location->readiness_strict = 0;
    else
      return "readiness_policy expects degraded or strict";
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "purge_token_file") == 0 ||
      ngx_strcmp(values[1].data, "cache_flush_file") == 0) {
    ngx_str_t *target = ngx_strcmp(values[1].data, "purge_token_file") == 0
                            ? &location->purge_token_file
                            : &location->cache_flush_file;
    if (target->len != 0U || values[2].len == 0U ||
#ifdef _WIN32
        !(values[2].len >= 3U &&
          ((values[2].data[0] >= 'A' && values[2].data[0] <= 'Z') ||
           (values[2].data[0] >= 'a' && values[2].data[0] <= 'z')) &&
          values[2].data[1] == ':' &&
          (values[2].data[2] == '/' || values[2].data[2] == '\\'))
#else
        values[2].data[0] != '/'
#endif
    )
      return "expects one absolute local path";
    *target = values[2];
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "purge_allow") == 0) {
    ngx_cidr_t cidr;
    ngx_cidr_t *stored;
    if (ngx_ptocidr(&values[2], &cidr) != NGX_OK)
      return "purge_allow expects a canonical CIDR";
    if (location->purge_allow == NULL) {
      location->purge_allow =
          ngx_array_create(configuration->pool, 4U, sizeof(ngx_cidr_t));
      if (location->purge_allow == NULL) return NGX_CONF_ERROR;
    }
    stored = ngx_array_push(location->purge_allow);
    if (stored == NULL) return NGX_CONF_ERROR;
    *stored = cidr;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "cache_mime_types") == 0) {
    if (location->core.cache_mime_types[0] != '\0' ||
        values[2].len >= sizeof(location->core.cache_mime_types)) {
      return "is duplicate or too long";
    }
    ngx_memzero(location->core.cache_mime_types,
                sizeof(location->core.cache_mime_types));
    ngx_memcpy(location->core.cache_mime_types, values[2].data, values[2].len);
    return NGX_CONF_OK;
  }

  ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                     "unsupported laghu command \"%V\"", &values[1]);
  return NGX_CONF_ERROR;
}
