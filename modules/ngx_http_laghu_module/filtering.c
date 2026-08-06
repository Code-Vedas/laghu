// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_http_laghu_internal.h"

ngx_http_output_header_filter_pt ngx_http_laghu_next_header_filter;
ngx_http_output_body_filter_pt ngx_http_laghu_next_body_filter;
time_t ngx_http_laghu_last_queue_warning;

ngx_int_t ngx_http_laghu_transaction_header_filter(
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
  context->log_started_ms = ngx_current_msec;
  prepared = laghu_http_transaction_prepare(
      &context->transaction, &context->request, &context->response,
      &context->environment, &result);
  if (ngx_http_laghu_apply_result(request, &result) != NGX_OK) {
    laghu_http_transaction_result_release(&result);
    return ngx_http_laghu_next_header_filter(request);
  }
  if (!prepared || result.action == LAGHU_HTTP_ACTION_BYPASS) {
    ngx_http_laghu_log_transaction(request, context, &result,
                                   prepared ? "none" : "runtime");
    laghu_http_transaction_result_release(&result);
    return ngx_http_laghu_next_header_filter(request);
  }
  {
    laghu_cache_limits limits = {
        .size_limit = conf->service.cache_limits.size_limit,
        .inode_limit = conf->service.cache_limits.inode_limit,
        .metadata_size = conf->service.cache_limits.metadata_size,
        .clean_interval = conf->service.cache_limits.clean_interval};
    if (!laghu_cache_backend_register_path(conf->service.image_cache,
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
    ngx_http_laghu_log_transaction(request, context, &result, "none");
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

ngx_int_t ngx_http_laghu_transaction_body_filter(ngx_http_request_t *request,
                                                 ngx_chain_t *chain) {
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
    ngx_http_laghu_log_transaction(request, context, &result, "none");
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
  ngx_http_laghu_log_transaction(request, context, NULL, "runtime");
  context->capture_enabled = false;
  return ngx_http_laghu_next_body_filter(request, chain);
}
