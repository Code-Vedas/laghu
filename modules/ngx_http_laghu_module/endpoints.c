// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>

#include "laghu/html_cache.h"
#include "ngx_http_laghu_internal.h"

static void ngx_http_laghu_enqueue_html_refresh(ngx_http_laghu_loc_conf_t *conf, const ngx_str_t *path, const laghu_html_cache_record *record) {
  laghu_runtime_queue *queue;
  laghu_runtime_job job = {0};
  unsigned int index, candidate = 0U;
  ngx_atomic_t now;
  if (conf == NULL || path == NULL || record == NULL || path->len >= sizeof(job.request_path) ||
      !laghu_html_cache_key(conf->core.html_cache_origin, (const char *)path->data, job.index_key)) {
    return;
  }
  queue = ngx_http_laghu_html_refresh_queue(conf);
  if (queue == NULL) return;
  job.kind = LAGHU_RUNTIME_JOB_HTML_REFRESH;
  ngx_memcpy(job.request_path, path->data, path->len);
  ngx_memcpy(job.policy_key, job.index_key, sizeof(job.policy_key));
  ngx_cpystrn((u_char *)job.validator, (u_char *)record->entry.validator, sizeof(job.validator));
  now = (ngx_atomic_t)ngx_time();
  if (!ngx_atomic_cmp_set(&conf->html_refresh_dedup_lock, 0U, 1U)) return;
  for (index = 0U; index < LAGHU_NGINX_HTML_REFRESH_DEDUP; ++index) {
    if (conf->html_refresh_until[index] > now && memcmp(conf->html_refresh_keys[index], job.index_key, sizeof(job.index_key)) == 0) {
      (void)ngx_atomic_cmp_set(&conf->html_refresh_dedup_lock, 1U, 0U);
      return;
    }
    if (conf->html_refresh_until[index] < conf->html_refresh_until[candidate]) candidate = index;
  }
  if (laghu_runtime_queue_try_publish(queue, &job)) {
    ngx_memcpy(conf->html_refresh_keys[candidate], job.index_key, sizeof(job.index_key));
    conf->html_refresh_until[candidate] = now + 1U;
  }
  (void)ngx_atomic_cmp_set(&conf->html_refresh_dedup_lock, 1U, 0U);
}

