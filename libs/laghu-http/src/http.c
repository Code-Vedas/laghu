// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_internal.h"
#include "laghu/assets.h"
#include "laghu/budget.h"
#include "laghu/cache.h"
#include "laghu/catalog.h"
#include "laghu/css.h"
#include "laghu/html.h"
#include "laghu/javascript.h"
#include "laghu/precompressed.h"
#include "laghu/queue.h"
#include "laghu/types.h"

static unsigned char laghu_http_ascii_lower(unsigned char value) {
  return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A')) : value;
}

bool laghu_http_view_valid(laghu_buffer value) { return value.length == 0U || value.data != NULL; }

static bool laghu_http_view_equal(laghu_buffer left, const char *right) {
  size_t index;
  size_t length = strlen(right);
  if (!laghu_http_view_valid(left) || left.length != length) {
    return false;
  }
  for (index = 0U; index < length; ++index) {
    if (laghu_http_ascii_lower(left.data[index]) != laghu_http_ascii_lower((unsigned char)right[index])) {
      return false;
    }
  }
  return true;
}

static bool laghu_http_copy_view(laghu_buffer value, char *output, size_t capacity) {
  if (!laghu_http_view_valid(value) || output == NULL || capacity == 0U || value.length >= capacity) {
    return false;
  }
  if (value.length != 0U) {
    memcpy(output, value.data, value.length);
  }
  output[value.length] = '\0';
  return true;
}

bool laghu_http_header_name_equal(laghu_buffer name, const char *expected) { return laghu_http_view_equal(name, expected); }

const laghu_http_header *laghu_http_find_header(const laghu_http_header *headers, size_t count, const char *name) {
  size_t index;
  for (index = 0U; index < count; ++index) {
    if (laghu_http_header_name_equal(headers[index].name, name)) {
      return &headers[index];
    }
  }
  return NULL;
}

static bool laghu_http_headers_valid(const laghu_http_header *headers, size_t count, size_t maximum) {
  size_t index;
  if (count > maximum || (count != 0U && headers == NULL)) {
    return false;
  }
  for (index = 0U; index < count; ++index) {
    size_t offset;
    if (!laghu_http_view_valid(headers[index].name) || !laghu_http_view_valid(headers[index].value) || headers[index].name.length == 0U ||
        headers[index].name.length > LAGHU_HTTP_MAX_HEADER_NAME || headers[index].value.length > LAGHU_HTTP_MAX_HEADER_VALUE) {
      return false;
    }
    for (offset = 0U; offset < headers[index].name.length; ++offset) {
      unsigned char value = headers[index].name.data[offset];
      if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '!' || value == '#' ||
            value == '$' || value == '%' || value == '&' || value == '\'' || value == '*' || value == '+' || value == '-' || value == '.' ||
            value == '^' || value == '_' || value == '`' || value == '|' || value == '~')) {
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
  if (!laghu_http_view_valid(value) || value.length == 0U || (path && value.data[0] != '/')) {
    return false;
  }
  for (index = 0U; index < value.length; ++index) {
    if (value.data[index] == '\0' || value.data[index] == '\r' || value.data[index] == '\n') {
      return false;
    }
  }
  return true;
}

static bool laghu_http_copy_header(const laghu_http_header *header, char *output, size_t capacity) {
  if (header == NULL) {
    if (output != NULL && capacity != 0U) {
      output[0] = '\0';
    }
    return true;
  }
  return laghu_http_copy_view(header->value, output, capacity);
}

static bool laghu_http_content_type_is(const char *content_type, const char *prefix) {
  size_t index;
  size_t length = strlen(prefix);
  if (content_type == NULL || strlen(content_type) < length) {
    return false;
  }
  for (index = 0U; index < length; ++index) {
    if (laghu_http_ascii_lower((unsigned char)content_type[index]) != laghu_http_ascii_lower((unsigned char)prefix[index])) {
      return false;
    }
  }
  return true;
}

void laghu_http_result_init(laghu_http_transaction_result *result) {
  if (result != NULL) {
    memset(result, 0, sizeof(*result));
    result->version = LAGHU_HTTP_ABI_VERSION;
    result->struct_size = sizeof(*result);
    result->action = LAGHU_HTTP_ACTION_BYPASS;
    result->decision = LAGHU_DECISION_BYPASS_ERROR;
  }
}

void laghu_http_transaction_result_release(laghu_http_transaction_result *result) {
  size_t index;
  if (result == NULL) {
    return;
  }
  free(result->owned_body);
  free(result->cache_owned_body);
  for (index = 0U; index < result->header_operation_count; ++index) {
    free(result->header_operations[index].value);
  }
  laghu_http_result_init(result);
}

bool laghu_http_add_header_operation(laghu_http_transaction_result *result, laghu_http_header_operation_kind kind, const char *name,
                                     const char *value) {
  laghu_http_header_operation *operation;
  size_t name_length;
  size_t value_length = value == NULL ? 0U : strlen(value);
  if (result == NULL || name == NULL || result->header_operation_count >= LAGHU_HTTP_MAX_HEADER_OPERATIONS) {
    return false;
  }
  name_length = strlen(name);
  if (name_length == 0U || name_length > LAGHU_HTTP_MAX_HEADER_NAME || value_length > LAGHU_HTTP_MAX_HEADER_VALUE ||
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

bool laghu_http_add_early_hint(laghu_http_transaction_result *result, const char *value) {
  laghu_http_header_operation *operation;
  if (!laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_APPEND, "Link", value)) {
    return false;
  }
  operation = &result->header_operations[result->header_operation_count - 1U];
  operation->early_hint = true;
  return true;
}

bool laghu_http_add_status(laghu_http_transaction_result *result, laghu_decision decision) {
  const char *cache = "bypass";
  const char *transform = "pass";
  result->decision = decision;
  if (decision == LAGHU_DECISION_IMAGE_HIT) {
    cache = "hit";
    transform = "optimized";
  } else if (decision == LAGHU_DECISION_PASS) {
    cache = "miss";
    transform = "queued";
  } else if (decision == LAGHU_DECISION_BYPASS_ERROR) {
    cache = "error";
    transform = "failed-open";
  }
  return laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "X-Laghu", laghu_decision_name(decision)) &&
         laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "X-Laghu-Cache", cache) &&
         laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "X-Laghu-Transform", transform);
}

