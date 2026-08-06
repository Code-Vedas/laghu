// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>
#include <string.h>

#include "laghu/catalog.h"
#include "laghu/css.h"
#include "laghu/instrumentation.h"
#include "ngx_http_laghu_internal.h"

static time_t ngx_http_laghu_beacon_window;
static ngx_uint_t ngx_http_laghu_beacon_count;

static bool ngx_http_laghu_same_origin(ngx_http_request_t *request) {
  ngx_list_part_t *part = &request->headers_in.headers.part;
  ngx_table_elt_t *headers = part->elts;
  ngx_uint_t index;
  for (index = 0U;; ++index) {
    if (index >= part->nelts) {
      if (part->next == NULL) return false;
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
                        sizeof("same-origin") - 1U) == 0)
      return true;
  }
}

static ngx_int_t ngx_http_laghu_send_beacon_script(ngx_http_request_t *request,
                                                   const unsigned char *script,
                                                   size_t script_length) {
  ngx_buf_t *buffer;
  ngx_chain_t output;
  request->headers_out.status = NGX_HTTP_OK;
  ngx_str_set(&request->headers_out.content_type, "application/javascript");
  request->headers_out.content_length_n = (off_t)script_length;
  if (ngx_http_send_header(request) == NGX_ERROR ||
      request->method == NGX_HTTP_HEAD)
    return NGX_OK;
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

static ngx_int_t ngx_http_laghu_read_beacon(ngx_http_request_t *request) {
  time_t now = ngx_time();
  if (request->headers_in.content_type == NULL ||
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
  if (ngx_http_read_client_request_body(request, ngx_http_laghu_beacon_body) >=
      NGX_HTTP_SPECIAL_RESPONSE)
    return NGX_HTTP_BAD_REQUEST;
  return NGX_DONE;
}

ngx_int_t ngx_http_laghu_beacon_endpoint(ngx_http_request_t *request,
                                         ngx_http_laghu_loc_conf_t *conf) {
  laghu_http_beacon_options options;
  laghu_http_beacon_plan plan;
  laghu_buffer method = {(const unsigned char *)request->method_name.data,
                         request->method_name.len};
  laghu_buffer target = {(const unsigned char *)request->uri.data,
                         request->uri.len};

  laghu_http_beacon_options_init(&options);
  options.image_enabled = conf->core.mode == LAGHU_MODE_ON &&
                          conf->core.image_beacon == LAGHU_MODE_ON;
  options.critical_css_enabled =
      conf->core.mode == LAGHU_MODE_ON &&
      conf->core.critical_css_beacon == LAGHU_MODE_ON;
  options.instrumentation_enabled =
      conf->core.mode == LAGHU_MODE_ON &&
      conf->core.instrumentation_beacon == LAGHU_MODE_ON;
  if (!laghu_http_beacon_plan_build(&plan, method, target, &options) ||
      !plan.recognized)
    return NGX_DECLINED;
  if (plan.status != 0U) return (ngx_int_t)plan.status;
  switch (plan.route) {
    case LAGHU_HTTP_BEACON_ROUTE_IMAGE_SCRIPT: {
      const char *script = laghu_runtime_image_beacon_script();
      return ngx_http_laghu_send_beacon_script(
          request, (const unsigned char *)script, strlen(script));
    }
    case LAGHU_HTTP_BEACON_ROUTE_IMAGE_REPORT:
    case LAGHU_HTTP_BEACON_ROUTE_CRITICAL_CSS_REPORT:
    case LAGHU_HTTP_BEACON_ROUTE_INSTRUMENTATION_REPORT:
      return ngx_http_laghu_read_beacon(request);
    case LAGHU_HTTP_BEACON_ROUTE_CRITICAL_CSS_SCRIPT: {
      const char *script = laghu_runtime_critical_css_beacon_script();
      return ngx_http_laghu_send_beacon_script(
          request, (const unsigned char *)script, strlen(script));
    }
    case LAGHU_HTTP_BEACON_ROUTE_INSTRUMENTATION_SCRIPT: {
      const char *script = laghu_runtime_instrumentation_script();
      return ngx_http_laghu_send_beacon_script(
          request, (const unsigned char *)script, strlen(script));
    }
    case LAGHU_HTTP_BEACON_ROUTE_NONE:
    default:
      return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }
}
