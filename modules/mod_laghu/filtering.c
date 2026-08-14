// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <errno.h>
#include <httpd.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>

/* Apache requires httpd.h to define its public record types first. */
#include <apr_buckets.h>
#include <apr_network_io.h>
#include <apr_strings.h>
#include <apr_tables.h>
#include <http_config.h>
#include <http_core.h>
#include <http_log.h>
#include <http_protocol.h>
#include <http_request.h>
#include <util_filter.h>

#include "laghu/assets.h"
#include "laghu/cache.h"
#include "laghu/catalog.h"
#include "laghu/config.h"
#include "laghu/core.h"
#include "laghu/css.h"
#include "laghu/fonts.h"
#include "laghu/html_cache.h"
#include "laghu/http.h"
#include "laghu/image.h"
#include "laghu/instrumentation.h"
#include "laghu/javascript.h"
#include "laghu/operational.h"
#include "laghu/queue.h"
#include "laghu/rum.h"
#include "laghu/source.h"
#include "laghu/types.h"

#define LAGHU_CACHE_SET_SIZE (1U << 0U)
#define LAGHU_CACHE_SET_INODES (1U << 1U)
#define LAGHU_CACHE_SET_CLEAN (1U << 2U)
#define LAGHU_CACHE_SET_METADATA (1U << 3U)
#define LAGHU_ADMIN_SET_METHOD (1U << 0U)
#define LAGHU_ADMIN_SET_QUERY (1U << 1U)
#define LAGHU_ADMIN_SET_STATS (1U << 2U)
#define LAGHU_ADMIN_SET_TOKEN (1U << 3U)
#define LAGHU_ADMIN_SET_FLUSH (1U << 4U)
#define LAGHU_ADMIN_SET_METRICS (1U << 5U)
#define LAGHU_ADMIN_SET_READINESS (1U << 6U)
#define LAGHU_ADMIN_SET_READINESS_POLICY (1U << 7U)

#include "mod_laghu_internal.h"

static void laghu_apache_generate_trace_ids(const request_rec *request, char trace_id[33U], char span_id[17U]) {
  uint64_t trace_high = UINT64_C(1469598103934665603);
  uint64_t trace_low = UINT64_C(1099511628211);
  if (request == NULL || trace_id == NULL || span_id == NULL) return;
  trace_high ^= (uint64_t)apr_time_now();
  trace_high *= UINT64_C(1099511628211);
  trace_low ^= (uint64_t)(uintptr_t)request;
  trace_low *= UINT64_C(1099511628211);
  trace_low ^= (uint64_t)apr_time_sec(apr_time_now()) << 32U;
  if (trace_high == 0U && trace_low == 0U) trace_high = UINT64_C(1);
  (void)snprintf(trace_id, 33U, "%016" PRIx64 "%016" PRIx64, trace_high, trace_low);
  (void)snprintf(span_id, 17U, "%016" PRIx64, trace_high ^ trace_low);
}

static bool laghu_apache_html_cache_token_equal(const unsigned char *value, size_t length, const char *expected) {
  return strlen(expected) == length && ap_cstr_casecmpn((const char *)value, expected, length) == 0;
}

static bool laghu_apache_html_cache_control_prohibits(laghu_buffer value) {
  size_t cursor = 0U;
  while (cursor < value.length) {
    size_t end = cursor;
    size_t token_start;
    size_t token_end;
    while (end < value.length && value.data[end] != ',') ++end;
    token_start = cursor;
    while (token_start < end && (value.data[token_start] == ' ' || value.data[token_start] == '\t')) ++token_start;
    token_end = token_start;
    while (token_end < end && value.data[token_end] != '=') ++token_end;
    while (token_end > token_start && (value.data[token_end - 1U] == ' ' || value.data[token_end - 1U] == '\t')) --token_end;
    if (laghu_apache_html_cache_token_equal(value.data + token_start, token_end - token_start, "private") ||
        laghu_apache_html_cache_token_equal(value.data + token_start, token_end - token_start, "no-store") ||
        laghu_apache_html_cache_token_equal(value.data + token_start, token_end - token_start, "no-cache"))
      return true;
    cursor = end + 1U;
  }
  return false;
}

