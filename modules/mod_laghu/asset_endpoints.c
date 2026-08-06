// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "mod_laghu_internal.h"

int laghu_apache_asset_endpoint(request_rec *request,
                                laghu_apache_config *config) {
  static const char prefix[] = "/.laghu/image/";
  static const char css_prefix[] = "/.laghu/css/";
  static const char javascript_prefix[] = "/.laghu/js/";
  static const char media_prefix[] = "/.laghu/media/";
  laghu_runtime_cache_entry entry;
  unsigned char *body;
  const char *key;
  bool css_asset;
  bool javascript_asset;
  bool javascript_map;
  bool media_asset = false;
  css_asset = strlen(request->uri) ==
                  sizeof(css_prefix) - 1U + LAGHU_SHA256_HEX_LENGTH &&
              strncmp(request->uri, css_prefix, sizeof(css_prefix) - 1U) == 0;
  javascript_asset = strlen(request->uri) == sizeof(javascript_prefix) - 1U +
                                                 LAGHU_SHA256_HEX_LENGTH &&
                     strncmp(request->uri, javascript_prefix,
                             sizeof(javascript_prefix) - 1U) == 0;
  javascript_map =
      strlen(request->uri) ==
          sizeof(javascript_prefix) - 1U + LAGHU_SHA256_HEX_LENGTH + 4U &&
      strncmp(request->uri, javascript_prefix,
              sizeof(javascript_prefix) - 1U) == 0 &&
      strcmp(request->uri + strlen(request->uri) - 4U, ".map") == 0;
  media_asset =
      strlen(request->uri) ==
          sizeof(media_prefix) - 1U + LAGHU_SHA256_HEX_LENGTH &&
      strncmp(request->uri, media_prefix, sizeof(media_prefix) - 1U) == 0;
  if (!css_asset && !javascript_asset && !javascript_map && !media_asset &&
      (strlen(request->uri) != sizeof(prefix) - 1U + LAGHU_SHA256_HEX_LENGTH ||
       strncmp(request->uri, prefix, sizeof(prefix) - 1U) != 0)) {
    return HTTP_NOT_FOUND;
  }
  if (request->method_number != M_GET) {
    return HTTP_METHOD_NOT_ALLOWED;
  }
  key = request->uri + (css_asset ? sizeof(css_prefix) - 1U
                        : (javascript_asset || javascript_map)
                            ? sizeof(javascript_prefix) - 1U
                        : media_asset ? sizeof(media_prefix) - 1U
                                      : sizeof(prefix) - 1U);
  {
    size_t offset;
    for (offset = 0U; offset < LAGHU_SHA256_HEX_LENGTH; ++offset) {
      if (!((key[offset] >= '0' && key[offset] <= '9') ||
            (key[offset] >= 'a' && key[offset] <= 'f'))) {
        return HTTP_NOT_FOUND;
      }
    }
  }
  {
    laghu_apache_context transaction_context;
    laghu_http_transaction_result result;
    const laghu_http_header_operation *operation;
    size_t index;
    memset(&transaction_context, 0, sizeof(transaction_context));
    memset(&result, 0, sizeof(result));
    transaction_context.config = config;
    request->status = HTTP_OK;
    request->clength = 0;
    if (!laghu_apache_normalize(request, &transaction_context) ||
        !laghu_http_transaction_prepare(
            &transaction_context.transaction, &transaction_context.request,
            &transaction_context.response, &transaction_context.environment,
            &result) ||
        result.action != LAGHU_HTTP_ACTION_SERVE_CACHED ||
        result.selected.length == 0U ||
        result.selected.length > LAGHU_IMAGE_MAX_INPUT_BYTES) {
      laghu_http_transaction_result_release(&result);
      return HTTP_NOT_FOUND;
    }
    body = apr_pmemdup(request->pool, result.selected.data,
                       result.selected.length);
    if (body == NULL || !laghu_apache_apply_result(request, &result)) {
      laghu_http_transaction_result_release(&result);
      return HTTP_INTERNAL_SERVER_ERROR;
    }
    entry.length = result.selected.length;
    entry.content_type[0] = '\0';
    for (index = 0U; index < result.header_operation_count; ++index) {
      operation = &result.header_operations[index];
      if (strcmp(operation->name, "Content-Type") == 0 &&
          operation->value != NULL) {
        apr_cpystrn(entry.content_type, operation->value,
                    sizeof(entry.content_type));
      }
    }
    laghu_http_transaction_result_release(&result);
  }
  ap_set_content_type(request, css_asset        ? "text/css"
                               : javascript_map ? "application/json"
                                                : entry.content_type);
  ap_set_content_length(request, (apr_off_t)entry.length);
  apr_table_setn(request->headers_out, "Cache-Control",
                 "public, max-age=31536000, immutable");
  apr_table_set(request->headers_out, "ETag",
                apr_psprintf(request->pool, "\"%s\"", key));
  if (!request->header_only &&
      ap_rwrite(body, (int)entry.length, request) < 0) {
    return HTTP_INTERNAL_SERVER_ERROR;
  }
  return OK;
}
