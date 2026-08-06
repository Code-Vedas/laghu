// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>
#include <string.h>

#include "laghu/cache.h"
#include "laghu/operational.h"
#include "ngx_http_laghu_internal.h"

static ngx_int_t ngx_http_laghu_admin_write(
    ngx_http_request_t *request,
    const laghu_http_administrative_response *response, const char *body) {
  ngx_buf_t *buffer;
  ngx_chain_t output;
  ngx_table_elt_t *header = ngx_list_push(&request->headers_out.headers);
  if (header == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
  header->hash = 1U;
  ngx_str_set(&header->key, "Cache-Control");
  ngx_str_set(&header->value, "no-store");
  request->headers_out.status = response->status;
  if (response->content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_PROMETHEUS) {
    ngx_str_set(&request->headers_out.content_type,
                "text/plain; version=0.0.4; charset=utf-8");
  } else {
    ngx_str_set(&request->headers_out.content_type, "application/json");
  }
  request->headers_out.content_length_n = (off_t)response->length;
  if (ngx_http_send_header(request) == NGX_ERROR ||
      request->method == NGX_HTTP_HEAD)
    return NGX_OK;
  buffer = ngx_calloc_buf(request->pool);
  if (buffer == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
  buffer->pos = (u_char *)body;
  buffer->last = (u_char *)body + response->length;
  buffer->memory = 1U;
  buffer->last_buf = 1U;
  output.buf = buffer;
  output.next = NULL;
  return ngx_http_output_filter(request, &output);
}

static ngx_int_t ngx_http_laghu_admin_json(ngx_http_request_t *request,
                                           ngx_uint_t status,
                                           const char *json) {
  laghu_http_administrative_response response = {
      status, LAGHU_HTTP_ADMINISTRATIVE_CONTENT_JSON, strlen(json)};
  return ngx_http_laghu_admin_write(request, &response, json);
}

static bool ngx_http_laghu_admin_authorized(
    ngx_http_request_t *request, const ngx_http_laghu_loc_conf_t *conf) {
  ngx_table_elt_t *token = NULL;
  ngx_list_part_t *part = &request->headers_in.headers.part;
  ngx_table_elt_t *headers = part->elts;
  ngx_uint_t header_index;
  if (conf->service.purge_token_file[0] == '\0' ||
      !ngx_http_laghu_peer_matches(request, conf->service.purge_allow,
                                   conf->service.purge_allow_count))
    return false;
  for (header_index = 0U;; ++header_index) {
    if (header_index >= part->nelts) {
      if (part->next == NULL) break;
      part = part->next;
      headers = part->elts;
      header_index = 0U;
    }
    if (headers[header_index].key.len == sizeof("X-Laghu-Purge-Token") - 1U &&
        ngx_strncasecmp(headers[header_index].key.data,
                        (u_char *)"X-Laghu-Purge-Token",
                        sizeof("X-Laghu-Purge-Token") - 1U) == 0) {
      token = &headers[header_index];
      break;
    }
  }
  if (token != NULL) {
    ngx_file_t file;
    u_char expected[257U];
    ssize_t length;
    size_t supplied = token->value.len, maximum, compare_index;
    u_char difference;
    ngx_memzero(&file, sizeof(file));
    file.name.data = (u_char *)conf->service.purge_token_file;
    file.name.len = strlen(conf->service.purge_token_file);
    file.fd = ngx_open_file((u_char *)conf->service.purge_token_file,
                            NGX_FILE_RDONLY, NGX_FILE_OPEN, 0U);
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
                   (compare_index < supplied ? token->value.data[compare_index]
                                             : 0U));
    return length >= 16 && length < (ssize_t)sizeof(expected) &&
           difference == 0U;
  }
  return false;
}

ngx_int_t ngx_http_laghu_admin_endpoint(ngx_http_request_t *request,
                                        ngx_http_laghu_loc_conf_t *conf) {
  laghu_http_administrative_options options;
  laghu_http_administrative_plan plan;
  laghu_cache_limits cache_limits = {
      .size_limit = conf->service.cache_limits.size_limit,
      .inode_limit = conf->service.cache_limits.inode_limit,
      .metadata_size = conf->service.cache_limits.metadata_size,
      .clean_interval = conf->service.cache_limits.clean_interval};
  laghu_buffer method = {(const unsigned char *)request->method_name.data,
                         request->method_name.len};
  laghu_buffer target = {(const unsigned char *)request->unparsed_uri.data,
                         request->unparsed_uri.len};

  laghu_http_administrative_options_init(&options);
  options.metrics_enabled = conf->service.metrics;
  options.readiness_enabled = conf->service.readiness;
  options.statistics_enabled = conf->service.statistics;
  options.purge_method_enabled = conf->service.purge_method;
  options.purge_query_enabled = conf->service.purge_query;
  options.purge_query_get_only = false;
  if (!laghu_http_administrative_plan_build(&plan, method, target, &options) ||
      !plan.recognized)
    return NGX_DECLINED;
  if (plan.status == NGX_HTTP_NOT_FOUND) return NGX_HTTP_NOT_FOUND;
  if (plan.requires_authorization &&
      !ngx_http_laghu_admin_authorized(request, conf))
    return ngx_http_laghu_admin_json(request, NGX_HTTP_FORBIDDEN,
                                     "{\"status\":\"forbidden\"}");
  if (!laghu_cache_backend_register_path(conf->service.image_cache,
                                         &cache_limits))
    return ngx_http_laghu_admin_json(request, NGX_HTTP_SERVICE_UNAVAILABLE,
                                     "{\"status\":\"unavailable\"}");
  if (plan.status != 0U) {
    if (plan.status == NGX_HTTP_BAD_REQUEST)
      return ngx_http_laghu_admin_json(request, NGX_HTTP_BAD_REQUEST,
                                       "{\"status\":\"malformed\"}");
    return (ngx_int_t)plan.status;
  }
  if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_STATS) {
    laghu_cache_stats stats = {0};
    laghu_http_administrative_response response;
    char *json;
    if (!laghu_cache_backend_health_path(conf->service.image_cache, &stats))
      return NGX_HTTP_SERVICE_UNAVAILABLE;
    json = ngx_pnalloc(request->pool, LAGHU_OPERATIONAL_RENDER_SIZE);
    if (json == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
    if (!laghu_http_administrative_render_stats(&cache_limits, &stats, json,
                                                LAGHU_OPERATIONAL_RENDER_SIZE,
                                                &response))
      return NGX_HTTP_INTERNAL_SERVER_ERROR;
    return ngx_http_laghu_admin_write(request, &response, json);
  }
  if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_METRICS ||
      plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_READINESS) {
    laghu_operational_snapshot *snapshot;
    laghu_http_administrative_response response;
    char *rendered;
    uint64_t now = (uint64_t)ngx_time();
    bool cache_ready = true;
    (void)laghu_operational_registry_heartbeat(&ngx_http_laghu_operational, now,
                                               true, 0U, 0U);
    snapshot = ngx_pcalloc(request->pool, sizeof(*snapshot));
    rendered = ngx_pnalloc(request->pool, LAGHU_OPERATIONAL_RENDER_SIZE);
    if (snapshot == NULL || rendered == NULL)
      return NGX_HTTP_INTERNAL_SERVER_ERROR;
    if (!laghu_operational_registry_snapshot(&ngx_http_laghu_operational,
                                             snapshot))
      return ngx_http_laghu_admin_json(request, NGX_HTTP_SERVICE_UNAVAILABLE,
                                       "{\"status\":\"unavailable\"}");
    if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_READINESS) {
      laghu_cache_stats stats = {0};
      cache_ready =
          laghu_cache_backend_health_path(conf->service.image_cache, &stats);
      laghu_operational_registry_cache(&ngx_http_laghu_operational, &stats);
    }
    if (!laghu_http_administrative_render_operational(
            &plan, snapshot, now, true, cache_ready,
            conf->service.readiness_strict, rendered,
            LAGHU_OPERATIONAL_RENDER_SIZE, &response))
      return NGX_HTTP_INTERNAL_SERVER_ERROR;
    return ngx_http_laghu_admin_write(request, &response, rendered);
  }
  if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_PURGE) {
    laghu_http_administrative_response response;
    char *rendered = ngx_pnalloc(request->pool, LAGHU_OPERATIONAL_RENDER_SIZE);
    uint64_t matched = 0U;
    laghu_cache_purge_result result;
    if (rendered == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
    result = laghu_cache_backend_purge_url_path(conf->service.image_cache,
                                                plan.normalized_path,
                                                (uint64_t)ngx_time(), &matched);
    if (!laghu_http_administrative_render_purge(result, matched, rendered,
                                                LAGHU_OPERATIONAL_RENDER_SIZE,
                                                &response))
      return NGX_HTTP_INTERNAL_SERVER_ERROR;
    return ngx_http_laghu_admin_write(request, &response, rendered);
  }
  return NGX_HTTP_INTERNAL_SERVER_ERROR;
}