static bool laghu_apache_html_cache_vary_is_encoding(laghu_buffer value) {
  size_t cursor = 0U;
  bool found = false;
  while (cursor < value.length) {
    size_t end = cursor;
    size_t token_start;
    size_t token_end;
    while (end < value.length && value.data[end] != ',') ++end;
    token_start = cursor;
    while (token_start < end && (value.data[token_start] == ' ' || value.data[token_start] == '\t')) ++token_start;
    token_end = end;
    while (token_end > token_start && (value.data[token_end - 1U] == ' ' || value.data[token_end - 1U] == '\t')) --token_end;
    if (!laghu_apache_html_cache_token_equal(value.data + token_start, token_end - token_start, "Accept-Encoding")) return false;
    found = true;
    cursor = end + 1U;
  }
  return found;
}

static bool laghu_apache_html_cache_response_safe(const laghu_apache_context *context) {
  size_t index;
  bool html = false;
  if (context == NULL || context->response.status != HTTP_OK || context->request.method.length != 3U ||
      memcmp(context->request.method.data, "GET", 3U) != 0)
    return false;
  for (index = 0U; index < context->request.header_count; ++index)
    if ((context->request.headers[index].name.length == 6U &&
         ap_cstr_casecmpn((const char *)context->request.headers[index].name.data, "Cookie", 6U) == 0) ||
        (context->request.headers[index].name.length == 13U &&
         ap_cstr_casecmpn((const char *)context->request.headers[index].name.data, "Authorization", 13U) == 0))
      return false;
  for (index = 0U; index < context->response.header_count; ++index) {
    const laghu_http_header *header = &context->response.headers[index];
    if ((header->name.length == 10U && ap_cstr_casecmpn((const char *)header->name.data, "Set-Cookie", 10U) == 0) ||
        (header->name.length == 4U && ap_cstr_casecmpn((const char *)header->name.data, "Vary", 4U) == 0 &&
         !laghu_apache_html_cache_vary_is_encoding(header->value)) ||
        (header->name.length == 16U && ap_cstr_casecmpn((const char *)header->name.data, "Content-Encoding", 16U) == 0))
      return false;
    if (header->name.length == 13U && ap_cstr_casecmpn((const char *)header->name.data, "Cache-Control", 13U) == 0 &&
        laghu_apache_html_cache_control_prohibits(header->value))
      return false;
    if (header->name.length == 12U && ap_cstr_casecmpn((const char *)header->name.data, "Content-Type", 12U) == 0 && header->value.length >= 9U &&
        ap_cstr_casecmpn((const char *)header->value.data, "text/html", 9U) == 0)
      html = true;
  }
  return html;
}

static void laghu_apache_publish_html_cache(laghu_apache_context *context, const laghu_http_transaction_result *result) {
  const laghu_apache_config *config;
  if (context == NULL || result == NULL || result->dependencies_pending || (config = context->config) == NULL ||
      config->core.html_cache_origin[0] == '\0' || config->core.html_cache_ttl == LAGHU_HTML_CACHE_TTL_UNSET ||
      !laghu_apache_html_cache_response_safe(context))
    return;
  (void)laghu_html_cache_publish(config->service.image_cache, config->core.html_cache_origin, (const char *)context->request.normalized_path.data,
                                 (const char *)context->response.source_validator.data,
                                 result->cache_selected.data == NULL ? result->selected : result->cache_selected,
                                 (uint64_t)apr_time_sec(apr_time_now()), NULL);
}

