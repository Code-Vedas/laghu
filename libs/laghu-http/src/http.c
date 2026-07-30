// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char laghu_http_ascii_lower(unsigned char value) {
  return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A'))
                                      : value;
}

static bool laghu_http_view_valid(laghu_buffer value) {
  return value.length == 0U || value.data != NULL;
}

static bool laghu_http_view_equal(laghu_buffer left, const char *right) {
  size_t index;
  size_t length = strlen(right);
  if (!laghu_http_view_valid(left) || left.length != length) {
    return false;
  }
  for (index = 0U; index < length; ++index) {
    if (laghu_http_ascii_lower(left.data[index]) !=
        laghu_http_ascii_lower((unsigned char)right[index])) {
      return false;
    }
  }
  return true;
}

static bool laghu_http_copy_view(laghu_buffer value, char *output,
                                 size_t capacity) {
  if (!laghu_http_view_valid(value) || output == NULL || capacity == 0U ||
      value.length >= capacity) {
    return false;
  }
  if (value.length != 0U) {
    memcpy(output, value.data, value.length);
  }
  output[value.length] = '\0';
  return true;
}

static bool laghu_http_header_name_equal(laghu_buffer name,
                                         const char *expected) {
  return laghu_http_view_equal(name, expected);
}

static const laghu_http_header *laghu_http_find_header(
    const laghu_http_header *headers, size_t count, const char *name) {
  size_t index;
  for (index = 0U; index < count; ++index) {
    if (laghu_http_header_name_equal(headers[index].name, name)) {
      return &headers[index];
    }
  }
  return NULL;
}

static bool laghu_http_headers_valid(const laghu_http_header *headers,
                                     size_t count, size_t maximum) {
  size_t index;
  if (count > maximum || (count != 0U && headers == NULL)) {
    return false;
  }
  for (index = 0U; index < count; ++index) {
    size_t offset;
    if (!laghu_http_view_valid(headers[index].name) ||
        !laghu_http_view_valid(headers[index].value) ||
        headers[index].name.length == 0U ||
        headers[index].name.length > LAGHU_HTTP_MAX_HEADER_NAME ||
        headers[index].value.length > LAGHU_HTTP_MAX_HEADER_VALUE) {
      return false;
    }
    for (offset = 0U; offset < headers[index].name.length; ++offset) {
      unsigned char value = headers[index].name.data[offset];
      if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '!' || value == '#' ||
            value == '$' || value == '%' || value == '&' || value == '\'' ||
            value == '*' || value == '+' || value == '-' || value == '.' ||
            value == '^' || value == '_' || value == '`' || value == '|' ||
            value == '~')) {
        return false;
      }
    }
    for (offset = 0U; offset < headers[index].value.length; ++offset) {
      unsigned char value = headers[index].value.data[offset];
      if (value == '\0' || value == '\r' || value == '\n') {
        return false;
      }
    }
  }
  return true;
}

static bool laghu_http_normalized_view_valid(laghu_buffer value, bool path) {
  size_t index;
  if (!laghu_http_view_valid(value) || value.length == 0U ||
      (path && value.data[0] != '/')) {
    return false;
  }
  for (index = 0U; index < value.length; ++index) {
    if (value.data[index] == '\0' || value.data[index] == '\r' ||
        value.data[index] == '\n') {
      return false;
    }
  }
  return true;
}

static bool laghu_http_copy_header(const laghu_http_header *header,
                                   char *output, size_t capacity) {
  if (header == NULL) {
    if (output != NULL && capacity != 0U) {
      output[0] = '\0';
    }
    return true;
  }
  return laghu_http_copy_view(header->value, output, capacity);
}

static bool laghu_http_join_response_headers(
    const laghu_http_response *response, const char *name, char *output,
    size_t capacity);

static bool laghu_http_header_contains(const laghu_http_header *header,
                                       const char *needle) {
  size_t index;
  size_t needle_length = strlen(needle);
  if (header == NULL || needle_length == 0U ||
      header->value.length < needle_length) {
    return false;
  }
  for (index = 0U; index + needle_length <= header->value.length; ++index) {
    size_t offset;
    for (offset = 0U; offset < needle_length; ++offset) {
      if (laghu_http_ascii_lower(header->value.data[index + offset]) !=
          laghu_http_ascii_lower((unsigned char)needle[offset])) {
        break;
      }
    }
    if (offset == needle_length) {
      return true;
    }
  }
  return false;
}

static bool laghu_http_content_type_is(const char *content_type,
                                       const char *prefix) {
  size_t index;
  size_t length = strlen(prefix);
  if (content_type == NULL || strlen(content_type) < length) {
    return false;
  }
  for (index = 0U; index < length; ++index) {
    if (laghu_http_ascii_lower((unsigned char)content_type[index]) !=
        laghu_http_ascii_lower((unsigned char)prefix[index])) {
      return false;
    }
  }
  return true;
}

static void laghu_http_result_init(laghu_http_transaction_result *result) {
  if (result != NULL) {
    memset(result, 0, sizeof(*result));
    result->version = LAGHU_HTTP_ABI_VERSION;
    result->struct_size = sizeof(*result);
    result->action = LAGHU_HTTP_ACTION_BYPASS;
    result->decision = LAGHU_DECISION_BYPASS_ERROR;
  }
}

void laghu_http_transaction_result_release(
    laghu_http_transaction_result *result) {
  size_t index;
  if (result == NULL) {
    return;
  }
  free(result->owned_body);
  for (index = 0U; index < result->header_operation_count; ++index) {
    free(result->header_operations[index].value);
  }
  laghu_http_result_init(result);
}