static bool laghu_http_etag_equal(laghu_buffer candidate, const char *etag) {
  size_t etag_length;
  size_t offset = 0U;
  if (etag == NULL) return false;
  etag_length = strlen(etag);
  while (offset < candidate.length && (candidate.data[offset] == ' ' || candidate.data[offset] == '\t')) ++offset;
  if (offset + 2U <= candidate.length && (candidate.data[offset] == 'W' || candidate.data[offset] == 'w') && candidate.data[offset + 1U] == '/')
    offset += 2U;
  while (offset < candidate.length && (candidate.data[offset] == ' ' || candidate.data[offset] == '\t')) ++offset;
  return candidate.length - offset == etag_length && memcmp(candidate.data + offset, etag, etag_length) == 0;
}

bool laghu_http_request_matches_result_etag(const laghu_http_request *request, const laghu_http_transaction_result *result) {
  const char *etag = NULL;
  size_t index;
  if (request == NULL || result == NULL || !laghu_http_header_name_equal(request->method, "GET")) return false;
  for (index = 0U; index < result->header_operation_count; ++index) {
    const laghu_http_header_operation *operation = &result->header_operations[index];
    if (operation->kind != LAGHU_HTTP_HEADER_REMOVE && strcmp(operation->name, "ETag") == 0) {
      etag = operation->value;
    }
  }
  if (etag == NULL) return false;
  for (index = 0U; index < request->header_count; ++index) {
    laghu_buffer value;
    size_t offset = 0U;
    if (!laghu_http_header_name_equal(request->headers[index].name, "If-None-Match")) continue;
    value = request->headers[index].value;
    while (offset < value.length) {
      size_t start, end;
      while (offset < value.length && (value.data[offset] == ' ' || value.data[offset] == '\t' || value.data[offset] == ',')) ++offset;
      if (offset == value.length) break;
      if (value.data[offset] == '*') return true;
      start = offset;
      while (offset < value.length && value.data[offset] != ',') ++offset;
      end = offset;
      while (end > start && (value.data[end - 1U] == ' ' || value.data[end - 1U] == '\t')) --end;
      if (laghu_http_etag_equal((laghu_buffer){value.data + start, end - start}, etag)) return true;
    }
  }
  return false;
}

