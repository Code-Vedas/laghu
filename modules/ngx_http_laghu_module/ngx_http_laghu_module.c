// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "laghu/config.h"
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
    laghu_operational_registry_budget(
        &ngx_http_laghu_operational, &context->transaction.budget,
        context->transaction.environment.config.transform_deadline_ms);
    laghu_operational_registry_lcp(
        &ngx_http_laghu_operational, result.lcp_decision, result.lcp_applied,
        result.lcp_profile_observations, result.lcp_profile_ready);
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
  laghu_config_setting shared_setting;
  char shared_error[160U];

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

  shared_setting = laghu_config_setting_find((const char *)values[1].data);
  if (shared_setting != LAGHU_CONFIG_SETTING_UNKNOWN) {
    if (!laghu_config_setting_apply(&location->core, shared_setting,
                                    (const char *)values[2].data, shared_error,
                                    sizeof(shared_error))) {
      ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                         "invalid laghu %V: %s", &values[1], shared_error);
      return NGX_CONF_ERROR;
    }
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

  ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                     "unsupported laghu command \"%V\"", &values[1]);
  return NGX_CONF_ERROR;
}
