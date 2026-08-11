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

bool ngx_http_laghu_peer_matches(ngx_http_request_t *request,
                                 const laghu_service_cidr *cidrs,
                                 size_t count) {
  const struct sockaddr *peer = request->connection->sockaddr;
  unsigned char address[16U] = {0};
  unsigned int family;
  size_t index;
  if (peer == NULL) return false;
  if (peer->sa_family == AF_INET) {
    ngx_memcpy(address, &((const struct sockaddr_in *)peer)->sin_addr, 4U);
    family = LAGHU_SERVICE_CIDR_FAMILY_IPV4;
  }
#if (NGX_HAVE_INET6)
  else if (peer->sa_family == AF_INET6) {
    ngx_memcpy(address, &((const struct sockaddr_in6 *)peer)->sin6_addr, 16U);
    family = LAGHU_SERVICE_CIDR_FAMILY_IPV6;
  }
#endif
  else
    return false;
  for (index = 0U; index < count; ++index)
    if (laghu_service_cidr_matches(&cidrs[index], address, family)) return true;
  return false;
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

void ngx_http_laghu_remove_header(ngx_http_request_t *request,
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
      ngx_http_laghu_peer_matches(request, conf->service.trusted_proxies,
                                  conf->service.trusted_proxy_count)) {
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
  context->response.status = request->headers_out.status == 0U
                                 ? NGX_HTTP_OK
                                 : (unsigned int)request->headers_out.status;
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
  context->environment.cache_path = conf->service.image_cache;
  context->environment.rum = ngx_http_laghu_rum;
  context->environment.queue = ngx_http_laghu_image_queue(conf);
  context->environment.font_fetch_queue = ngx_http_laghu_font_queue(conf);
  context->environment.font_providers = conf->service.font_providers;
  context->environment.javascript_queue = ngx_http_laghu_javascript_queue(conf);
  context->environment.chrome_analysis_queue =
      ngx_http_laghu_chrome_analysis_queue(conf);
  context->environment.chrome_analysis_timeout_ms =
      conf->service.chrome_analysis_timeout_ms;
  context->environment.javascript_target = conf->service.javascript_target;
  context->environment.javascript_observations =
      conf->service.javascript_observations;
  context->environment.javascript_defer = conf->service.javascript_defer;
  context->environment.asset_offload = conf->service.asset_offload;
  context->environment.now = (uint64_t)ngx_time();
  return true;
}
