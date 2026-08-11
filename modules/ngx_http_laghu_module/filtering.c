// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <inttypes.h>

#include "laghu/html_cache.h"
#include "ngx_http_laghu_internal.h"

ngx_http_output_header_filter_pt ngx_http_laghu_next_header_filter;
ngx_http_output_body_filter_pt ngx_http_laghu_next_body_filter;
time_t ngx_http_laghu_last_queue_warning;

static bool ngx_http_laghu_html_cache_token_equal(const unsigned char *value,
                                                  size_t length,
                                                  const char *expected) {
  return ngx_strlen(expected) == length &&
         ngx_strncasecmp((u_char *)value, (u_char *)expected, length) == 0;
}

static void laghu_http_laghu_generate_trace_ids(const ngx_http_request_t *request,
                                               char trace_id[33U],
                                               char span_id[17U]) {
  uint64_t trace_high = UINT64_C(1469598103934665603);
  uint64_t trace_low = UINT64_C(1099511628211);
  trace_high ^= (uint64_t)ngx_time();
  trace_high *= UINT64_C(1099511628211);
  trace_low ^= (uint64_t)(uintptr_t)request;
  trace_low *= UINT64_C(1099511628211);
  if (request != NULL && request->connection != NULL) {
    trace_high ^= (uint64_t)request->connection->number;
    trace_low ^= (uint64_t)request->method_name.len;
  }
  if (trace_high == 0U && trace_low == 0U) trace_high = UINT64_C(1);
  (void)snprintf(trace_id, 33U, "%016" PRIx64 "%016" PRIx64, trace_high,
                 trace_low);
  (void)snprintf(span_id, 17U, "%016" PRIx64,
                 trace_high ^ trace_low);
}

static bool ngx_http_laghu_html_cache_control_prohibits(laghu_buffer value) {
  size_t cursor = 0U;
  while (cursor < value.length) {
    size_t end = cursor;
    size_t token_start;
    size_t token_end;
    while (end < value.length && value.data[end] != ',') ++end;
    token_start = cursor;
    while (token_start < end &&
           (value.data[token_start] == ' ' || value.data[token_start] == '\t'))
      ++token_start;
    token_end = token_start;
    while (token_end < end && value.data[token_end] != '=') ++token_end;
    while (token_end > token_start && (value.data[token_end - 1U] == ' ' ||
                                       value.data[token_end - 1U] == '\t'))
      --token_end;
    if (ngx_http_laghu_html_cache_token_equal(
            value.data + token_start, token_end - token_start, "private") ||
        ngx_http_laghu_html_cache_token_equal(
            value.data + token_start, token_end - token_start, "no-store") ||
        ngx_http_laghu_html_cache_token_equal(
            value.data + token_start, token_end - token_start, "no-cache"))
      return true;
    cursor = end + 1U;
  }
  return false;
}

static bool ngx_http_laghu_html_cache_response_safe(
    const ngx_http_laghu_request_ctx_t *context) {
  size_t index;
  bool html = false;
  if (context == NULL || context->response.status != NGX_HTTP_OK ||
      context->request.method.length != 3U ||
      memcmp(context->request.method.data, "GET", 3U) != 0) {
    return false;
  }
  for (index = 0U; index < context->request.header_count; ++index)
    if ((context->request.headers[index].name.length == 6U &&
         ngx_strncasecmp((u_char *)context->request.headers[index].name.data,
                         (u_char *)"Cookie", 6U) == 0) ||
        (context->request.headers[index].name.length == 13U &&
         ngx_strncasecmp((u_char *)context->request.headers[index].name.data,
                         (u_char *)"Authorization", 13U) == 0))
      return false;
  for (index = 0U; index < context->response.header_count; ++index) {
    const laghu_http_header *header = &context->response.headers[index];
    if ((header->name.length == 10U &&
         ngx_strncasecmp((u_char *)header->name.data, (u_char *)"Set-Cookie",
                         10U) == 0) ||
        (header->name.length == 4U &&
         ngx_strncasecmp((u_char *)header->name.data, (u_char *)"Vary", 4U) ==
             0) ||
        (header->name.length == 16U &&
         ngx_strncasecmp((u_char *)header->name.data,
                         (u_char *)"Content-Encoding", 16U) == 0))
      return false;
    if (header->name.length == 13U &&
        ngx_strncasecmp((u_char *)header->name.data, (u_char *)"Cache-Control",
                        13U) == 0 &&
        ngx_http_laghu_html_cache_control_prohibits(header->value))
      return false;
    if (header->name.length == 12U &&
        ngx_strncasecmp((u_char *)header->name.data, (u_char *)"Content-Type",
                        12U) == 0 &&
        header->value.length >= 9U &&
        ngx_strncasecmp((u_char *)header->value.data, (u_char *)"text/html",
                        9U) == 0)
      html = true;
  }
  return html;
}