static bool laghu_http_add_header_operation(
    laghu_http_transaction_result *result,
    laghu_http_header_operation_kind kind, const char *name,
    const char *value) {
  laghu_http_header_operation *operation;
  size_t name_length;
  size_t value_length = value == NULL ? 0U : strlen(value);
  if (result == NULL || name == NULL ||
      result->header_operation_count >= LAGHU_HTTP_MAX_HEADER_OPERATIONS) {
    return false;
  }
  name_length = strlen(name);
  if (name_length == 0U || name_length > LAGHU_HTTP_MAX_HEADER_NAME ||
      value_length > LAGHU_HTTP_MAX_HEADER_VALUE ||
      (kind != LAGHU_HTTP_HEADER_REMOVE && value == NULL)) {
    return false;
  }
  operation = &result->header_operations[result->header_operation_count++];
  memset(operation, 0, sizeof(*operation));
  operation->kind = kind;
  memcpy(operation->name, name, name_length + 1U);
  if (value != NULL) {
    operation->value = malloc(value_length + 1U);
    if (operation->value == NULL) {
      --result->header_operation_count;
      return false;
    }
    memcpy(operation->value, value, value_length + 1U);
  }
  return true;
}

static bool laghu_http_add_status(laghu_http_transaction_result *result,
                                  laghu_decision decision) {
  result->decision = decision;
  return laghu_http_add_header_operation(
      result, LAGHU_HTTP_HEADER_SET, "X-Laghu", laghu_decision_name(decision));
}

static laghu_image_filter_mask laghu_http_image_filters(
    const laghu_policy *policy) {
  laghu_image_filter_mask filters =
      LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_RECOMPRESS_IMAGES |
      LAGHU_IMAGE_RECOMPRESS_JPEG | LAGHU_IMAGE_RECOMPRESS_PNG |
      LAGHU_IMAGE_RECOMPRESS_WEBP;
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_METADATA) != 0U) {
    filters |= LAGHU_IMAGE_STRIP_METADATA | LAGHU_IMAGE_STRIP_COLOR_PROFILE;
  }
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_MODERN) != 0U) {
    filters |= LAGHU_IMAGE_JPEG_PROGRESSIVE | LAGHU_IMAGE_JPEG_TO_WEBP |
               LAGHU_IMAGE_PNG_TO_JPEG | LAGHU_IMAGE_GIF_TO_PNG |
               LAGHU_IMAGE_TO_WEBP_LOSSLESS | LAGHU_IMAGE_TO_WEBP_ANIMATED |
               LAGHU_IMAGE_JPEG_SAMPLING | LAGHU_IMAGE_IN_PLACE_BROWSER;
  }
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_RESPONSIVE) != 0U) {
    filters |= LAGHU_IMAGE_RESIZE_ATTRIBUTE | LAGHU_IMAGE_RESIZE_RENDERED |
               LAGHU_IMAGE_RESIZE_MOBILE | LAGHU_IMAGE_RESPONSIVE |
               LAGHU_IMAGE_RESPONSIVE_ZOOM;
  }
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_DIMENSIONS) != 0U) {
    filters |= LAGHU_IMAGE_INSERT_DIMENSIONS;
  }
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_LAZYLOAD) != 0U) {
    filters |= LAGHU_IMAGE_LAZYLOAD | LAGHU_IMAGE_INLINE_PREVIEW;
  }
  if ((policy->filter_families & LAGHU_FILTER_RESOURCE_INLINE) != 0U) {
    filters |= LAGHU_IMAGE_INLINE | LAGHU_IMAGE_DEDUP_INLINE;
  }
  return filters;
}

static bool laghu_http_backend_supports(const char *content_type,
                                        laghu_image_filter_mask filters,
                                        uint32_t capabilities,
                                        bool allow_lossy) {
  if (laghu_http_content_type_is(content_type, "image/jpeg")) {
    return allow_lossy && (capabilities & LAGHU_IMAGE_CAP_JPEG_LOAD) != 0U &&
           (((capabilities & LAGHU_IMAGE_CAP_JPEG_SAVE) != 0U &&
             (filters &
              (LAGHU_IMAGE_RECOMPRESS_IMAGES | LAGHU_IMAGE_RECOMPRESS_JPEG |
               LAGHU_IMAGE_JPEG_PROGRESSIVE | LAGHU_IMAGE_JPEG_SAMPLING)) !=
                 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U &&
             (filters & LAGHU_IMAGE_JPEG_TO_WEBP) != 0U));
  }
  if (laghu_http_content_type_is(content_type, "image/png")) {
    return (capabilities & LAGHU_IMAGE_CAP_PNG_LOAD) != 0U &&
           (((capabilities & LAGHU_IMAGE_CAP_PNG_SAVE) != 0U &&
             (filters & (LAGHU_IMAGE_RECOMPRESS_IMAGES |
                         LAGHU_IMAGE_RECOMPRESS_PNG)) != 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_JPEG_SAVE) != 0U && allow_lossy &&
             (filters & LAGHU_IMAGE_PNG_TO_JPEG) != 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U &&
             (filters & LAGHU_IMAGE_TO_WEBP_LOSSLESS) != 0U));
  }
  if (laghu_http_content_type_is(content_type, "image/gif")) {
    return (capabilities & LAGHU_IMAGE_CAP_GIF_LOAD) != 0U &&
           (((capabilities & LAGHU_IMAGE_CAP_PNG_SAVE) != 0U &&
             (filters & LAGHU_IMAGE_GIF_TO_PNG) != 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U &&
             (filters & (LAGHU_IMAGE_TO_WEBP_LOSSLESS |
                         LAGHU_IMAGE_TO_WEBP_ANIMATED)) != 0U));
  }
  return laghu_http_content_type_is(content_type, "image/webp") &&
         (capabilities & LAGHU_IMAGE_CAP_WEBP_LOAD) != 0U &&
         (capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U;
}

static laghu_html_planner_mask laghu_http_html_plan(
    const laghu_policy *policy) {
  laghu_html_planner_mask plan = 0U;
  bool html = (policy->filter_families & LAGHU_FILTER_HTML_MINIFY) != 0U;
  bool css = (policy->filter_families & LAGHU_FILTER_CSS_MINIFY) != 0U;
  if (html) {
    plan |= LAGHU_HTML_PLAN_LEXICAL | LAGHU_HTML_PLAN_CONVERT_META_TAGS;
  }
  if ((policy->filter_families & LAGHU_FILTER_RESOURCE_HINTS) != 0U) {
    plan |= LAGHU_HTML_PLAN_RESOURCE_HINTS;
  }
  if (html && policy->allow_structural_rewrite) {
    plan |= LAGHU_HTML_PLAN_ADD_COMBINE_HEAD;
  }
  if (html && css && policy->allow_structural_rewrite) {
    plan |= LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD;
    if (policy->allow_script_reordering) {
      plan |= LAGHU_HTML_PLAN_MOVE_CSS_ABOVE_SCRIPTS;
    }
  }
  return plan;
}