static ngx_int_t ngx_http_laghu_html_cache_handler(ngx_http_request_t *request, ngx_http_laghu_loc_conf_t *conf) {
  laghu_html_cache_record record;
  laghu_runtime_cache_entry encoded;
  laghu_precompressed_coding coding;
  ngx_str_t path;
  unsigned char *body;
  char accept_encoding[LAGHU_HTTP_MAX_HEADER_VALUE + 1U];
  u_char *content_type;
  ngx_buf_t *buffer;
  ngx_chain_t output;
  ngx_int_t status;
  size_t content_type_length;
  ngx_table_elt_t *header;
  u_char *shield_policy;
  size_t shield_policy_length;
  bool administration_candidate;
  if (request == NULL || conf == NULL || conf->core.mode != LAGHU_MODE_ON || request->method != NGX_HTTP_GET ||
      conf->core.html_cache_origin[0] == '\0' || conf->core.html_cache_ttl == LAGHU_HTML_CACHE_TTL_UNSET ||
      request->headers_in.authorization != NULL ||
#if (nginx_version >= 1023000)
      request->headers_in.cookie != NULL ||
#else
      request->headers_in.cookies.nelts != 0U ||
#endif
      (request->uri.len >= sizeof("/.laghu/") - 1U && ngx_strncmp(request->uri.data, "/.laghu/", sizeof("/.laghu/") - 1U) == 0)) {
    return NGX_DECLINED;
  }
  administration_candidate = ngx_http_laghu_administration_candidate(request, conf);
  if (administration_candidate) return NGX_DECLINED;
  path.len = request->uri.len + (request->args.len == 0U ? 0U : 1U + request->args.len);
  if (path.len == 0U || request->uri.data[0] != '/' || (path.data = ngx_pnalloc(request->pool, path.len + 1U)) == NULL) {
    return NGX_DECLINED;
  }
  ngx_memcpy(path.data, request->uri.data, request->uri.len);
  if (request->args.len != 0U) {
    path.data[request->uri.len] = '?';
    ngx_memcpy(path.data + request->uri.len + 1U, request->args.data, request->args.len);
  }
  path.data[path.len] = '\0';
  if (!laghu_html_cache_lookup(conf->service.image_cache, conf->core.html_cache_origin, (const char *)path.data, (uint64_t)ngx_time(),
                               conf->core.html_cache_ttl, conf->core.html_cache_stale_ttl, &record) ||
      record.state == LAGHU_HTML_CACHE_MISS || record.entry.length == 0U || record.entry.content_type[0] == '\0') {
    return NGX_DECLINED;
  }
  if (record.state == LAGHU_HTML_CACHE_STALE) ngx_http_laghu_enqueue_html_refresh(conf, &path, &record);
  content_type_length = ngx_strlen(record.entry.content_type);
  if (content_type_length >= sizeof(record.entry.content_type) || (body = ngx_pnalloc(request->pool, record.entry.length)) == NULL ||
      (content_type = ngx_pnalloc(request->pool, content_type_length + 1U)) == NULL || (buffer = ngx_calloc_buf(request->pool)) == NULL ||
      !laghu_runtime_cache_read(&record.entry, body, record.entry.length)) {
    return NGX_DECLINED;
  }
  accept_encoding[0] = '\0';
  if (request->headers_in.accept_encoding != NULL) {
    if (request->headers_in.accept_encoding->value.len > LAGHU_HTTP_MAX_HEADER_VALUE) return NGX_DECLINED;
    ngx_memcpy(accept_encoding, request->headers_in.accept_encoding->value.data, request->headers_in.accept_encoding->value.len);
    accept_encoding[request->headers_in.accept_encoding->value.len] = '\0';
  }
  if (laghu_precompressed_select(conf->service.image_cache, (laghu_buffer){body, record.entry.length},
                                 request->headers_in.accept_encoding == NULL ? NULL : accept_encoding, &encoded, &coding)) {
    unsigned char *compressed = ngx_pnalloc(request->pool, encoded.length);
    if (compressed != NULL && laghu_runtime_cache_read(&encoded, compressed, encoded.length)) {
      body = compressed;
      record.entry = encoded;
      header = ngx_list_push(&request->headers_out.headers);
      if (header == NULL) return NGX_DECLINED;
      header->hash = 1U;
      ngx_str_set(&header->key, "Content-Encoding");
      header->value.data = (u_char *)laghu_precompressed_coding_name(coding);
      header->value.len = ngx_strlen(header->value.data);
    }
  }
  ngx_memcpy(content_type, record.entry.content_type, content_type_length);
  content_type[content_type_length] = '\0';
  request->headers_out.status = NGX_HTTP_OK;
  request->headers_out.content_type = (ngx_str_t){content_type_length, content_type};
  request->headers_out.content_length_n = (off_t)record.entry.length;
  header = ngx_list_push(&request->headers_out.headers);
  if (header == NULL) return NGX_DECLINED;
  header->hash = 1U;
  ngx_str_set(&header->key, "X-Laghu-Cache");
  ngx_str_set(&header->value, "hit");
  header = ngx_list_push(&request->headers_out.headers);
  if (header == NULL) return NGX_DECLINED;
  header->hash = 1U;
  ngx_str_set(&header->key, "Vary");
  ngx_str_set(&header->value, "Accept-Encoding");
  if (conf->core.origin_shield == LAGHU_MODE_ON && conf->core.html_cache_ttl != LAGHU_HTML_CACHE_TTL_UNSET) {
    shield_policy = ngx_pnalloc(request->pool, 96U);
    if (shield_policy == NULL) return NGX_DECLINED;
    shield_policy_length =
        (size_t)(ngx_snprintf(shield_policy, 96U, "public, s-maxage=%ui, stale-while-revalidate=%ui", conf->core.html_cache_ttl,
                              conf->core.html_cache_stale_ttl == LAGHU_HTML_CACHE_STALE_TTL_UNSET ? 0U : conf->core.html_cache_stale_ttl) -
                 shield_policy);
    for (unsigned int shield_index = 0U; shield_index < 2U; ++shield_index) {
      header = ngx_list_push(&request->headers_out.headers);
      if (header == NULL) return NGX_DECLINED;
      header->hash = 1U;
      if (shield_index == 0U) {
        ngx_str_set(&header->key, "CDN-Cache-Control");
      } else {
        ngx_str_set(&header->key, "Surrogate-Control");
      }
      header->value = (ngx_str_t){shield_policy_length, shield_policy};
    }
  }
  buffer->pos = body;
  buffer->last = body + record.entry.length;
  buffer->memory = 1U;
  buffer->last_buf = request == request->main;
  buffer->last_in_chain = 1U;
  output.buf = buffer;
  output.next = NULL;
  status = ngx_http_send_header(request);
  if (status == NGX_ERROR || status > NGX_OK || request->header_only) {
    return status;
  }
  return ngx_http_output_filter(request, &output);
}

