// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_http_laghu_internal.h"

ngx_int_t ngx_http_laghu_send_early_hints(ngx_http_request_t *request, const laghu_http_transaction_result *result) {
  ngx_list_part_t *part;
  ngx_table_elt_t *headers;
  ngx_uint_t *hashes;
  size_t count = 0U;
  size_t original_count;
  size_t index;
  bool emitted = false;
  if (request == NULL || result == NULL || request != request->main || request->http_version < NGX_HTTP_VERSION_11) {
    return NGX_OK;
  }
  for (index = 0U; index < result->header_operation_count; ++index) {
    if (result->header_operations[index].early_hint) emitted = true;
  }
  if (!emitted) return NGX_OK;
  for (part = &request->headers_out.headers.part; part != NULL; part = part->next) {
    count += part->nelts;
  }
  original_count = count;
  hashes = ngx_palloc(request->pool, count * sizeof(*hashes));
  if (count != 0U && hashes == NULL) return NGX_ERROR;
  count = 0U;
  for (part = &request->headers_out.headers.part; part != NULL; part = part->next) {
    headers = part->elts;
    for (index = 0U; index < part->nelts; ++index) {
      hashes[count++] = headers[index].hash;
      headers[index].hash = 0U;
    }
  }
  for (index = 0U; index < result->header_operation_count; ++index) {
    const laghu_http_header_operation *operation = &result->header_operations[index];
    ngx_table_elt_t *header;
    if (!operation->early_hint) continue;
    header = ngx_list_push(&request->headers_out.headers);
    if (header == NULL) goto restore_error;
    ngx_memzero(header, sizeof(*header));
    ngx_str_set(&header->key, "Link");
    header->value.len = strlen(operation->value);
    header->value.data = ngx_pnalloc(request->pool, header->value.len);
    if (header->value.data == NULL) goto restore_error;
    ngx_memcpy(header->value.data, operation->value, header->value.len);
    header->hash = 1U;
  }
#if (nginx_version >= 1029000)
  if (ngx_http_send_early_hints(request) == NGX_ERROR) goto restore_error;
#endif
  count = 0U;
  for (part = &request->headers_out.headers.part; part != NULL; part = part->next) {
    headers = part->elts;
    for (index = 0U; index < part->nelts; ++index) {
      headers[index].hash = count < original_count ? hashes[count] : 0U;
      ++count;
    }
  }
  return NGX_OK;

restore_error:
  count = 0U;
  for (part = &request->headers_out.headers.part; part != NULL; part = part->next) {
    headers = part->elts;
    for (index = 0U; index < part->nelts; ++index) {
      headers[index].hash = count < original_count ? hashes[count] : 0U;
      ++count;
    }
  }
  return NGX_ERROR;
}

ngx_int_t ngx_http_laghu_apply_result(ngx_http_request_t *request, const laghu_http_transaction_result *result) {
  ngx_table_elt_t **staged;
  size_t index;
  staged = ngx_pcalloc(request->pool, result->header_operation_count * sizeof(*staged));
  if (result->header_operation_count != 0U && staged == NULL) {
    return NGX_ERROR;
  }
  for (index = 0U; index < result->header_operation_count; ++index) {
    const laghu_http_header_operation *operation = &result->header_operations[index];
    if (operation->kind == LAGHU_HTTP_HEADER_REMOVE) {
      continue;
    }
    staged[index] = ngx_list_push(&request->headers_out.headers);
    if (staged[index] == NULL) {
      return NGX_ERROR;
    }
    ngx_memzero(staged[index], sizeof(**staged));
    staged[index]->key.len = strlen(operation->name);
    staged[index]->key.data = ngx_pnalloc(request->pool, staged[index]->key.len + 1U);
    staged[index]->value.len = strlen(operation->value);
    staged[index]->value.data = ngx_pnalloc(request->pool, staged[index]->value.len + 1U);
    if (staged[index]->key.data == NULL || staged[index]->value.data == NULL) {
      return NGX_ERROR;
    }
    ngx_memcpy(staged[index]->key.data, operation->name, staged[index]->key.len + 1U);
    ngx_memcpy(staged[index]->value.data, operation->value, staged[index]->value.len + 1U);
  }
  for (index = 0U; index < result->header_operation_count; ++index) {
    const laghu_http_header_operation *operation = &result->header_operations[index];
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
      } else if (ngx_strcasecmp(header->key.data, (u_char *)"Content-Length") == 0) {
        request->headers_out.content_length_n = ngx_atoof(header->value.data, header->value.len);
        request->headers_out.content_length = header;
      } else if (ngx_strcasecmp(header->key.data, (u_char *)"ETag") == 0) {
        request->headers_out.etag = header;
      }
    }
  }
  return NGX_OK;
}