static bool laghu_http_method_is(const laghu_http_request *request,
                                 const char *method) {
  return laghu_http_view_equal(request->method, method);
}

static bool laghu_http_accepts_webp(const laghu_http_request *request) {
  size_t header_index;
  for (header_index = 0U; header_index < request->header_count;
       ++header_index) {
    laghu_buffer value;
    size_t start = 0U;
    if (!laghu_http_header_name_equal(request->headers[header_index].name,
                                      "Accept")) {
      continue;
    }
    value = request->headers[header_index].value;
    while (start < value.length) {
      size_t end = start;
      size_t type_end;
      bool accepted = true;
      while (end < value.length && value.data[end] != ',') {
        ++end;
      }
      while (start < end &&
             (value.data[start] == ' ' || value.data[start] == '\t')) {
        ++start;
      }
      type_end = start;
      while (type_end < end && value.data[type_end] != ';' &&
             value.data[type_end] != ' ' && value.data[type_end] != '\t') {
        ++type_end;
      }
      if (type_end - start == sizeof("image/webp") - 1U &&
          laghu_http_view_equal(
              (laghu_buffer){value.data + start, type_end - start},
              "image/webp")) {
        size_t parameter = type_end;
        while (parameter < end) {
          size_t quality;
          while (parameter < end && (value.data[parameter] == ';' ||
                                     value.data[parameter] == ' ' ||
                                     value.data[parameter] == '\t')) {
            ++parameter;
          }
          if (parameter + 1U < end &&
              (value.data[parameter] == 'q' || value.data[parameter] == 'Q')) {
            quality = parameter + 1U;
            while (quality < end && (value.data[quality] == ' ' ||
                                     value.data[quality] == '\t')) {
              ++quality;
            }
            if (quality < end && value.data[quality] == '=') {
              ++quality;
              while (quality < end && (value.data[quality] == ' ' ||
                                       value.data[quality] == '\t')) {
                ++quality;
              }
              accepted = false;
              while (quality < end && value.data[quality] != ';' &&
                     value.data[quality] != ' ' &&
                     value.data[quality] != '\t') {
                if (value.data[quality] >= '1' && value.data[quality] <= '9') {
                  accepted = true;
                }
                ++quality;
              }
              break;
            }
          }
          while (parameter < end && value.data[parameter] != ';') {
            ++parameter;
          }
        }
        if (accepted) {
          return true;
        }
      }
      start = end + 1U;
    }
  }
  return false;
}

static bool laghu_http_contract_valid(
    const laghu_http_request *request, const laghu_http_response *response,
    const laghu_http_environment *environment) {
  return request != NULL && response != NULL && environment != NULL &&
         request->version == LAGHU_HTTP_ABI_VERSION &&
         request->struct_size == sizeof(*request) &&
         response->version == LAGHU_HTTP_ABI_VERSION &&
         response->struct_size == sizeof(*response) &&
         environment->version == LAGHU_HTTP_ABI_VERSION &&
         environment->struct_size == sizeof(*environment) &&
         laghu_http_normalized_view_valid(request->method, false) &&
         laghu_http_normalized_view_valid(request->scheme, false) &&
         laghu_http_normalized_view_valid(request->authority, false) &&
         laghu_http_normalized_view_valid(request->normalized_path, true) &&
         laghu_http_headers_valid(request->headers, request->header_count,
                                  LAGHU_HTTP_MAX_REQUEST_HEADERS) &&
         laghu_http_headers_valid(response->headers, response->header_count,
                                  LAGHU_HTTP_MAX_RESPONSE_HEADERS) &&
         laghu_http_view_valid(response->source_validator) &&
         environment->cache_path != NULL;
}

static uint32_t laghu_http_refresh_backend(
    laghu_http_environment *environment) {
  laghu_runtime_queue *queue;
  if (environment == NULL || environment->queue == NULL) {
    return 0U;
  }
  queue = environment->queue;
  if (queue->mapping == NULL &&
      (environment->worker_queue_path == NULL ||
       !laghu_runtime_queue_open(queue, environment->worker_queue_path))) {
    return 0U;
  }
  if (!laghu_runtime_queue_refresh(queue) || queue->capabilities == 0U ||
      queue->worker_heartbeat == 0U || environment->now == 0U ||
      queue->worker_heartbeat > environment->now ||
      environment->now - queue->worker_heartbeat > 45U) {
    return 0U;
  }
  return queue->capabilities;
}

static unsigned int laghu_http_parse_uint_header(
    const laghu_http_request *request, const char *name, unsigned int maximum) {
  const laghu_http_header *header =
      laghu_http_find_header(request->headers, request->header_count, name);
  unsigned int value = 0U;
  size_t index;
  if (header == NULL || header->value.length == 0U) {
    return 0U;
  }
  for (index = 0U; index < header->value.length; ++index) {
    unsigned char byte = header->value.data[index];
    if (byte < '0' || byte > '9' || value > (maximum - (byte - '0')) / 10U) {
      return 0U;
    }
    value = value * 10U + (unsigned int)(byte - '0');
  }
  return value > 0U && value <= maximum ? value : 0U;
}

