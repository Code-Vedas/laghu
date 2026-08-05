// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "laghu/assets.h"
#include "laghu/cache.h"
#include "laghu/catalog.h"
#include "laghu/config.h"
#include "laghu/core.h"
#include "laghu/css.h"
#include "laghu/fonts.h"
#include "laghu/http.h"
#include "laghu/image.h"
#include "laghu/instrumentation.h"
#include "laghu/javascript.h"
#include "laghu/operational.h"
#include "laghu/queue.h"
#include "laghu/rum.h"
#include "laghu/source.h"
#include "laghu/types.h"
#include "ngx_http_laghu_internal.h"

static ngx_http_output_header_filter_pt ngx_http_laghu_next_header_filter;
static ngx_http_output_body_filter_pt ngx_http_laghu_next_body_filter;
static time_t ngx_http_laghu_last_queue_warning;
static ngx_int_t ngx_http_laghu_filter_init(ngx_conf_t *configuration);
void ngx_http_laghu_queue_cleanup(void *data) {
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

bool ngx_http_laghu_queue_refresh(ngx_http_laghu_loc_conf_t *conf) {
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

ngx_int_t ngx_http_laghu_apply_result(
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

bool ngx_http_laghu_normalize(ngx_http_request_t *request,
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
