// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>
#include <string.h>

#include "laghu/catalog.h"
#include "laghu/types.h"
#include "ngx_http_laghu_internal.h"

static void ngx_http_laghu_asset_close_file(void *data) {
  ngx_file_t *file = data;
  if (file != NULL && file->fd != NGX_INVALID_FILE) ngx_close_file(file->fd);
}

ngx_int_t ngx_http_laghu_asset_endpoint(ngx_http_request_t *request, ngx_http_laghu_loc_conf_t *conf) {
  static const char prefix[] = "/.laghu/image/";
  static const char css_prefix[] = "/.laghu/css/";
  static const char javascript_prefix[] = "/.laghu/js/";
  static const char media_prefix[] = "/.laghu/media/";
  laghu_runtime_cache_entry entry;
  ngx_buf_t *buffer;
  ngx_file_t *file;
  ngx_pool_cleanup_t *cleanup;
  ngx_chain_t output;
  ngx_table_elt_t *header;
  char key[LAGHU_RUNTIME_KEY_SIZE];
  bool css_asset = false;
  bool javascript_asset = false;
  bool javascript_map = false;
  bool media_asset = false;

  css_asset = request->uri.len == sizeof(css_prefix) - 1U + LAGHU_SHA256_HEX_LENGTH &&
              ngx_strncmp(request->uri.data, css_prefix, sizeof(css_prefix) - 1U) == 0;
  javascript_asset = request->uri.len == sizeof(javascript_prefix) - 1U + LAGHU_SHA256_HEX_LENGTH &&
                     ngx_strncmp(request->uri.data, javascript_prefix, sizeof(javascript_prefix) - 1U) == 0;
  javascript_map = request->uri.len == sizeof(javascript_prefix) - 1U + LAGHU_SHA256_HEX_LENGTH + 4U &&
                   ngx_strncmp(request->uri.data, javascript_prefix, sizeof(javascript_prefix) - 1U) == 0 &&
                   ngx_strncmp(request->uri.data + request->uri.len - 4U, ".map", 4U) == 0;
  media_asset = request->uri.len == sizeof(media_prefix) - 1U + LAGHU_SHA256_HEX_LENGTH &&
                ngx_strncmp(request->uri.data, media_prefix, sizeof(media_prefix) - 1U) == 0;
  if (!css_asset && !javascript_asset && !javascript_map && !media_asset &&
      (request->uri.len != sizeof(prefix) - 1U + LAGHU_SHA256_HEX_LENGTH || ngx_strncmp(request->uri.data, prefix, sizeof(prefix) - 1U) != 0))
    return NGX_DECLINED;
  if (!(request->method & (NGX_HTTP_GET | NGX_HTTP_HEAD))) return NGX_HTTP_NOT_ALLOWED;
  if (conf->core.mode != LAGHU_MODE_ON) return NGX_HTTP_NOT_FOUND;
  ngx_memcpy(key,
             request->uri.data + (css_asset                              ? sizeof(css_prefix) - 1U
                                  : (javascript_asset || javascript_map) ? sizeof(javascript_prefix) - 1U
                                  : media_asset                          ? sizeof(media_prefix) - 1U
                                                                         : sizeof(prefix) - 1U),
             LAGHU_SHA256_HEX_LENGTH);
  key[LAGHU_SHA256_HEX_LENGTH] = '\0';
  {
    size_t offset;
    for (offset = 0U; offset < LAGHU_SHA256_HEX_LENGTH; ++offset)
      if (!((key[offset] >= '0' && key[offset] <= '9') || (key[offset] >= 'a' && key[offset] <= 'f'))) return NGX_HTTP_NOT_FOUND;
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
        !laghu_http_transaction_prepare(&transaction_context.transaction, &transaction_context.request, &transaction_context.response,
                                        &transaction_context.environment, &result) ||
        result.action != LAGHU_HTTP_ACTION_SERVE_CACHED || result.selected.length == 0U || result.selected.length > LAGHU_IMAGE_MAX_INPUT_BYTES) {
      laghu_http_transaction_result_release(&result);
      return NGX_HTTP_NOT_FOUND;
    }
    if (!result.cached_file || ngx_http_laghu_apply_result(request, &result) != NGX_OK) {
      laghu_http_transaction_result_release(&result);
      return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    entry = result.cached_entry;
    entry.content_type[0] = '\0';
    for (operation_index = 0U; operation_index < result.header_operation_count; ++operation_index) {
      if (strcmp(result.header_operations[operation_index].name, "Content-Type") == 0 && result.header_operations[operation_index].value != NULL) {
        ngx_cpystrn((u_char *)entry.content_type, (u_char *)result.header_operations[operation_index].value, sizeof(entry.content_type));
      }
    }
    laghu_http_transaction_result_release(&result);
  }
  request->headers_out.status = NGX_HTTP_OK;
  request->headers_out.content_length_n = (off_t)entry.length;
  request->headers_out.content_type.data = (u_char *)(css_asset ? "text/css" : javascript_map ? "application/json" : entry.content_type);
  request->headers_out.content_type.len = ngx_strlen(request->headers_out.content_type.data);
  header = ngx_list_push(&request->headers_out.headers);
  if (header == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
  header->hash = 1U;
  ngx_str_set(&header->key, "Cache-Control");
  ngx_str_set(&header->value, "public, max-age=31536000, immutable");
  header = ngx_list_push(&request->headers_out.headers);
  if (header == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
  header->hash = 1U;
  ngx_str_set(&header->key, "ETag");
  header->value.data = ngx_pnalloc(request->pool, LAGHU_SHA256_HEX_LENGTH + 3U);
  if (header->value.data == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
  header->value.len = LAGHU_SHA256_HEX_LENGTH + 2U;
  header->value.data[0] = '"';
  ngx_memcpy(header->value.data + 1U, key, LAGHU_SHA256_HEX_LENGTH);
  header->value.data[LAGHU_SHA256_HEX_LENGTH + 1U] = '"';
  header->value.data[LAGHU_SHA256_HEX_LENGTH + 2U] = '\0';
  if (ngx_http_send_header(request) == NGX_ERROR || request->method == NGX_HTTP_HEAD) return NGX_OK;
  buffer = ngx_calloc_buf(request->pool);
  cleanup = ngx_pool_cleanup_add(request->pool, sizeof(*file));
  if (buffer == NULL || cleanup == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
  file = cleanup->data;
  ngx_memzero(file, sizeof(*file));
  file->fd = NGX_INVALID_FILE;
  file->log = request->connection->log;
  file->name.len = strlen(entry.variant_path);
  file->name.data = ngx_pnalloc(request->pool, file->name.len + 1U);
  if (file->name.data == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
  ngx_memcpy(file->name.data, entry.variant_path, file->name.len + 1U);
  file->fd = ngx_open_file(file->name.data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0U);
  if (file->fd == NGX_INVALID_FILE) return NGX_HTTP_NOT_FOUND;
  cleanup->handler = ngx_http_laghu_asset_close_file;
  buffer->file = file;
  buffer->file_last = entry.length;
  buffer->in_file = 1U;
  buffer->last_buf = 1U;
  output.buf = buffer;
  output.next = NULL;
  return ngx_http_output_filter(request, &output);
}