static unsigned int laghu_http_parse_dpr(const laghu_http_request *request) {
  const laghu_http_header *header =
      laghu_http_find_header(request->headers, request->header_count, "DPR");
  unsigned int whole = 0U;
  unsigned int fraction = 0U;
  unsigned int digits = 0U;
  bool decimal = false;
  size_t index;
  if (header == NULL || header->value.length == 0U) {
    return 100U;
  }
  for (index = 0U; index < header->value.length; ++index) {
    unsigned char byte = header->value.data[index];
    if (byte == '.' && !decimal) {
      decimal = true;
      continue;
    }
    if (byte < '0' || byte > '9') {
      return 100U;
    }
    if (!decimal) {
      if (whole > 4U) {
        return 100U;
      }
      whole = whole * 10U + (unsigned int)(byte - '0');
    } else if (digits < 2U) {
      fraction = fraction * 10U + (unsigned int)(byte - '0');
      ++digits;
    }
  }
  if (decimal && digits == 0U) {
    return 100U;
  }
  if (digits == 1U) {
    fraction *= 10U;
  }
  whole = whole * 100U + fraction;
  return whole >= 100U && whole <= 400U ? whole : 100U;
}

static bool laghu_http_internal_asset_key(const char *path,
                                          char output[LAGHU_RUNTIME_KEY_SIZE]) {
  static const char image_prefix[] = "/.laghu/image/";
  static const char css_prefix[] = "/.laghu/css/";
  const char *key = NULL;
  size_t index;
  if (path == NULL) {
    return false;
  }
  if (strncmp(path, image_prefix, sizeof(image_prefix) - 1U) == 0) {
    key = path + sizeof(image_prefix) - 1U;
  } else if (strncmp(path, css_prefix, sizeof(css_prefix) - 1U) == 0) {
    key = path + sizeof(css_prefix) - 1U;
  }
  if (key == NULL || strlen(key) != LAGHU_SHA256_HEX_LENGTH) {
    return false;
  }
  for (index = 0U; index < LAGHU_SHA256_HEX_LENGTH; ++index) {
    if (!((key[index] >= '0' && key[index] <= '9') ||
          (key[index] >= 'a' && key[index] <= 'f'))) {
      return false;
    }
  }
  memcpy(output, key, LAGHU_RUNTIME_KEY_SIZE);
  return true;
}

void laghu_http_transaction_init(laghu_http_transaction *transaction) {
  if (transaction != NULL) {
    memset(transaction, 0, sizeof(*transaction));
    transaction->version = LAGHU_HTTP_ABI_VERSION;
    transaction->struct_size = sizeof(*transaction);
    transaction->decision = LAGHU_DECISION_BYPASS_ERROR;
  }
}

static bool laghu_http_set_origin(laghu_http_transaction *transaction,
                                  const laghu_http_request *request) {
  if (!laghu_http_copy_view(request->normalized_path, transaction->path,
                            sizeof(transaction->path)) ||
      request->scheme.length + request->authority.length + 4U >
          sizeof(transaction->origin)) {
    return false;
  }
  if (request->scheme.length == 0U || request->authority.length == 0U) {
    transaction->origin[0] = '\0';
    return true;
  }
  memcpy(transaction->origin, request->scheme.data, request->scheme.length);
  memcpy(transaction->origin + request->scheme.length, "://", 3U);
  memcpy(transaction->origin + request->scheme.length + 3U,
         request->authority.data, request->authority.length);
  transaction->origin[request->scheme.length + request->authority.length + 3U] =
      '\0';
  return true;
}

static bool laghu_http_copy_cached_result(
    laghu_http_transaction_result *result,
    const laghu_runtime_cache_entry *entry) {
  result->owned_body = malloc(entry->length);
  if (result->owned_body == NULL ||
      !laghu_runtime_cache_read(entry, result->owned_body, entry->length)) {
    free(result->owned_body);
    result->owned_body = NULL;
    return false;
  }
  result->selected = (laghu_buffer){result->owned_body, entry->length};
  result->action = LAGHU_HTTP_ACTION_SERVE_CACHED;
  return laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET,
                                         "Content-Type", entry->content_type);
}

static bool laghu_http_add_length(laghu_http_transaction_result *result,
                                  size_t length) {
  char value[32U];
  int written = snprintf(value, sizeof(value), "%zu", length);
  return written > 0 && (size_t)written < sizeof(value) &&
         laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET,
                                         "Content-Length", value);
}

static bool laghu_http_finish_cached_headers(
    laghu_http_transaction_result *result,
    const laghu_runtime_cache_entry *entry) {
  char etag[LAGHU_RUNTIME_KEY_SIZE + 16U];
  (void)snprintf(etag, sizeof(etag), "\"laghu-%s\"", entry->payload_hash);
  return laghu_http_add_length(result, entry->length) &&
         laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "Vary",
                                         "Accept") &&
         laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "ETag",
                                         etag) &&
         laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_REMOVE,
                                         "Content-MD5", NULL) &&
         laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_REMOVE,
                                         "Digest", NULL);
}

