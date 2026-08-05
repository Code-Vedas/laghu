// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>
#include <string.h>

#include "laghu/catalog.h"
#include "laghu/css.h"
#include "laghu/instrumentation.h"
#include "laghu/operational.h"
#include "laghu/types.h"
#include "ngx_http_laghu_internal.h"

static time_t ngx_http_laghu_beacon_window;
static ngx_uint_t ngx_http_laghu_beacon_count;

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

ngx_int_t ngx_http_laghu_variant_handler(ngx_http_request_t *request) {
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