bool laghu_apache_peer_matches(request_rec *request, const laghu_service_cidr *cidrs, size_t count) {
  unsigned char address[16U] = {0};
  unsigned int family;
  size_t index;
  if (request->connection->client_addr == NULL) return false;
  if (request->connection->client_addr->family == AF_INET) {
    memcpy(address, &request->connection->client_addr->sa.sin.sin_addr, 4U);
    family = LAGHU_SERVICE_CIDR_FAMILY_IPV4;
  } else if (request->connection->client_addr->family == AF_INET6) {
    memcpy(address, &request->connection->client_addr->sa.sin6.sin6_addr, 16U);
    family = LAGHU_SERVICE_CIDR_FAMILY_IPV6;
  } else
    return false;
  for (index = 0U; index < count; ++index)
    if (laghu_service_cidr_matches(&cidrs[index], address, family)) return true;
  return false;
}

static bool laghu_apache_collect_table(const apr_table_t *table, laghu_http_header *headers, size_t capacity, size_t *count) {
  const apr_array_header_t *array = apr_table_elts(table);
  const apr_table_entry_t *entries = (const apr_table_entry_t *)array->elts;
  int index;
  *count = 0U;
  for (index = 0; index < array->nelts; ++index) {
    if (entries[index].key == NULL || entries[index].val == NULL) {
      continue;
    }
    if (*count >= capacity) {
      return false;
    }
    headers[*count].name = (laghu_buffer){(const unsigned char *)entries[index].key, strlen(entries[index].key)};
    headers[*count].value = (laghu_buffer){(const unsigned char *)entries[index].val, strlen(entries[index].val)};
    ++*count;
  }
  return true;
}