bool laghu_http_transaction_prepare(laghu_http_transaction *transaction,
                                    const laghu_http_request *request,
                                    const laghu_http_response *response,
                                    const laghu_http_environment *environment,
                                    laghu_http_transaction_result *result) {
  const laghu_http_header *content_type;
  const laghu_http_header *cache_control;
  const laghu_http_header *encoding;
  const laghu_http_header *etag;
  laghu_response classification;
  laghu_runtime_cache_entry cache_entry;
  laghu_decision decision;
  laghu_http_result_init(result);
  if (transaction == NULL || result == NULL ||
      transaction->version != LAGHU_HTTP_ABI_VERSION ||
      transaction->struct_size != sizeof(*transaction) ||
      !laghu_http_contract_valid(request, response, environment)) {
    return result != NULL &&
           laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) && false;
  }
  laghu_http_transaction_init(transaction);
  transaction->request = request;
  transaction->response = response;
  transaction->environment = *environment;
  transaction->capability_mask =
      laghu_http_refresh_backend(&transaction->environment);
  transaction->viewport_width =
      laghu_http_parse_uint_header(request, "Sec-CH-Viewport-Width", 8192U);
  if (transaction->viewport_width == 0U) {
    transaction->viewport_width =
        laghu_http_parse_uint_header(request, "Viewport-Width", 8192U);
  }
  transaction->dpr_hundredths = laghu_http_parse_dpr(request);
  if (!laghu_http_set_origin(transaction, request)) {
    return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) && false;
  }
  if (laghu_http_internal_asset_key(transaction->path,
                                    transaction->cache_key) &&
      laghu_runtime_cache_lookup_variant(
          environment->cache_path, transaction->cache_key, &cache_entry)) {
    char asset_etag[LAGHU_RUNTIME_KEY_SIZE + 24U];
    (void)snprintf(asset_etag, sizeof(asset_etag), "\"%s\"",
                   transaction->cache_key);
    if (strncmp(transaction->path, "/.laghu/css/", 12U) == 0) {
      memcpy(cache_entry.content_type, "text/css", sizeof("text/css"));
    }
    if (!laghu_http_copy_cached_result(result, &cache_entry) ||
        !laghu_http_add_length(result, cache_entry.length) ||
        !laghu_http_add_header_operation(
            result, LAGHU_HTTP_HEADER_SET, "Cache-Control",
            "public, max-age=31536000, immutable") ||
        !laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "ETag",
                                         asset_etag) ||
        !laghu_http_add_status(result, LAGHU_DECISION_IMAGE_HIT)) {
      laghu_http_transaction_result_release(result);
      return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) &&
             false;
    }
    transaction->action = LAGHU_HTTP_ACTION_SERVE_CACHED;
    transaction->decision = LAGHU_DECISION_IMAGE_HIT;
    transaction->prepared = true;
    memcpy(result->cache_key, transaction->cache_key,
           sizeof(result->cache_key));
    return true;
  }
  content_type = laghu_http_find_header(response->headers,
                                        response->header_count, "Content-Type");
  cache_control = laghu_http_find_header(
      response->headers, response->header_count, "Cache-Control");
  encoding = laghu_http_find_header(response->headers, response->header_count,
                                    "Content-Encoding");
  etag =
      laghu_http_find_header(response->headers, response->header_count, "ETag");
  if (!laghu_http_copy_header(content_type, transaction->content_type,
                              sizeof(transaction->content_type)) ||
      !laghu_http_copy_header(etag, transaction->validator,
                              sizeof(transaction->validator))) {
    return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) && false;
  }
  if ((transaction->validator[0] == 'W' || transaction->validator[0] == 'w') &&
      transaction->validator[1] == '/') {
    transaction->validator[0] = '\0';
  }
  if (response->source_validator.length != 0U) {
    if (!laghu_http_copy_view(response->source_validator,
                              transaction->validator,
                              sizeof(transaction->validator)) ||
        ((transaction->validator[0] == 'W' ||
          transaction->validator[0] == 'w') &&
         transaction->validator[1] == '/')) {
      transaction->validator[0] = '\0';
    }
  }
  classification.status = response->status;
  classification.request_path = transaction->path;
  classification.content_type =
      content_type == NULL ? NULL : transaction->content_type;
  classification.cache_control = NULL;
  if (cache_control != NULL) {
    if (!laghu_http_join_response_headers(response, "Cache-Control",
                                          transaction->cache_control,
                                          sizeof(transaction->cache_control))) {
      return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) &&
             false;
    }
    classification.cache_control = transaction->cache_control;
  }
  classification.has_authorization =
      laghu_http_find_header(request->headers, request->header_count,
                             "Authorization") != NULL;
  decision = laghu_decide(&environment->config, &classification);
  if (decision == LAGHU_DECISION_PASS &&
      (!response->complete || response->partial)) {
    decision = LAGHU_DECISION_BYPASS_STATUS;
  }
  if (decision == LAGHU_DECISION_PASS && encoding != NULL &&
      encoding->value.length != 0U) {
    decision = LAGHU_DECISION_BYPASS_ENCODED;
  }
  if (decision == LAGHU_DECISION_PASS &&
      (!laghu_http_method_is(request, "GET") ||
       laghu_http_method_is(request, "HEAD"))) {
    decision = LAGHU_DECISION_PASS;
    transaction->action = LAGHU_HTTP_ACTION_BYPASS;
  }
  if (decision != LAGHU_DECISION_PASS ||
      !laghu_http_method_is(request, "GET")) {
    transaction->decision = decision;
    transaction->prepared = true;
    result->action = LAGHU_HTTP_ACTION_BYPASS;
    return laghu_http_add_status(result, decision);
  }
  if (!laghu_resolve_config_policy(&environment->config,
                                   &transaction->policy) ||
      !laghu_variant_key((laghu_buffer){NULL, 0U}, &transaction->policy,
                         transaction->policy_key)) {
    return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) && false;
  }
  transaction->image_filters = laghu_http_image_filters(&transaction->policy);
  transaction->html_plan = laghu_http_html_plan(&transaction->policy);
  transaction->accept_webp = laghu_http_accepts_webp(request);
  if (laghu_http_content_type_is(transaction->content_type, "text/html")) {
    if (!response->has_declared_length || response->declared_length == 0U ||
        response->declared_length > LAGHU_IMAGE_MAX_INPUT_BYTES) {
      transaction->decision = LAGHU_DECISION_PASS;
      transaction->prepared = true;
      return laghu_http_add_status(result, LAGHU_DECISION_PASS);
    }
    transaction->action = LAGHU_HTTP_ACTION_CAPTURE_HTML;
    result->capture_limit = LAGHU_IMAGE_MAX_INPUT_BYTES;
  } else if (laghu_http_content_type_is(transaction->content_type,
                                        "text/css")) {
    if (!response->has_declared_length || response->declared_length == 0U ||
        response->declared_length > LAGHU_CSS_MAX_INPUT_BYTES ||
        ((transaction->policy.filter_families & LAGHU_FILTER_CSS_MINIFY) ==
             0U &&
         !transaction->policy.allow_structural_rewrite)) {
      transaction->decision = LAGHU_DECISION_PASS;
      transaction->prepared = true;
      return laghu_http_add_status(result, LAGHU_DECISION_PASS);
    }
    transaction->action = LAGHU_HTTP_ACTION_CAPTURE_CSS;
    result->capture_limit = LAGHU_CSS_MAX_INPUT_BYTES;
  } else if (laghu_http_content_type_is(transaction->content_type, "image/")) {
    if (!laghu_runtime_index_key(
            transaction->path, transaction->validator, transaction->policy_key,
            transaction->accept_webp, transaction->cache_key)) {
      return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) &&
             false;
    }
    memcpy(result->cache_key, transaction->cache_key,
           sizeof(result->cache_key));
    {
      laghu_catalog_record catalog;
      if (laghu_catalog_lookup_url(
              environment->cache_path, transaction->path,
              transaction->policy_key, transaction->capability_mask,
              environment->now, environment->config.image_metadata_ttl,
              &catalog)) {
        unsigned int index;
        for (index = 0U; index < catalog.variant_count &&
                         transaction->target_count < LAGHU_RUNTIME_MAX_TARGETS;
             ++index) {
          if (!catalog.variants[index].ready &&
              !catalog.variants[index].terminally_excluded &&
              catalog.variants[index].width > 0U) {
            unsigned int target = transaction->target_count++;
            transaction->target_width[target] = catalog.variants[index].width;
            transaction->target_height[target] = catalog.variants[index].height;
            transaction->resize_filter[target] = LAGHU_IMAGE_RESIZE_ATTRIBUTE;
          }
        }
      }
    }
    if (transaction->validator[0] != '\0' && transaction->target_count == 0U &&
        laghu_runtime_cache_lookup(environment->cache_path,
                                   transaction->cache_key,
                                   transaction->validator, &cache_entry) &&
        cache_entry.length != 0U &&
        cache_entry.length <= LAGHU_IMAGE_MAX_INPUT_BYTES) {
      if (laghu_http_copy_cached_result(result, &cache_entry) &&
          laghu_http_finish_cached_headers(result, &cache_entry) &&
          laghu_http_add_status(result, LAGHU_DECISION_IMAGE_HIT)) {
        transaction->action = LAGHU_HTTP_ACTION_SERVE_CACHED;
        transaction->decision = LAGHU_DECISION_IMAGE_HIT;
        transaction->prepared = true;
        return true;
      }
      {
        laghu_http_transaction_result_release(result);
      }
    }
    if (transaction->image_filters == 0U || environment->queue == NULL ||
        !laghu_http_backend_supports(
            transaction->content_type, transaction->image_filters,
            transaction->capability_mask, transaction->policy.allow_lossy)) {
      transaction->decision = LAGHU_DECISION_BYPASS_IMAGE_BACKEND;
      transaction->prepared = true;
      return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_IMAGE_BACKEND);
    }
    if (response->has_declared_length &&
        response->declared_length > LAGHU_IMAGE_MAX_INPUT_BYTES) {
      transaction->decision = LAGHU_DECISION_PASS;
      transaction->prepared = true;
      return laghu_http_add_status(result, LAGHU_DECISION_PASS);
    }
    transaction->action = LAGHU_HTTP_ACTION_CAPTURE_IMAGE;
    result->capture_limit = LAGHU_IMAGE_MAX_INPUT_BYTES;
  }
  transaction->decision = LAGHU_DECISION_PASS;
  transaction->prepared = true;
  result->action = transaction->action;
  return laghu_http_add_status(result, LAGHU_DECISION_PASS);
}