ngx_int_t ngx_http_laghu_variant_handler(ngx_http_request_t *request) {
  ngx_http_laghu_loc_conf_t *conf = ngx_http_get_module_loc_conf(request, ngx_http_laghu_module);
  bool administration_candidate;
  bool laghu_asset = false;
  bool laghu_beacon = false;
  bool laghu_admin_prefix = false;
  ngx_int_t status;
  if (request == NULL || request->uri.len == 0U || conf == NULL) {
    return NGX_DECLINED;
  }
  administration_candidate = ngx_http_laghu_administration_candidate(request, conf);
  laghu_admin_prefix = request->uri.len >= sizeof("/.laghu/") - 1U && ngx_strncmp(request->uri.data, "/.laghu/", sizeof("/.laghu/") - 1U) == 0;
  laghu_beacon =
      request->uri.len >= sizeof("/.laghu/beacon/") - 1U && ngx_strncmp(request->uri.data, "/.laghu/beacon/", sizeof("/.laghu/beacon/") - 1U) == 0;
  laghu_asset =
      request->uri.len >= sizeof("/.laghu/image/") - 1U && ngx_strncmp(request->uri.data, "/.laghu/image/", sizeof("/.laghu/image/") - 1U) == 0;
  laghu_asset =
      laghu_asset ||
      (request->uri.len >= sizeof("/.laghu/css/") - 1U && ngx_strncmp(request->uri.data, "/.laghu/css/", sizeof("/.laghu/css/") - 1U) == 0) ||
      (request->uri.len >= sizeof("/.laghu/js/") - 1U && ngx_strncmp(request->uri.data, "/.laghu/js/", sizeof("/.laghu/js/") - 1U) == 0) ||
      (request->uri.len >= sizeof("/.laghu/media/") - 1U && ngx_strncmp(request->uri.data, "/.laghu/media/", sizeof("/.laghu/media/") - 1U) == 0);
  status = ngx_http_laghu_html_cache_handler(request, conf);

  if (status != NGX_DECLINED) return status;
  if (!administration_candidate && !laghu_admin_prefix) {
    return NGX_DECLINED;
  }
  if (administration_candidate || laghu_admin_prefix) {
    status = ngx_http_laghu_admin_endpoint(request, conf);
    if (status != NGX_DECLINED) return status;
  }
  if (laghu_beacon) {
    status = ngx_http_laghu_beacon_endpoint(request, conf);
    if (status != NGX_DECLINED) return status;
  }
  if (laghu_asset) {
    return ngx_http_laghu_asset_endpoint(request, conf);
  }
  return NGX_HTTP_NOT_FOUND;
}