bool laghu_apache_normalize(request_rec *request, laghu_apache_context *context) {
  const char *authority = request->hostname != NULL ? request->hostname : request->server->server_hostname;
  const char *validator = apr_table_get(request->headers_out, "ETag");
  const char *file_validator = NULL;
  laghu_http_transaction_init(&context->transaction);
  if (!laghu_apache_collect_table(request->headers_in, context->request_headers, LAGHU_HTTP_MAX_REQUEST_HEADERS, &context->request.header_count) ||
      !laghu_apache_collect_table(request->headers_out, context->response_headers, LAGHU_HTTP_MAX_RESPONSE_HEADERS,
                                  &context->response.header_count)) {
    return false;
  }
  if (request->content_type != NULL && apr_table_get(request->headers_out, "Content-Type") == NULL) {
    size_t index = context->response.header_count++;
    if (index >= LAGHU_HTTP_MAX_RESPONSE_HEADERS) {
      return false;
    }
    context->response_headers[index].name = (laghu_buffer){(const unsigned char *)"Content-Type", 12U};
    context->response_headers[index].value = (laghu_buffer){(const unsigned char *)request->content_type, strlen(request->content_type)};
  }
  if ((validator == NULL || ap_cstr_casecmpn(validator, "W/", 2U) == 0) && request->finfo.filetype != APR_NOFILE && request->mtime > 0) {
    file_validator = apr_psprintf(request->pool, "file-%" APR_TIME_T_FMT "-%" APR_OFF_T_FMT, request->mtime, request->finfo.size);
  }
  context->request.version = LAGHU_HTTP_ABI_VERSION;
  context->request.struct_size = sizeof(context->request);
  context->request.method = (laghu_buffer){(const unsigned char *)request->method, strlen(request->method)};
  context->request.scheme = (laghu_buffer){(const unsigned char *)ap_http_scheme(request), strlen(ap_http_scheme(request))};
  if (context->config->core.respect_x_forwarded_proto == LAGHU_MODE_ON && context->config->service.trusted_proxy_count != 0U) {
    bool trusted;
    const char *forwarded = apr_table_get(request->headers_in, "X-Forwarded-Proto");
    trusted = laghu_apache_peer_matches(request, context->config->service.trusted_proxies, context->config->service.trusted_proxy_count);
    if (trusted && forwarded != NULL && strchr(forwarded, ',') == NULL &&
        (ap_cstr_casecmp(forwarded, "http") == 0 || ap_cstr_casecmp(forwarded, "https") == 0))
      context->request.scheme = (laghu_buffer){(const unsigned char *)forwarded, strlen(forwarded)};
  }
  context->request.authority = (laghu_buffer){(const unsigned char *)authority, authority == NULL ? 0U : strlen(authority)};
  context->request.normalized_path =
      (laghu_buffer){(const unsigned char *)request->unparsed_uri, request->unparsed_uri == NULL ? 0U : strlen(request->unparsed_uri)};
  context->request.headers = context->request_headers;
  context->response.version = LAGHU_HTTP_ABI_VERSION;
  context->response.struct_size = sizeof(context->response);
  context->response.status = (unsigned int)request->status;
  context->response.headers = context->response_headers;
  context->response.has_declared_length = request->clength >= 0;
  context->response.declared_length = request->clength >= 0 ? (size_t)request->clength : 0U;
  context->response.complete = true;
  context->response.partial = request->status == HTTP_PARTIAL_CONTENT;
  if (file_validator != NULL) {
    context->response.source_validator = (laghu_buffer){(const unsigned char *)file_validator, strlen(file_validator)};
  }
  context->environment.version = LAGHU_HTTP_ABI_VERSION;
  context->environment.struct_size = sizeof(context->environment);
  context->environment.config = context->config->core;
  context->environment.cache_path = context->config->service.image_cache;
  context->environment.rum = laghu_apache_rum;
  context->environment.queue = laghu_apache_image_queue(context->config);
  context->environment.queue_capabilities = laghu_apache_image_queue_capabilities(context->config);
  context->environment.font_fetch_queue = laghu_apache_font_queue(context->config);
  if (context->environment.font_fetch_queue != NULL) context->environment.font_providers = context->config->service.font_providers;
  context->environment.javascript_queue = laghu_apache_javascript_queue(context->config);
  context->environment.chrome_analysis_queue = laghu_apache_chrome_analysis_queue(context->config);
  context->environment.chrome_analysis_timeout_ms = context->config->service.chrome_analysis_timeout_ms;
  context->environment.javascript_target = context->config->service.javascript_target;
  context->environment.javascript_observations = context->config->service.javascript_observations;
  context->environment.javascript_defer = context->config->service.javascript_defer;
  context->environment.asset_offload = context->config->service.asset_offload;
  context->environment.now = (uint64_t)apr_time_sec(apr_time_now());
  return true;
}

void laghu_apache_send_early_hints(request_rec *request, const laghu_http_transaction_result *result) {
  apr_table_t *headers;
  apr_table_t *original;
  int status;
  const char *status_line;
  size_t index;
  if (request == NULL || result == NULL || request->proto_num < HTTP_VERSION(1, 1)) return;
  headers = apr_table_make(request->pool, (int)result->header_operation_count);
  if (headers == NULL) return;
  for (index = 0U; index < result->header_operation_count; ++index) {
    const laghu_http_header_operation *operation = &result->header_operations[index];
    if (operation->early_hint) apr_table_addn(headers, "Link", operation->value);
  }
  if (apr_is_empty_table(headers)) return;
  status = request->status;
  status_line = request->status_line;
  original = request->headers_out;
  request->headers_out = headers;
  request->status = HTTP_EARLY_HINTS;
  request->status_line = "103 Early Hints";
  ap_send_interim_response(request, 1);
  request->status = status;
  request->status_line = status_line;
  request->headers_out = original;
}