static bool laghu_http_select_owned(laghu_http_transaction_result *result,
                                    const unsigned char *data, size_t length) {
  if (length == 0U) {
    return false;
  }
  result->owned_body = malloc(length);
  if (result->owned_body == NULL) {
    return false;
  }
  memcpy(result->owned_body, data, length);
  result->selected = (laghu_buffer){result->owned_body, length};
  return true;
}

static bool laghu_http_add_entity_headers(laghu_http_transaction_result *result,
                                          const char *prefix,
                                          const char *dependency_key) {
  char etag[LAGHU_RUNTIME_KEY_SIZE + 32U];
  if (dependency_key == NULL || dependency_key[0] == '\0') {
    return true;
  }
  (void)snprintf(etag, sizeof(etag), "\"%s%s\"", prefix, dependency_key);
  return laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "ETag",
                                         etag) &&
         laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_REMOVE,
                                         "Content-MD5", NULL) &&
         laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_REMOVE,
                                         "Digest", NULL);
}

static bool laghu_http_finalize_css(laghu_http_transaction *transaction,
                                    laghu_buffer body,
                                    laghu_http_transaction_result *result) {
  laghu_runtime_css_result rewritten;
  bool changed;
  bool ok = laghu_runtime_rewrite_css(
      transaction->environment.queue, transaction->environment.cache_path, body,
      transaction->path, transaction->origin, transaction->policy_key,
      transaction->capability_mask, transaction->environment.now,
      transaction->environment.config.image_metadata_ttl,
      (transaction->policy.filter_families & LAGHU_FILTER_CSS_MINIFY) != 0U,
      transaction->policy.allow_structural_rewrite,
      transaction->environment.config.css_inline_limit,
      transaction->environment.config.css_outline_threshold, &rewritten);
  if (!ok) {
    return false;
  }
  changed = rewritten.rewritten;
  if (changed &&
      !laghu_http_select_owned(result, rewritten.data, rewritten.length)) {
    laghu_runtime_css_result_release(&rewritten);
    return false;
  }
  if (changed) {
    memcpy(result->dependency_key, rewritten.dependency_key,
           sizeof(result->dependency_key));
  }
  laghu_runtime_css_result_release(&rewritten);
  return !changed || (laghu_http_add_length(result, result->selected.length) &&
                      laghu_http_add_entity_headers(result, "laghu-css-",
                                                    result->dependency_key));
}

static bool laghu_http_join_response_headers(
    const laghu_http_response *response, const char *name, char *output,
    size_t capacity) {
  size_t index;
  size_t length = 0U;
  output[0] = '\0';
  for (index = 0U; index < response->header_count; ++index) {
    const laghu_http_header *header = &response->headers[index];
    if (!laghu_http_header_name_equal(header->name, name)) {
      continue;
    }
    if (length != 0U) {
      if (length + 2U >= capacity) {
        return false;
      }
      output[length++] = ',';
      output[length++] = ' ';
    }
    if (header->value.length >= capacity - length) {
      return false;
    }
    memcpy(output + length, header->value.data, header->value.length);
    length += header->value.length;
    output[length] = '\0';
  }
  return true;
}