static laghu_image_filter_mask laghu_http_image_filters(const laghu_policy *policy) {
  laghu_image_filter_mask filters = LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_RECOMPRESS_IMAGES | LAGHU_IMAGE_RECOMPRESS_JPEG |
                                    LAGHU_IMAGE_RECOMPRESS_PNG | LAGHU_IMAGE_RECOMPRESS_WEBP;
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_METADATA) != 0U) {
    filters |= LAGHU_IMAGE_STRIP_METADATA | LAGHU_IMAGE_STRIP_COLOR_PROFILE;
  }
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_MODERN) != 0U) {
    filters |= LAGHU_IMAGE_JPEG_PROGRESSIVE | LAGHU_IMAGE_JPEG_TO_WEBP | LAGHU_IMAGE_PNG_TO_JPEG | LAGHU_IMAGE_GIF_TO_PNG |
               LAGHU_IMAGE_TO_WEBP_LOSSLESS | LAGHU_IMAGE_TO_WEBP_ANIMATED | LAGHU_IMAGE_JPEG_SAMPLING | LAGHU_IMAGE_IN_PLACE_BROWSER |
               LAGHU_IMAGE_GIF_TO_VIDEO;
  }
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_RESPONSIVE) != 0U) {
    filters |=
        LAGHU_IMAGE_RESIZE_ATTRIBUTE | LAGHU_IMAGE_RESIZE_RENDERED | LAGHU_IMAGE_RESIZE_MOBILE | LAGHU_IMAGE_RESPONSIVE | LAGHU_IMAGE_RESPONSIVE_ZOOM;
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

static bool laghu_http_backend_supports(const char *content_type, laghu_image_filter_mask filters, uint32_t capabilities, bool allow_lossy,
                                       bool accept_jxl) {
  if (laghu_http_content_type_is(content_type, "image/jpeg")) {
    return allow_lossy && (capabilities & LAGHU_IMAGE_CAP_JPEG_LOAD) != 0U &&
           (((capabilities & LAGHU_IMAGE_CAP_JPEG_SAVE) != 0U && (filters & (LAGHU_IMAGE_RECOMPRESS_IMAGES | LAGHU_IMAGE_RECOMPRESS_JPEG |
                                                                             LAGHU_IMAGE_JPEG_PROGRESSIVE | LAGHU_IMAGE_JPEG_SAMPLING)) != 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U && (filters & LAGHU_IMAGE_JPEG_TO_WEBP) != 0U) ||
            (accept_jxl && (capabilities & LAGHU_IMAGE_CAP_JXL_SAVE) != 0U));
  }
  if (laghu_http_content_type_is(content_type, "image/png")) {
    return (capabilities & LAGHU_IMAGE_CAP_PNG_LOAD) != 0U &&
           (((capabilities & LAGHU_IMAGE_CAP_PNG_SAVE) != 0U && (filters & (LAGHU_IMAGE_RECOMPRESS_IMAGES | LAGHU_IMAGE_RECOMPRESS_PNG)) != 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_JPEG_SAVE) != 0U && allow_lossy && (filters & LAGHU_IMAGE_PNG_TO_JPEG) != 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U && (filters & LAGHU_IMAGE_TO_WEBP_LOSSLESS) != 0U) ||
            (accept_jxl && allow_lossy && (capabilities & LAGHU_IMAGE_CAP_JXL_SAVE) != 0U));
  }
  if (laghu_http_content_type_is(content_type, "image/gif")) {
    return (capabilities & LAGHU_IMAGE_CAP_GIF_LOAD) != 0U &&
           (((capabilities & LAGHU_IMAGE_CAP_PNG_SAVE) != 0U && (filters & LAGHU_IMAGE_GIF_TO_PNG) != 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U && (filters & (LAGHU_IMAGE_TO_WEBP_LOSSLESS | LAGHU_IMAGE_TO_WEBP_ANIMATED)) != 0U) ||
            (filters & LAGHU_IMAGE_GIF_TO_VIDEO) != 0U);
  }
  return laghu_http_content_type_is(content_type, "image/webp") && (capabilities & LAGHU_IMAGE_CAP_WEBP_LOAD) != 0U &&
         (capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U;
}

static laghu_html_planner_mask laghu_http_html_plan(const laghu_policy *policy) {
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

static bool laghu_http_method_is(const laghu_http_request *request, const char *method) { return laghu_http_view_equal(request->method, method); }

static bool laghu_http_accepts_image(const laghu_http_request *request, const char *expected) {
  size_t header_index;
  for (header_index = 0U; header_index < request->header_count; ++header_index) {
    laghu_buffer value;
    size_t start = 0U;
    if (!laghu_http_header_name_equal(request->headers[header_index].name, "Accept")) {
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
      while (start < end && (value.data[start] == ' ' || value.data[start] == '\t')) {
        ++start;
      }
      type_end = start;
      while (type_end < end && value.data[type_end] != ';' && value.data[type_end] != ' ' && value.data[type_end] != '\t') {
        ++type_end;
      }
      if (type_end - start == strlen(expected) && laghu_http_view_equal((laghu_buffer){value.data + start, type_end - start}, expected)) {
        size_t parameter = type_end;
        while (parameter < end) {
          size_t quality;
          while (parameter < end && (value.data[parameter] == ';' || value.data[parameter] == ' ' || value.data[parameter] == '\t')) {
            ++parameter;
          }
          if (parameter + 1U < end && (value.data[parameter] == 'q' || value.data[parameter] == 'Q')) {
            quality = parameter + 1U;
            while (quality < end && (value.data[quality] == ' ' || value.data[quality] == '\t')) {
              ++quality;
            }
            if (quality < end && value.data[quality] == '=') {
              ++quality;
              while (quality < end && (value.data[quality] == ' ' || value.data[quality] == '\t')) {
                ++quality;
              }
              accepted = false;
              while (quality < end && value.data[quality] != ';' && value.data[quality] != ' ' && value.data[quality] != '\t') {
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

static bool laghu_http_contract_valid(const laghu_http_request *request, const laghu_http_response *response,
                                      const laghu_http_environment *environment) {
  return request != NULL && response != NULL && environment != NULL && request->version == LAGHU_HTTP_ABI_VERSION &&
         request->struct_size == sizeof(*request) && response->version == LAGHU_HTTP_ABI_VERSION && response->struct_size == sizeof(*response) &&
         environment->version == LAGHU_HTTP_ABI_VERSION && environment->struct_size == sizeof(*environment) &&
         laghu_http_normalized_view_valid(request->method, false) && laghu_http_normalized_view_valid(request->scheme, false) &&
         laghu_http_normalized_view_valid(request->authority, false) && laghu_http_normalized_view_valid(request->normalized_path, true) &&
         laghu_http_headers_valid(request->headers, request->header_count, LAGHU_HTTP_MAX_REQUEST_HEADERS) &&
         laghu_http_headers_valid(response->headers, response->header_count, LAGHU_HTTP_MAX_RESPONSE_HEADERS) &&
         laghu_http_view_valid(response->source_validator) && environment->cache_path != NULL;
}

static uint32_t laghu_http_refresh_backend(laghu_http_environment *environment) {
  return environment != NULL && environment->queue != NULL ? environment->queue_capabilities : 0U;
}

static unsigned int laghu_http_parse_uint_header(const laghu_http_request *request, const char *name, unsigned int maximum) {
  const laghu_http_header *header = laghu_http_find_header(request->headers, request->header_count, name);
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

static unsigned int laghu_http_parse_dpr(const laghu_http_request *request, const char *name) {
  const laghu_http_header *header = laghu_http_find_header(request->headers, request->header_count, name);
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

static uint32_t laghu_http_rollout_hash(const unsigned char *data, size_t length, uint32_t hash) {
  size_t index;
  for (index = 0U; index < length; ++index) {
    hash ^= (uint32_t)data[index];
    hash *= 16777619U;
  }
  return hash;
}

static unsigned int laghu_http_rollout_bucket(const laghu_http_request *request, unsigned int modulo) {
  const unsigned char *query;
  size_t index;
  size_t path_length;
  uint32_t hash;
  if (request == NULL || request->normalized_path.data == NULL || request->normalized_path.length == 0U || modulo == 0U) {
    return 0U;
  }
  hash = 2166136261U;
  hash = laghu_http_rollout_hash(request->method.data, request->method.length, hash);
  hash = laghu_http_rollout_hash(request->authority.data, request->authority.length, hash);
  path_length = request->normalized_path.length;
  query = request->normalized_path.data;
  for (index = 0U; index < path_length; ++index) {
    if (query[index] == '?') {
      break;
    }
    hash = laghu_http_rollout_hash(&query[index], 1U, hash);
  }
  hash ^= (uint32_t)modulo;
  hash *= 16777619U;
  return (unsigned int)(hash % modulo);
}

static bool laghu_http_apply_rollout(laghu_config *config, const laghu_http_request *request) {
  unsigned int bucket;
  if (config == NULL || request == NULL || config->rollout != LAGHU_MODE_ON) {
    return false;
  }
  if (config->rollout_percentage == LAGHU_ROLLOUT_PERCENTAGE_UNSET) {
    return false;
  }
  bucket = laghu_http_rollout_bucket(request, 100U);
  if (bucket >= config->rollout_percentage) {
    return false;
  }
  if (config->rollout_preset != LAGHU_PRESET_UNSET) {
    config->preset = config->rollout_preset;
    config->rewrite_level = LAGHU_REWRITE_LEVEL_UNSET;
  } else if (config->rollout_rewrite_level != LAGHU_REWRITE_LEVEL_UNSET) {
    config->rewrite_level = config->rollout_rewrite_level;
    config->preset = LAGHU_PRESET_UNSET;
  }
  return true;
}

static bool laghu_http_internal_asset_key(const char *path, char output[LAGHU_RUNTIME_KEY_SIZE]) {
  static const char image_prefix[] = "/.laghu/image/";
  static const char css_prefix[] = "/.laghu/css/";
  static const char javascript_prefix[] = "/.laghu/js/";
  static const char media_prefix[] = "/.laghu/media/";
  const char *key = NULL;
  bool javascript = false;
  size_t index;
  if (path == NULL) {
    return false;
  }
  if (strncmp(path, image_prefix, sizeof(image_prefix) - 1U) == 0) {
    key = path + sizeof(image_prefix) - 1U;
  } else if (strncmp(path, css_prefix, sizeof(css_prefix) - 1U) == 0) {
    key = path + sizeof(css_prefix) - 1U;
  } else if (strncmp(path, javascript_prefix, sizeof(javascript_prefix) - 1U) == 0) {
    key = path + sizeof(javascript_prefix) - 1U;
    javascript = true;
  } else if (strncmp(path, media_prefix, sizeof(media_prefix) - 1U) == 0) {
    key = path + sizeof(media_prefix) - 1U;
  }
  if (javascript && key != NULL && strlen(key) == LAGHU_SHA256_HEX_LENGTH + 4U && strcmp(key + LAGHU_SHA256_HEX_LENGTH, ".map") == 0) {
    /* The immutable key excludes the representational route suffix. */
  } else if (key == NULL || strlen(key) != LAGHU_SHA256_HEX_LENGTH) {
    return false;
  }
  for (index = 0U; index < LAGHU_SHA256_HEX_LENGTH; ++index) {
    if (!((key[index] >= '0' && key[index] <= '9') || (key[index] >= 'a' && key[index] <= 'f'))) {
      return false;
    }
  }
  memcpy(output, key, LAGHU_SHA256_HEX_LENGTH);
  output[LAGHU_SHA256_HEX_LENGTH] = '\0';
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

static bool laghu_http_set_origin(laghu_http_transaction *transaction, const laghu_http_request *request) {
  if (!laghu_http_copy_view(request->normalized_path, transaction->path, sizeof(transaction->path)) ||
      request->scheme.length + request->authority.length + 4U > sizeof(transaction->origin)) {
    return false;
  }
  if (request->scheme.length == 0U || request->authority.length == 0U) {
    transaction->origin[0] = '\0';
    return true;
  }
  memcpy(transaction->origin, request->scheme.data, request->scheme.length);
  memcpy(transaction->origin + request->scheme.length, "://", 3U);
  memcpy(transaction->origin + request->scheme.length + 3U, request->authority.data, request->authority.length);
  transaction->origin[request->scheme.length + request->authority.length + 3U] = '\0';
  return true;
}

static bool laghu_http_copy_cached_result(laghu_http_transaction_result *result, const laghu_runtime_cache_entry *entry) {
  result->cached_entry = *entry;
  result->cached_file = true;
  result->selected = (laghu_buffer){NULL, entry->length};
  result->action = LAGHU_HTTP_ACTION_SERVE_CACHED;
  return laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "Content-Type", entry->content_type);
}

bool laghu_http_add_length(laghu_http_transaction_result *result, size_t length) {
  char value[32U];
  int written = snprintf(value, sizeof(value), "%zu", length);
  return written > 0 && (size_t)written < sizeof(value) && laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "Content-Length", value);
}

static bool laghu_http_finish_cached_headers(const laghu_http_transaction *transaction, laghu_http_transaction_result *result,
                                             const laghu_runtime_cache_entry *entry) {
  char etag[LAGHU_RUNTIME_KEY_SIZE + 16U];
  const char *vary = "Accept";
  if (transaction->client_hint_variant) {
    if (transaction->sec_ch_viewport_width && transaction->sec_ch_dpr) {
      vary = "Accept, Sec-CH-DPR, Sec-CH-Viewport-Width";
    } else if (transaction->sec_ch_viewport_width) {
      vary = "Accept, Sec-CH-Viewport-Width";
    } else if (transaction->legacy_viewport_width && transaction->legacy_dpr) {
      vary = "Accept, DPR, Viewport-Width";
    } else if (transaction->legacy_viewport_width) {
      vary = "Accept, Viewport-Width";
    } else if (transaction->sec_ch_dpr) {
      vary = "Accept, Sec-CH-DPR";
    } else if (transaction->legacy_dpr) {
      vary = "Accept, DPR";
    }
  }
  (void)snprintf(etag, sizeof(etag), "\"laghu-%s\"", entry->payload_hash);
  return laghu_http_add_length(result, entry->length) && laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "Vary", vary) &&
         laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "ETag", etag) &&
         laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_REMOVE, "Content-MD5", NULL) &&
         laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_REMOVE, "Digest", NULL);
}

static bool laghu_http_apply_precompressed_cached(const laghu_http_transaction *transaction, laghu_http_transaction_result *result) {
  const laghu_http_header *accept_encoding;
  laghu_runtime_cache_entry entry;
  laghu_precompressed_coding coding;
  char accept_encoding_value[LAGHU_HTTP_MAX_HEADER_VALUE + 1U];
  char etag[LAGHU_RUNTIME_KEY_SIZE + 24U];
  if (transaction == NULL || result == NULL || !result->cached_file || result->selected.length < LAGHU_PRECOMPRESSED_MINIMUM ||
      !laghu_precompressed_text_type(transaction->content_type))
    return true;
  if (laghu_http_find_header(transaction->request->headers, transaction->request->header_count, "Range") != NULL) return true;
  accept_encoding = laghu_http_find_header(transaction->request->headers, transaction->request->header_count, "Accept-Encoding");
  if (accept_encoding != NULL && (accept_encoding->value.length > LAGHU_HTTP_MAX_HEADER_VALUE ||
                                  !laghu_http_copy_view(accept_encoding->value, accept_encoding_value, sizeof(accept_encoding_value))))
    return false;
  if (!laghu_precompressed_select_hash(transaction->environment.cache_path, result->cached_entry.payload_hash,
                                       accept_encoding == NULL ? NULL : accept_encoding_value, &entry, &coding)) {
    return laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "Vary", "Accept, Accept-Encoding");
  }
  (void)snprintf(etag, sizeof(etag), "\"laghu-%s-%s\"", laghu_precompressed_coding_name(coding), entry.payload_hash);
  if (!laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "Vary", "Accept, Accept-Encoding") ||
      !laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "Content-Encoding", laghu_precompressed_coding_name(coding)) ||
      !laghu_http_add_length(result, entry.length) || !laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "ETag", etag) ||
      !laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_REMOVE, "Content-MD5", NULL) ||
      !laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_REMOVE, "Digest", NULL)) {
    return false;
  }
  result->cached_entry = entry;
  result->selected.length = entry.length;
  return true;
}

bool laghu_http_transaction_prepare(laghu_http_transaction *transaction, const laghu_http_request *request, const laghu_http_response *response,
                                    const laghu_http_environment *environment, laghu_http_transaction_result *result) {
  const laghu_http_header *content_type;
  const laghu_http_header *cache_control;
  const laghu_http_header *encoding;
  const laghu_http_header *etag;
  const laghu_http_header *vary;
  laghu_response classification;
  laghu_runtime_cache_entry cache_entry;
  laghu_decision decision;
  bool image_class_known = false;
  laghu_image_content_class image_content_class = LAGHU_IMAGE_CONTENT_PHOTO;
  laghu_http_result_init(result);
  if (transaction == NULL || result == NULL || transaction->version != LAGHU_HTTP_ABI_VERSION || transaction->struct_size != sizeof(*transaction) ||
      !laghu_http_contract_valid(request, response, environment)) {
    return result != NULL && laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) && false;
  }
  laghu_http_transaction_init(transaction);
  transaction->request = request;
  transaction->response = response;
  transaction->environment = *environment;
  {
    const laghu_http_header *traceparent = laghu_http_find_header(request->headers, request->header_count, "traceparent");
    const laghu_http_header *tracestate = laghu_http_find_header(request->headers, request->header_count, "tracestate");
    char parent[56U] = "";
    char state[LAGHU_TRACE_STATE_SIZE] = "";
    if (traceparent != NULL && traceparent->value.length < sizeof(parent)) {
      memcpy(parent, traceparent->value.data, traceparent->value.length);
      if (tracestate != NULL && tracestate->value.length < sizeof(state)) memcpy(state, tracestate->value.data, tracestate->value.length);
    }
    if (!laghu_trace_context_parse(parent, state, &transaction->trace))
      (void)laghu_trace_context_root(environment->otel_sampling_rate, &transaction->trace);
    if (environment->otel_sampling_rate == 0U) {
      transaction->trace.sampled = false;
      memcpy(transaction->trace.flags, "00", 3U);
    }
  }
  (void)laghu_http_apply_rollout(&transaction->environment.config, request);
  const laghu_config *config = &transaction->environment.config;
  laghu_transform_budget_init(&transaction->budget, config->transform_memory_limit, config->transform_deadline_ms, config->variants_per_source);
  transaction->capability_mask = laghu_http_refresh_backend(&transaction->environment);
  transaction->viewport_width = laghu_http_parse_uint_header(request, "Sec-CH-Viewport-Width", 8192U);
  transaction->sec_ch_viewport_width = transaction->viewport_width != 0U;
  if (transaction->viewport_width == 0U) {
    transaction->viewport_width = laghu_http_parse_uint_header(request, "Viewport-Width", 8192U);
    transaction->legacy_viewport_width = transaction->viewport_width != 0U;
  }
  transaction->dpr_hundredths = laghu_http_parse_dpr(request, "Sec-CH-DPR");
  {
    const laghu_http_header *save_data = laghu_http_find_header(request->headers, request->header_count, "Save-Data");
    transaction->save_data = save_data != NULL && laghu_http_view_equal(save_data->value, "on");
  }
  transaction->sec_ch_dpr = laghu_http_find_header(request->headers, request->header_count, "Sec-CH-DPR") != NULL;
  if (!transaction->sec_ch_dpr) {
    transaction->dpr_hundredths = laghu_http_parse_dpr(request, "DPR");
    transaction->legacy_dpr = laghu_http_find_header(request->headers, request->header_count, "DPR") != NULL;
  }
  if (!laghu_http_set_origin(transaction, request)) {
    return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) && false;
  }
  transaction->cache_publishable = true;
  laghu_cache_source_scope(environment->cache_path, transaction->path);
  laghu_cache_variant_limit_scope(environment->cache_path, config->variants_per_source);
  if (laghu_http_internal_asset_key(transaction->path, transaction->cache_key) &&
      laghu_runtime_cache_lookup_variant(environment->cache_path, transaction->cache_key, &cache_entry)) {
    char asset_etag[LAGHU_RUNTIME_KEY_SIZE + 24U];
    (void)snprintf(asset_etag, sizeof(asset_etag), "\"%s\"", transaction->cache_key);
    if (strncmp(transaction->path, "/.laghu/css/", 12U) == 0) {
      memcpy(cache_entry.content_type, "text/css", sizeof("text/css"));
    } else if (strncmp(transaction->path, "/.laghu/js/", 11U) == 0) {
      if (strlen(transaction->path) > 4U && strcmp(transaction->path + strlen(transaction->path) - 4U, ".map") == 0)
        memcpy(cache_entry.content_type, "application/json", sizeof("application/json"));
      else
        memcpy(cache_entry.content_type, "application/javascript", sizeof("application/javascript"));
    }
    if (!laghu_http_copy_cached_result(result, &cache_entry) || !laghu_http_add_length(result, cache_entry.length) ||
        !laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "Cache-Control", "public, max-age=31536000, immutable") ||
        !laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "ETag", asset_etag) ||
        !laghu_http_apply_precompressed_cached(transaction, result) || !laghu_http_add_status(result, LAGHU_DECISION_IMAGE_HIT)) {
      laghu_http_transaction_result_release(result);
      return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) && false;
    }
    transaction->action = LAGHU_HTTP_ACTION_SERVE_CACHED;
    transaction->decision = LAGHU_DECISION_IMAGE_HIT;
    transaction->prepared = true;
    memcpy(result->cache_key, transaction->cache_key, sizeof(result->cache_key));
    return true;
  }
  {
    char source[LAGHU_RUNTIME_PATH_SIZE * 2U];
    int written = transaction->origin[0] == '\0' ? snprintf(source, sizeof(source), "%s", transaction->path)
                                                 : snprintf(source, sizeof(source), "%s%s", transaction->origin, transaction->path);
    if (written <= 0 || (size_t)written >= sizeof(source) || !laghu_resource_allowed(config, source)) {
      transaction->decision = LAGHU_DECISION_BYPASS_RESOURCE_POLICY;
      transaction->prepared = true;
      result->action = LAGHU_HTTP_ACTION_BYPASS;
      return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_RESOURCE_POLICY);
    }
  }
  content_type = laghu_http_find_header(response->headers, response->header_count, "Content-Type");
  cache_control = laghu_http_find_header(response->headers, response->header_count, "Cache-Control");
  encoding = laghu_http_find_header(response->headers, response->header_count, "Content-Encoding");
  etag = laghu_http_find_header(response->headers, response->header_count, "ETag");
  vary = laghu_http_find_header(response->headers, response->header_count, "Vary");
  if (!laghu_http_copy_header(content_type, transaction->content_type, sizeof(transaction->content_type)) ||
      !laghu_http_copy_header(etag, transaction->validator, sizeof(transaction->validator))) {
    return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) && false;
  }
  if ((transaction->validator[0] == 'W' || transaction->validator[0] == 'w') && transaction->validator[1] == '/') {
    transaction->validator[0] = '\0';
  }
  if (response->source_validator.length != 0U) {
    if (!laghu_http_copy_view(response->source_validator, transaction->validator, sizeof(transaction->validator)) ||
        ((transaction->validator[0] == 'W' || transaction->validator[0] == 'w') && transaction->validator[1] == '/')) {
      transaction->validator[0] = '\0';
    }
  }
  classification.status = response->status;
  classification.request_path = transaction->path;
  classification.content_type = content_type == NULL ? NULL : transaction->content_type;
  classification.cache_control = NULL;
  if (cache_control != NULL) {
    if (!laghu_http_join_response_headers(response, "Cache-Control", transaction->cache_control, sizeof(transaction->cache_control))) {
      return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) && false;
    }
    classification.cache_control = transaction->cache_control;
  }
  classification.has_authorization = laghu_http_find_header(request->headers, request->header_count, "Authorization") != NULL;
  decision = laghu_decide(config, &classification);
  if (decision == LAGHU_DECISION_PASS && (!response->complete || response->partial)) {
    decision = LAGHU_DECISION_BYPASS_STATUS;
  }
  if (decision == LAGHU_DECISION_PASS && encoding != NULL && encoding->value.length != 0U) {
    decision = LAGHU_DECISION_BYPASS_ENCODED;
  }
  if (decision == LAGHU_DECISION_PASS && vary != NULL) {
    char vary_value[LAGHU_HTTP_MAX_HEADER_VALUE + 1U];
    if (!laghu_http_join_response_headers(response, "Vary", vary_value, sizeof(vary_value))) {
      decision = LAGHU_DECISION_BYPASS_ERROR;
    } else if (!laghu_vary_supported(vary_value)) {
      transaction->cache_publishable = false;
      if (config->respect_vary != LAGHU_MODE_OFF) decision = LAGHU_DECISION_BYPASS_VARY;
    }
  }
  if (decision == LAGHU_DECISION_PASS && (!laghu_http_method_is(request, "GET") || laghu_http_method_is(request, "HEAD"))) {
    decision = LAGHU_DECISION_PASS;
    transaction->action = LAGHU_HTTP_ACTION_BYPASS;
  }
  if (decision != LAGHU_DECISION_PASS || !laghu_http_method_is(request, "GET")) {
    transaction->decision = decision;
    transaction->prepared = true;
    result->action = LAGHU_HTTP_ACTION_BYPASS;
    return laghu_http_add_status(result, decision);
  }
  {
    const char *query = strchr(transaction->path, '?');
    laghu_query_control control = LAGHU_QUERY_CONTROL_NONE;
    if (!laghu_apply_query_control(query, &control)) {
      transaction->decision = LAGHU_DECISION_BYPASS_QUERY_OVERRIDE;
      transaction->prepared = true;
      result->action = LAGHU_HTTP_ACTION_BYPASS;
      return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_QUERY_OVERRIDE);
    }
    if (control == LAGHU_QUERY_CONTROL_OFF) {
      transaction->decision = LAGHU_DECISION_BYPASS_QUERY_OFF;
      transaction->prepared = true;
      result->action = LAGHU_HTTP_ACTION_BYPASS;
      return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_QUERY_OFF);
    }
    if (control == LAGHU_QUERY_CONTROL_PREVIEW) {
      decision = LAGHU_DECISION_BYPASS_QUERY_PREVIEW;
      transaction->decision = decision;
      transaction->query_preview = true;
      result->decision = decision;
    }
    if (control == LAGHU_QUERY_CONTROL_EXPLAIN) {
      transaction->decision = LAGHU_DECISION_BYPASS_QUERY_EXPLAIN;
      transaction->prepared = true;
      result->action = LAGHU_HTTP_ACTION_BYPASS;
      return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_QUERY_EXPLAIN);
    }
    if (!laghu_apply_query_filter_overrides(config, query, &transaction->policy, NULL, NULL)) {
      transaction->decision = LAGHU_DECISION_BYPASS_QUERY_OVERRIDE;
      transaction->prepared = true;
      result->action = LAGHU_HTTP_ACTION_BYPASS;
      return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_QUERY_OVERRIDE);
    }
  }
  if (!laghu_variant_key((laghu_buffer){NULL, 0U}, &transaction->policy, transaction->policy_key)) {
    return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) && false;
  }
  if (!transaction->cache_publishable) {
    transaction->decision = LAGHU_DECISION_PASS;
    transaction->prepared = true;
    result->action = LAGHU_HTTP_ACTION_BYPASS;
    return laghu_http_add_status(result, LAGHU_DECISION_PASS);
  }
  transaction->image_filters = laghu_http_image_filters(&transaction->policy);
  transaction->html_plan = laghu_http_html_plan(&transaction->policy);
  transaction->accept_webp = laghu_http_accepts_image(request, "image/webp");
  transaction->accept_avif = laghu_http_accepts_image(request, "image/avif");
  transaction->accept_jxl = transaction->policy.allow_experimental && laghu_http_accepts_image(request, "image/jxl");
  {
    char asset_source[LAGHU_RUNTIME_PATH_SIZE];
    transaction->asset_allowed =
        transaction->cache_publishable && environment->asset_offload != NULL && response->has_declared_length &&
        snprintf(asset_source, sizeof(asset_source), "%s%s", environment->asset_offload->policy.source_domain, transaction->path) > 0 &&
        laghu_asset_source_allowed(&environment->asset_offload->policy, asset_source, transaction->content_type, response->declared_length);
  }
  if (laghu_http_content_type_is(transaction->content_type, "text/html")) {
    if (!response->has_declared_length || response->declared_length == 0U || response->declared_length > LAGHU_IMAGE_MAX_INPUT_BYTES) {
      transaction->decision = LAGHU_DECISION_PASS;
      transaction->prepared = true;
      return laghu_http_add_status(result, LAGHU_DECISION_PASS);
    }
    transaction->action = LAGHU_HTTP_ACTION_CAPTURE_HTML;
    result->capture_limit = LAGHU_IMAGE_MAX_INPUT_BYTES;
  } else if (laghu_http_content_type_is(transaction->content_type, "text/css")) {
    if (!response->has_declared_length || response->declared_length == 0U || response->declared_length > LAGHU_CSS_MAX_INPUT_BYTES ||
        ((transaction->policy.filter_families & LAGHU_FILTER_CSS_MINIFY) == 0U && !transaction->policy.allow_structural_rewrite &&
         !transaction->asset_allowed)) {
      transaction->decision = LAGHU_DECISION_PASS;
      transaction->prepared = true;
      return laghu_http_add_status(result, LAGHU_DECISION_PASS);
    }
    transaction->action = LAGHU_HTTP_ACTION_CAPTURE_CSS;
    result->capture_limit = LAGHU_CSS_MAX_INPUT_BYTES;
  } else if (laghu_http_content_type_is(transaction->content_type, "application/javascript") ||
             laghu_http_content_type_is(transaction->content_type, "text/javascript")) {
    if ((!response->has_declared_length || response->declared_length == 0U || response->declared_length > LAGHU_JAVASCRIPT_MAX_BYTES ||
         (transaction->policy.filter_families & LAGHU_FILTER_JAVASCRIPT_MINIFY) == 0U || environment->javascript_queue == NULL) &&
        !transaction->asset_allowed) {
      transaction->decision = LAGHU_DECISION_PASS;
      transaction->prepared = true;
      return laghu_http_add_status(result, LAGHU_DECISION_PASS);
    }
    transaction->action = LAGHU_HTTP_ACTION_CAPTURE_JAVASCRIPT;
    result->capture_limit = LAGHU_JAVASCRIPT_MAX_BYTES;
  } else if (laghu_http_content_type_is(transaction->content_type, "image/")) {
    {
      laghu_catalog_record catalog;
      if (laghu_catalog_lookup_url(environment->cache_path, transaction->path, transaction->policy_key, transaction->capability_mask,
                                   environment->now, config->image_metadata_ttl, &catalog)) {
        image_content_class = catalog.content_class;
        image_class_known = image_content_class <= LAGHU_IMAGE_CONTENT_FLAT_COLOR;
        uint64_t pixel_width = (uint64_t)transaction->viewport_width * transaction->dpr_hundredths;
        if ((transaction->image_filters & LAGHU_IMAGE_RESIZE_MOBILE) != 0U && transaction->viewport_width != 0U && catalog.natural_width != 0U &&
            catalog.natural_height != 0U) {
          unsigned int width = (unsigned int)((pixel_width + 99U) / 100U);
          if (width > catalog.natural_width) width = catalog.natural_width;
          if (width > 0U) {
            transaction->target_count = 1U;
            transaction->target_width[0] = width;
            transaction->target_height[0] =
                (unsigned int)(((uint64_t)width * catalog.natural_height + catalog.natural_width / 2U) / catalog.natural_width);
            transaction->resize_filter[0] = LAGHU_IMAGE_RESIZE_ATTRIBUTE;
            transaction->client_hint_variant = true;
          }
        } else {
          unsigned int index;
          for (index = 0U; index < catalog.variant_count && transaction->target_count < LAGHU_RUNTIME_MAX_TARGETS; ++index) {
            if (!catalog.variants[index].ready && !catalog.variants[index].terminally_excluded && catalog.variants[index].width > 0U) {
              unsigned int target = transaction->target_count++;
              transaction->target_width[target] = catalog.variants[index].width;
              transaction->target_height[target] = catalog.variants[index].height;
              transaction->resize_filter[target] = LAGHU_IMAGE_RESIZE_ATTRIBUTE;
            }
          }
        }
      }
    }
    if (!laghu_runtime_index_key_variant(transaction->path, transaction->validator, transaction->policy_key, transaction->accept_webp,
                                         transaction->accept_avif, transaction->accept_jxl,
                                         transaction->client_hint_variant ? transaction->target_width[0] : 0U,
                                         transaction->client_hint_variant ? transaction->target_height[0] : 0U,
                                         (unsigned int)laghu_image_viewport_bucket_for_width(transaction->viewport_width) |
                                             (transaction->save_data ? 4U : 0U), transaction->cache_key)) {
      return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) && false;
    }
    if (image_class_known && !laghu_runtime_index_key_content_class(transaction->cache_key, image_content_class, transaction->cache_key)) {
      return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) && false;
    }
    transaction->index_key_content_classified = image_class_known;
    memcpy(result->cache_key, transaction->cache_key, sizeof(result->cache_key));
    if (transaction->validator[0] != '\0' &&
        laghu_runtime_cache_lookup_readonly(environment->cache_path, transaction->cache_key, transaction->validator, &cache_entry) &&
        cache_entry.length != 0U && cache_entry.length <= LAGHU_IMAGE_MAX_INPUT_BYTES) {
      if (laghu_http_copy_cached_result(result, &cache_entry) && laghu_http_finish_cached_headers(transaction, result, &cache_entry) &&
          laghu_http_apply_precompressed_cached(transaction, result) && laghu_http_add_status(result, LAGHU_DECISION_IMAGE_HIT)) {
        transaction->action = LAGHU_HTTP_ACTION_SERVE_CACHED;
        transaction->decision = LAGHU_DECISION_IMAGE_HIT;
        transaction->prepared = true;
        return true;
      }
      {
        laghu_http_transaction_result_release(result);
      }
    }
    (void)laghu_cache_backend_associate_path(environment->cache_path, transaction->cache_key, transaction->path);
    if (laghu_http_content_type_is(transaction->content_type, "image/svg+xml")) {
      if (response->has_declared_length && response->declared_length > LAGHU_IMAGE_MAX_INPUT_BYTES) {
        transaction->decision = LAGHU_DECISION_PASS;
        transaction->prepared = true;
        return laghu_http_add_status(result, LAGHU_DECISION_PASS);
      }
      transaction->action = LAGHU_HTTP_ACTION_CAPTURE_IMAGE;
      result->capture_limit = LAGHU_IMAGE_MAX_INPUT_BYTES;
    } else if (transaction->image_filters == 0U || environment->queue == NULL ||
        !laghu_http_backend_supports(transaction->content_type, transaction->image_filters, transaction->capability_mask,
                                     transaction->policy.allow_lossy, transaction->accept_jxl)) {
      if (transaction->asset_allowed || ((transaction->policy.filter_families & (LAGHU_FILTER_CACHE_MEDIA | LAGHU_FILTER_CACHE_EXTENSION)) != 0U &&
                                         laghu_mime_type_allowed(transaction->policy.cache_mime_types, transaction->content_type))) {
        transaction->action = LAGHU_HTTP_ACTION_CAPTURE_RESOURCE;
        result->capture_limit = LAGHU_IMAGE_MAX_INPUT_BYTES;
        transaction->decision = LAGHU_DECISION_PASS;
        transaction->prepared = true;
        result->action = transaction->action;
        return laghu_http_add_status(result, LAGHU_DECISION_PASS);
      }
      transaction->decision = LAGHU_DECISION_BYPASS_IMAGE_BACKEND;
      transaction->prepared = true;
      return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_IMAGE_BACKEND);
    }
    if (response->has_declared_length && response->declared_length > LAGHU_IMAGE_MAX_INPUT_BYTES) {
      transaction->decision = LAGHU_DECISION_PASS;
      transaction->prepared = true;
      return laghu_http_add_status(result, LAGHU_DECISION_PASS);
    }
    transaction->action = LAGHU_HTTP_ACTION_CAPTURE_IMAGE;
    result->capture_limit = LAGHU_IMAGE_MAX_INPUT_BYTES;
  } else if (transaction->asset_allowed || ((transaction->policy.filter_families & (LAGHU_FILTER_CACHE_MEDIA | LAGHU_FILTER_CACHE_EXTENSION)) != 0U &&
                                            laghu_mime_type_allowed(transaction->policy.cache_mime_types, transaction->content_type))) {
    if (!laghu_runtime_index_key(transaction->path, transaction->validator, transaction->policy_key, false, false, false, 0U, 0U,
                                 transaction->cache_key)) {
      return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) && false;
    }
    (void)laghu_cache_backend_associate_path(environment->cache_path, transaction->cache_key, transaction->path);
    transaction->action = LAGHU_HTTP_ACTION_CAPTURE_RESOURCE;
    result->capture_limit = LAGHU_IMAGE_MAX_INPUT_BYTES;
  }
  if (transaction->query_preview) {
    transaction->decision = LAGHU_DECISION_BYPASS_QUERY_PREVIEW;
    transaction->prepared = true;
    result->action = transaction->action;
    return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_QUERY_PREVIEW);
  }
  transaction->decision = LAGHU_DECISION_PASS;
  transaction->prepared = true;
  result->action = transaction->action;
  return laghu_http_add_status(result, LAGHU_DECISION_PASS);
}