bool laghu_apache_apply_result(request_rec *request, const laghu_http_transaction_result *result) {
  const char **names = apr_pcalloc(request->pool, result->header_operation_count * sizeof(*names));
  const char **values = apr_pcalloc(request->pool, result->header_operation_count * sizeof(*values));
  size_t index;
  if (result->header_operation_count != 0U && (names == NULL || values == NULL)) {
    return false;
  }
  for (index = 0U; index < result->header_operation_count; ++index) {
    names[index] = apr_pstrdup(request->pool, result->header_operations[index].name);
    if (result->header_operations[index].kind != LAGHU_HTTP_HEADER_REMOVE) {
      values[index] = apr_pstrdup(request->pool, result->header_operations[index].value);
    }
    if (names[index] == NULL || (result->header_operations[index].kind != LAGHU_HTTP_HEADER_REMOVE && values[index] == NULL)) {
      return false;
    }
  }
  for (index = 0U; index < result->header_operation_count; ++index) {
    bool content_type = ap_cstr_casecmp(names[index], "Content-Type") == 0;
    switch (result->header_operations[index].kind) {
      case LAGHU_HTTP_HEADER_SET:
        apr_table_set(request->headers_out, names[index], values[index]);
        if (content_type) ap_set_content_type(request, values[index]);
        break;
      case LAGHU_HTTP_HEADER_APPEND:
        apr_table_add(request->headers_out, names[index], values[index]);
        break;
      case LAGHU_HTTP_HEADER_REMOVE:
        apr_table_unset(request->headers_out, names[index]);
        if (content_type) ap_set_content_type(request, NULL);
        break;
    }
  }
  return true;
}