static bool laghu_http_finalize_html(laghu_http_transaction *transaction,
                                     laghu_buffer body,
                                     laghu_http_transaction_result *result) {
  laghu_runtime_html_result rewritten;
  laghu_runtime_html_result font = {0};
  laghu_runtime_html_result hinted;
  laghu_runtime_html_result finalized;
  const laghu_http_header *csp = laghu_http_find_header(
      transaction->response->headers, transaction->response->header_count,
      "Content-Security-Policy");
  char csp_value[LAGHU_HTTP_MAX_HEADER_VALUE + 1U];
  char language[LAGHU_HTTP_MAX_HEADER_VALUE + 1U];
  char links[LAGHU_HTTP_MAX_HEADER_VALUE + 1U];
  const unsigned char *selected = body.data;
  size_t selected_length = body.length;
  bool base_rewritten;
  bool header_changed = false;
  char base_dependency[LAGHU_RUNTIME_KEY_SIZE];
  char dependency[LAGHU_RUNTIME_KEY_SIZE];
  bool csp_data;
  bool csp_inline;
  bool csp_self;
  unsigned int index;
  if (!laghu_http_copy_header(csp, csp_value, sizeof(csp_value)) ||
      !laghu_http_join_response_headers(transaction->response,
                                        "Content-Language", language,
                                        sizeof(language)) ||
      !laghu_http_join_response_headers(transaction->response, "Link", links,
                                        sizeof(links))) {
    return false;
  }
  csp_data = csp == NULL || laghu_http_header_contains(csp, "data:");
  csp_inline =
      csp == NULL || laghu_http_header_contains(csp, "'unsafe-inline'");
  csp_self = laghu_runtime_csp_allows_self_styles(
      csp == NULL ? NULL : csp_value, transaction->origin);
  if (!laghu_runtime_rewrite_html(
          transaction->environment.cache_path, body, transaction->path,
          transaction->origin, transaction->policy_key,
          transaction->capability_mask, transaction->environment.now,
          transaction->environment.config.image_metadata_ttl,
          transaction->image_filters,
          transaction->policy.allow_resource_inlining,
          (transaction->policy.filter_families &
           LAGHU_FILTER_RESOURCE_INLINE) != 0U &&
              transaction->policy.allow_resource_inlining,
          transaction->policy.allow_structural_rewrite,
          (transaction->policy.filter_families & LAGHU_FILTER_CSS_MINIFY) !=
                  0U &&
              transaction->policy.allow_structural_rewrite,
          transaction->html_plan, csp_data, csp_inline, csp_self,
          transaction->environment.config.image_beacon == LAGHU_MODE_ON,
          transaction->environment.config.image_inline_limit,
          transaction->environment.config.css_inline_limit,
          transaction->environment.config.css_outline_threshold,
          transaction->viewport_width, transaction->dpr_hundredths,
          &rewritten)) {
    return false;
  }
  base_rewritten = rewritten.rewritten;
  memcpy(base_dependency, rewritten.dependency_key, sizeof(base_dependency));
  memcpy(dependency, base_dependency, sizeof(dependency));
  if (rewritten.rewritten) {
    selected = rewritten.data;
    selected_length = rewritten.length;
  }
  if (transaction->environment.font_providers != NULL) {
    if (!laghu_runtime_rewrite_font_css(
            transaction->environment.font_fetch_queue,
            transaction->environment.cache_path,
            transaction->environment.font_providers,
            (laghu_buffer){selected, selected_length},
            transaction->environment.now,
            (transaction->policy.filter_families &
             LAGHU_FILTER_RESOURCE_INLINE) != 0U &&
                transaction->policy.allow_resource_inlining,
            csp_inline, transaction->environment.config.css_inline_limit,
            &font)) {
      laghu_runtime_html_result_release(&rewritten);
      return false;
    }
    if (font.dependencies_pending) {
      laghu_runtime_html_result_release(&font);
      laghu_runtime_html_result_release(&rewritten);
      return true;
    }
    if (font.rewritten) {
      char material[LAGHU_RUNTIME_KEY_SIZE * 2U + 2U];
      int length;
      selected = font.data;
      selected_length = font.length;
      base_rewritten = true;
      length = snprintf(material, sizeof(material), "%s\n%s", base_dependency,
                        font.dependency_key);
      if (length > 0 && (size_t)length < sizeof(material))
        (void)laghu_sha256_hex(
            (laghu_buffer){(const unsigned char *)material, (size_t)length},
            base_dependency);
      memcpy(dependency, base_dependency, sizeof(dependency));
    }
  }
  if (!laghu_runtime_finalize_html_headers(
          transaction->environment.cache_path, body, transaction->path,
          transaction->origin, transaction->policy_key,
          transaction->capability_mask, transaction->environment.now,
          transaction->environment.config.image_metadata_ttl,
          transaction->html_plan & LAGHU_HTML_PLAN_RESOURCE_HINTS, language,
          links, transaction->environment.config.css_inline_limit,
          transaction->environment.config.css_outline_threshold, base_rewritten,
          &hinted) ||
      !laghu_runtime_finalize_html_headers(
          transaction->environment.cache_path,
          (laghu_buffer){selected, selected_length}, transaction->path,
          transaction->origin, transaction->policy_key,
          transaction->capability_mask, transaction->environment.now,
          transaction->environment.config.image_metadata_ttl,
          transaction->html_plan & ~LAGHU_HTML_PLAN_RESOURCE_HINTS, language,
          links, transaction->environment.config.css_inline_limit,
          transaction->environment.config.css_outline_threshold, base_rewritten,
          &finalized)) {
    laghu_runtime_html_result_release(&rewritten);
    laghu_runtime_html_result_release(&font);
    return false;
  }
  if (hinted.invalid || hinted.dependencies_pending || finalized.invalid ||
      finalized.dependencies_pending) {
    base_rewritten = false;
  } else {
    char material[LAGHU_RUNTIME_KEY_SIZE * 3U + 4U];
    int material_length;
    if (finalized.rewritten) {
      selected = finalized.data;
      selected_length = finalized.length;
      base_rewritten = true;
    }
    material_length =
        snprintf(material, sizeof(material), "%s\n%s\n%s", base_dependency,
                 hinted.dependency_key, finalized.dependency_key);
    if (material_length > 0 && (size_t)material_length < sizeof(material)) {
      (void)laghu_sha256_hex((laghu_buffer){(const unsigned char *)material,
                                            (size_t)material_length},
                             dependency);
    }
    if (finalized.set_content_language) {
      if (!laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET,
                                           "Content-Language",
                                           finalized.content_language)) {
        laghu_runtime_html_result_release(&hinted);
        laghu_runtime_html_result_release(&finalized);
        laghu_runtime_html_result_release(&rewritten);
        laghu_runtime_html_result_release(&font);
        return false;
      }
      header_changed = true;
    }
    for (index = 0U; index < hinted.link_header_count; ++index) {
      if (!laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_APPEND,
                                           "Link",
                                           hinted.link_headers[index])) {
        laghu_runtime_html_result_release(&hinted);
        laghu_runtime_html_result_release(&finalized);
        laghu_runtime_html_result_release(&rewritten);
        laghu_runtime_html_result_release(&font);
        return false;
      }
      header_changed = true;
    }
  }
  if (base_rewritten &&
      !laghu_http_select_owned(result, selected, selected_length)) {
    laghu_runtime_html_result_release(&hinted);
    laghu_runtime_html_result_release(&finalized);
    laghu_runtime_html_result_release(&rewritten);
    laghu_runtime_html_result_release(&font);
    return false;
  }
  laghu_runtime_html_result_release(&hinted);
  laghu_runtime_html_result_release(&finalized);
  laghu_runtime_html_result_release(&rewritten);
  laghu_runtime_html_result_release(&font);
  if (!base_rewritten && !header_changed) {
    return true;
  }
  memcpy(result->dependency_key, dependency, sizeof(result->dependency_key));
  return (!base_rewritten ||
          laghu_http_add_length(result, result->selected.length)) &&
         laghu_http_add_entity_headers(result, "laghu-html-", dependency);
}