static void ngx_http_laghu_publish_html_cache(
    ngx_http_laghu_request_ctx_t *context, ngx_http_laghu_loc_conf_t *conf) {
  if (context == NULL || conf == NULL ||
      conf->core.html_cache_origin[0] == '\0' ||
      conf->core.html_cache_ttl == LAGHU_HTML_CACHE_TTL_UNSET ||
      !ngx_http_laghu_html_cache_response_safe(context))
    return;
  (void)laghu_html_cache_publish(
      conf->service.image_cache, conf->core.html_cache_origin,
      (const char *)context->request.normalized_path.data,
      (const char *)context->response.source_validator.data,
      (laghu_buffer){context->capture, context->capture_length},
      (uint64_t)ngx_time(), NULL);
}

ngx_int_t ngx_http_laghu_transaction_header_filter(
    ngx_http_request_t *request) {
  ngx_http_laghu_loc_conf_t *conf =
      ngx_http_get_module_loc_conf(request, ngx_http_laghu_module);
  ngx_http_laghu_request_ctx_t *context;
  laghu_http_transaction_result result;
  bool prepared;
  bool administration_candidate;
  if (conf == NULL) return ngx_http_laghu_next_header_filter(request);
  if (request->uri.len >= sizeof("/.laghu/") - 1U &&
      ngx_strncmp(request->uri.data, "/.laghu/", sizeof("/.laghu/") - 1U) ==
          0) {
    return ngx_http_laghu_next_header_filter(request);
  }
  administration_candidate = conf != NULL &&
                            ngx_http_laghu_administration_candidate(request, conf);
  if (administration_candidate) {
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
  laghu_http_laghu_generate_trace_ids(request, context->trace_id,
                                     context->span_id);
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
    result.not_modified =
        laghu_http_request_matches_result_etag(&context->request, &result);
    if (result.not_modified) {
      request->headers_out.status = NGX_HTTP_NOT_MODIFIED;
      ngx_http_laghu_remove_header(request, "Content-Length");
      ngx_http_laghu_log_transaction(request, context, &result, "none");
      laghu_http_transaction_result_release(&result);
      return ngx_http_laghu_next_header_filter(request);
    }
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
    ngx_http_laghu_loc_conf_t *conf =
        ngx_http_get_module_loc_conf(request, ngx_http_laghu_module);
    ngx_http_laghu_publish_html_cache(context, conf);
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
      unsigned char *copy;
      result.not_modified =
          laghu_http_request_matches_result_etag(&context->request, &result);
      copy = result.not_modified ? NULL
                                 : ngx_pnalloc(request->pool, selected_length);
      if ((!result.not_modified && copy == NULL) ||
          (!result.not_modified &&
           ngx_http_laghu_send_early_hints(request, &result) != NGX_OK) ||
          ngx_http_laghu_apply_result(request, &result) != NGX_OK) {
        laghu_http_transaction_result_release(&result);
        return NGX_ERROR;
      }
      if (result.not_modified) {
        request->headers_out.status = NGX_HTTP_NOT_MODIFIED;
        ngx_http_laghu_remove_header(request, "Content-Length");
        laghu_http_transaction_result_release(&result);
        context->header_deferred = false;
        context->capture_enabled = false;
        return ngx_http_laghu_next_header_filter(request);
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