apr_status_t laghu_apache_transaction_filter(ap_filter_t *filter, apr_bucket_brigade *brigade) {
  request_rec *request = filter->r;
  laghu_apache_context *context = filter->ctx;
  apr_bucket *bucket;
  bool eos = false;
  if (apr_table_get(request->notes, "laghu-html-cache-served") != NULL) {
    ap_remove_output_filter(filter);
    return ap_pass_brigade(filter->next, brigade);
  }
  if (context == NULL) {
    laghu_apache_config *server_config = ap_get_module_config(request->server->module_config, &laghu_module);
    laghu_apache_config *directory_config = ap_get_module_config(request->per_dir_config, &laghu_module);
    laghu_http_transaction_result prepared;
    bool ok;
    context = apr_pcalloc(request->pool, sizeof(*context));
    if (context == NULL) {
      ap_remove_output_filter(filter);
      return ap_pass_brigade(filter->next, brigade);
    }
    context->log_started = apr_time_now();
    laghu_apache_generate_trace_ids(request, context->trace_id, context->span_id);
    context->config = laghu_apache_merge_config(request->pool, server_config, directory_config);
    if (context->config == NULL || !laghu_apache_normalize(request, context)) {
      ap_remove_output_filter(filter);
      return ap_pass_brigade(filter->next, brigade);
    }
    ok = laghu_http_transaction_prepare(&context->transaction, &context->request, &context->response, &context->environment, &prepared);
    if (!laghu_apache_apply_result(request, &prepared)) {
      laghu_http_transaction_result_release(&prepared);
      ap_remove_output_filter(filter);
      return ap_pass_brigade(filter->next, brigade);
    }
    context->action = prepared.action;
    if (!ok || prepared.action == LAGHU_HTTP_ACTION_BYPASS) {
      laghu_apache_log_transaction(request, context, &prepared, ok ? "none" : "runtime");
      laghu_http_transaction_result_release(&prepared);
      ap_remove_output_filter(filter);
      return ap_pass_brigade(filter->next, brigade);
    }
    if (prepared.action == LAGHU_HTTP_ACTION_SERVE_CACHED) {
      prepared.not_modified = laghu_http_request_matches_result_etag(&context->request, &prepared);
      if (prepared.not_modified) {
        request->status = HTTP_NOT_MODIFIED;
        apr_table_unset(request->headers_out, "Content-Length");
        laghu_apache_log_transaction(request, context, &prepared, "none");
        laghu_http_transaction_result_release(&prepared);
        ap_remove_output_filter(filter);
        apr_brigade_cleanup(brigade);
        APR_BRIGADE_INSERT_TAIL(brigade, apr_bucket_eos_create(request->connection->bucket_alloc));
        return ap_pass_brigade(filter->next, brigade);
      }
      context->selected_body = apr_pmemdup(request->pool, prepared.selected.data, prepared.selected.length);
      context->selected_length = prepared.selected.length;
      if (context->selected_body == NULL) {
        laghu_http_transaction_result_release(&prepared);
        ap_remove_output_filter(filter);
        return ap_pass_brigade(filter->next, brigade);
      }
      context->cache_hit = true;
      laghu_apache_log_transaction(request, context, &prepared, "none");
    } else {
      context->capture_capacity = prepared.capture_limit;
      if (context->response.has_declared_length && context->response.declared_length < context->capture_capacity) {
        context->capture_capacity = context->response.declared_length;
      }
      context->capture = apr_palloc(request->pool, context->capture_capacity);
      context->capture_enabled = context->capture != NULL;
      if (!context->capture_enabled) {
        laghu_http_transaction_result_release(&prepared);
        ap_remove_output_filter(filter);
        return ap_pass_brigade(filter->next, brigade);
      }
    }
    laghu_http_transaction_result_release(&prepared);
    filter->ctx = context;
  }
  if (context->cache_hit) {
    apr_bucket_brigade *replacement;
    if (context->cache_sent) {
      apr_brigade_cleanup(brigade);
      return APR_SUCCESS;
    }
    replacement = apr_brigade_create(request->pool, request->connection->bucket_alloc);
    if (replacement == NULL) {
      return ap_pass_brigade(filter->next, brigade);
    }
    APR_BRIGADE_INSERT_TAIL(replacement, apr_bucket_pool_create((const char *)context->selected_body, context->selected_length, request->pool,
                                                                request->connection->bucket_alloc));
    APR_BRIGADE_INSERT_TAIL(replacement, apr_bucket_eos_create(request->connection->bucket_alloc));
    context->cache_sent = true;
    apr_brigade_cleanup(brigade);
    ap_remove_output_filter(filter);
    return ap_pass_brigade(filter->next, replacement);
  }
  for (bucket = APR_BRIGADE_FIRST(brigade); bucket != APR_BRIGADE_SENTINEL(brigade); bucket = APR_BUCKET_NEXT(bucket)) {
    const char *data;
    apr_size_t length;
    apr_status_t status;
    if (APR_BUCKET_IS_EOS(bucket)) {
      eos = true;
      continue;
    }
    if (APR_BUCKET_IS_METADATA(bucket)) {
      continue;
    }
    status = apr_bucket_read(bucket, &data, &length, APR_NONBLOCK_READ);
    if (status != APR_SUCCESS || length > context->capture_capacity - context->capture_length) {
      context->capture_enabled = false;
      break;
    }
    memcpy(context->capture + context->capture_length, data, length);
    context->capture_length += length;
  }
  if (eos && context->capture_enabled) {
    laghu_http_transaction_result result;
    (void)laghu_http_transaction_finalize(&context->transaction, (laghu_buffer){context->capture, context->capture_length}, &result);
    laghu_apache_publish_html_cache(context, &result);
    laghu_operational_registry_budget(&laghu_apache_operational, &context->transaction.budget,
                                      context->transaction.environment.config.transform_deadline_ms);
    laghu_operational_registry_lcp(&laghu_apache_operational, result.lcp_decision, result.lcp_applied, result.lcp_profile_observations,
                                   result.lcp_profile_ready);
    laghu_apache_log_defer_recommendation(request, &result);
    laghu_apache_log_transaction(request, context, &result, "none");
    if (context->action == LAGHU_HTTP_ACTION_CAPTURE_HTML || context->action == LAGHU_HTTP_ACTION_CAPTURE_CSS ||
        context->action == LAGHU_HTTP_ACTION_CAPTURE_JAVASCRIPT) {
      apr_bucket_brigade *replacement;
      unsigned char *selected = apr_pmemdup(request->pool, result.selected.data, result.selected.length);
      result.not_modified = laghu_http_request_matches_result_etag(&context->request, &result);
      if (!result.not_modified) laghu_apache_send_early_hints(request, &result);
      if ((!result.not_modified && selected == NULL) || !laghu_apache_apply_result(request, &result)) {
        laghu_http_transaction_result_release(&result);
        ap_remove_output_filter(filter);
        return ap_pass_brigade(filter->next, brigade);
      }
      if (result.not_modified) {
        request->status = HTTP_NOT_MODIFIED;
        apr_table_unset(request->headers_out, "Content-Length");
        laghu_http_transaction_result_release(&result);
        apr_brigade_cleanup(brigade);
        ap_remove_output_filter(filter);
        APR_BRIGADE_INSERT_TAIL(brigade, apr_bucket_eos_create(request->connection->bucket_alloc));
        return ap_pass_brigade(filter->next, brigade);
      }
      context->selected_length = result.selected.length;
      laghu_http_transaction_result_release(&result);
      replacement = apr_brigade_create(request->pool, request->connection->bucket_alloc);
      if (replacement == NULL) {
        return ap_pass_brigade(filter->next, brigade);
      }
      APR_BRIGADE_INSERT_TAIL(
          replacement, apr_bucket_pool_create((const char *)selected, context->selected_length, request->pool, request->connection->bucket_alloc));
      APR_BRIGADE_INSERT_TAIL(replacement, apr_bucket_eos_create(request->connection->bucket_alloc));
      apr_brigade_cleanup(brigade);
      ap_remove_output_filter(filter);
      return ap_pass_brigade(filter->next, replacement);
    }
    laghu_http_transaction_result_release(&result);
    context->capture_enabled = false;
  }
  if (context->action == LAGHU_HTTP_ACTION_CAPTURE_HTML || context->action == LAGHU_HTTP_ACTION_CAPTURE_CSS ||
      context->action == LAGHU_HTTP_ACTION_CAPTURE_JAVASCRIPT) {
    apr_bucket_brigade *metadata = apr_brigade_create(request->pool, request->connection->bucket_alloc);
    for (bucket = APR_BRIGADE_FIRST(brigade); bucket != APR_BRIGADE_SENTINEL(brigade); bucket = APR_BUCKET_NEXT(bucket)) {
      if (APR_BUCKET_IS_METADATA(bucket) && !APR_BUCKET_IS_EOS(bucket)) {
        apr_bucket *copy = NULL;
        if (apr_bucket_copy(bucket, &copy) == APR_SUCCESS) {
          APR_BRIGADE_INSERT_TAIL(metadata, copy);
        }
      }
    }
    apr_brigade_cleanup(brigade);
    if (!APR_BRIGADE_EMPTY(metadata)) {
      return ap_pass_brigade(filter->next, metadata);
    }
    return APR_SUCCESS;
  }
  if (eos) {
    laghu_apache_log_transaction(request, context, NULL, "runtime");
    ap_remove_output_filter(filter);
  }
  return ap_pass_brigade(filter->next, brigade);
}

void laghu_apache_insert_filter(request_rec *request) {
  laghu_apache_config *server_config = ap_get_module_config(request->server->module_config, &laghu_module);
  laghu_apache_config *directory_config = ap_get_module_config(request->per_dir_config, &laghu_module);
  laghu_config effective;
  if (server_config == NULL || directory_config == NULL ||
      (request->uri != NULL && strncmp(request->uri, "/.laghu/", sizeof("/.laghu/") - 1U) == 0)) {
    return;
  }
  laghu_config_merge(&effective, &server_config->core, &directory_config->core);
  if (effective.mode == LAGHU_MODE_ON) {
    ap_add_output_filter(LAGHU_APACHE_FILTER, NULL, request, request->connection);
  }
}