static bool laghu_http_finalize_image(laghu_http_transaction *transaction,
                                      laghu_buffer body,
                                      laghu_http_transaction_result *result) {
  laghu_runtime_job job;
  memset(&job, 0, sizeof(job));
  memcpy(job.request_path, transaction->path, strlen(transaction->path) + 1U);
  memcpy(job.content_type, transaction->content_type,
         strlen(transaction->content_type) + 1U);
  memcpy(job.index_key, transaction->cache_key, sizeof(job.index_key));
  memcpy(job.validator, transaction->validator, sizeof(job.validator));
  memcpy(job.policy_key, transaction->policy_key, sizeof(job.policy_key));
  job.filters = transaction->image_filters;
  job.quality = transaction->policy.image_quality != 0U
                    ? transaction->policy.image_quality
                    : 100U;
  job.metadata_limit = transaction->environment.config.image_metadata_limit;
  job.metadata_ttl = transaction->environment.config.image_metadata_ttl;
  job.target_count = transaction->target_count;
  memcpy(job.target_width, transaction->target_width, sizeof(job.target_width));
  memcpy(job.target_height, transaction->target_height,
         sizeof(job.target_height));
  memcpy(job.resize_filter, transaction->resize_filter,
         sizeof(job.resize_filter));
  job.allow_lossy = transaction->policy.allow_lossy;
  job.accept_webp = transaction->accept_webp;
  job.payload = body;
  result->job_published =
      laghu_runtime_queue_try_publish(transaction->environment.queue, &job);
  memcpy(result->cache_key, transaction->cache_key, sizeof(result->cache_key));
  return true;
}

bool laghu_http_transaction_finalize(laghu_http_transaction *transaction,
                                     laghu_buffer captured_body,
                                     laghu_http_transaction_result *result) {
  bool ok = true;
  laghu_http_result_init(result);
  if (transaction == NULL || result == NULL || !transaction->prepared ||
      transaction->version != LAGHU_HTTP_ABI_VERSION ||
      transaction->struct_size != sizeof(*transaction) ||
      !laghu_http_view_valid(captured_body)) {
    return result != NULL &&
           laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) && false;
  }
  result->action = transaction->action;
  result->original = captured_body;
  result->selected = captured_body;
  memcpy(result->cache_key, transaction->cache_key, sizeof(result->cache_key));
  if (transaction->action == LAGHU_HTTP_ACTION_BYPASS ||
      transaction->action == LAGHU_HTTP_ACTION_SERVE_CACHED) {
    return laghu_http_add_status(result, transaction->decision);
  }
  if (captured_body.length == 0U ||
      (transaction->action == LAGHU_HTTP_ACTION_CAPTURE_CSS &&
       captured_body.length > LAGHU_CSS_MAX_INPUT_BYTES) ||
      (transaction->action != LAGHU_HTTP_ACTION_CAPTURE_CSS &&
       captured_body.length > LAGHU_IMAGE_MAX_INPUT_BYTES) ||
      (transaction->response->has_declared_length &&
       captured_body.length != transaction->response->declared_length)) {
    return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR);
  }
  if (transaction->action == LAGHU_HTTP_ACTION_CAPTURE_CSS) {
    ok = laghu_http_finalize_css(transaction, captured_body, result);
  } else if (transaction->action == LAGHU_HTTP_ACTION_CAPTURE_HTML) {
    ok = laghu_http_finalize_html(transaction, captured_body, result);
  } else if (transaction->action == LAGHU_HTTP_ACTION_CAPTURE_IMAGE) {
    ok = laghu_http_finalize_image(transaction, captured_body, result);
  }
  if (!ok) {
    laghu_http_transaction_result_release(result);
    result->original = captured_body;
    result->selected = captured_body;
    result->action = transaction->action;
    return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR);
  }
  return laghu_http_add_status(result, LAGHU_DECISION_PASS);
}
