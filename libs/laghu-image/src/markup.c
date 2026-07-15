// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/image.h"

typedef struct {
  unsigned char *data;
  size_t length;
  size_t capacity;
} laghu_markup_builder;

static bool laghu_builder_reserve(laghu_markup_builder *builder, size_t extra) {
  size_t required;
  size_t capacity;
  unsigned char *grown;

  if (extra > SIZE_MAX - builder->length - 1U) {
    return false;
  }
  required = builder->length + extra + 1U;
  if (required <= builder->capacity) {
    return true;
  }
  capacity = builder->capacity == 0U ? 256U : builder->capacity;
  while (capacity < required) {
    if (capacity > SIZE_MAX / 2U) {
      capacity = required;
      break;
    }
    capacity *= 2U;
  }
  grown = realloc(builder->data, capacity);
  if (grown == NULL) {
    return false;
  }
  builder->data = grown;
  builder->capacity = capacity;
  return true;
}

static bool laghu_builder_append(laghu_markup_builder *builder,
                                 const void *data, size_t length) {
  if (!laghu_builder_reserve(builder, length)) {
    return false;
  }
  if (length != 0U) {
    memcpy(builder->data + builder->length, data, length);
  }
  builder->length += length;
  builder->data[builder->length] = '\0';
  return true;
}

static bool laghu_builder_format(laghu_markup_builder *builder,
                                 const char *format, ...) {
  char stack[512];
  va_list arguments;
  int length;

  va_start(arguments, format);
  length = vsnprintf(stack, sizeof(stack), format, arguments);
  va_end(arguments);
  return length >= 0 && (size_t)length < sizeof(stack) &&
         laghu_builder_append(builder, stack, (size_t)length);
}

static bool laghu_builder_string(laghu_markup_builder *builder,
                                 const char *value) {
  const char *resolved = value != NULL ? value : "";
  return laghu_builder_append(builder, resolved, strlen(resolved)) &&
         laghu_builder_append(builder, "\n", 1U);
}

static bool laghu_markup_dependency_key(
    const laghu_image_markup_options *options,
    char output[LAGHU_SHA256_HEX_SIZE]) {
  laghu_markup_builder builder = {0};
  size_t index;
  bool success;

  for (index = 0U; index < options->resource_count; ++index) {
    const laghu_image_resource *resource = &options->resources[index];
    if (resource->source_url != NULL &&
        (resource->source_hash == NULL ||
         strlen(resource->source_hash) != LAGHU_SHA256_HEX_SIZE - 1U)) {
      free(builder.data);
      output[0] = '\0';
      return false;
    }
    if (!laghu_builder_string(&builder, resource->source_url) ||
        !laghu_builder_string(&builder, resource->source_hash) ||
        !laghu_builder_string(&builder, resource->optimized_url) ||
        !laghu_builder_string(&builder, resource->responsive_1x_url) ||
        !laghu_builder_string(&builder, resource->responsive_2x_url) ||
        !laghu_builder_string(&builder, resource->inline_data_uri) ||
        !laghu_builder_string(&builder, resource->preview_data_uri) ||
        !laghu_builder_string(&builder, resource->sprite_url) ||
        !laghu_builder_format(&builder, "%u:%u:%u:%u\n", resource->width,
                              resource->height, resource->sprite_x,
                              resource->sprite_y)) {
      free(builder.data);
      output[0] = '\0';
      return false;
    }
  }
  success =
      laghu_sha256_hex((laghu_buffer){builder.data, builder.length}, output);
  free(builder.data);
  return success;
}

static const unsigned char *laghu_find(const unsigned char *start,
                                       size_t length, const char *needle) {
  size_t needle_length = strlen(needle);
  size_t index;
  if (needle_length == 0U || needle_length > length) {
    return NULL;
  }
  for (index = 0U; index + needle_length <= length; ++index) {
    if (memcmp(start + index, needle, needle_length) == 0) {
      return start + index;
    }
  }
  return NULL;
}

static bool laghu_tag_has(const unsigned char *tag, size_t length,
                          const char *attribute) {
  return laghu_find(tag, length, attribute) != NULL;
}

static const laghu_image_resource *laghu_find_resource(
    const laghu_image_markup_options *options, const unsigned char *url,
    size_t url_length, size_t *resource_index) {
  size_t index;
  for (index = 0U; index < options->resource_count; ++index) {
    const char *source = options->resources[index].source_url;
    if (source != NULL && strlen(source) == url_length &&
        memcmp(source, url, url_length) == 0) {
      if (resource_index != NULL) {
        *resource_index = index;
      }
      return &options->resources[index];
    }
  }
  return NULL;
}

static bool laghu_rewrite_img_tag(laghu_markup_builder *builder,
                                  const unsigned char *tag, size_t tag_length,
                                  const laghu_image_markup_options *options,
                                  bool *seen, size_t image_index,
                                  laghu_image_filter_mask *applied) {
  const unsigned char *source = laghu_find(tag, tag_length, "src=");
  const laghu_image_resource *resource;
  const char *replacement;
  size_t resource_index;
  size_t prefix_length;
  size_t url_length;
  unsigned char quote;

  if (source == NULL || (size_t)(source - tag) + 5U >= tag_length) {
    return laghu_builder_append(builder, tag, tag_length);
  }
  quote = source[4];
  if (quote != '\'' && quote != '"') {
    return laghu_builder_append(builder, tag, tag_length);
  }
  source += 5;
  url_length = 0U;
  while ((size_t)(source - tag) + url_length < tag_length &&
         source[url_length] != quote) {
    ++url_length;
  }
  if ((size_t)(source - tag) + url_length >= tag_length) {
    return laghu_builder_append(builder, tag, tag_length);
  }
  resource = laghu_find_resource(options, source, url_length, &resource_index);
  if (resource == NULL || resource->optimized_url == NULL) {
    return laghu_builder_append(builder, tag, tag_length);
  }

  replacement = resource->optimized_url;
  if (options->inline_images && resource->inline_data_uri != NULL &&
      (!options->deduplicate_inline || !seen[resource_index])) {
    replacement = resource->inline_data_uri;
    *applied |= LAGHU_IMAGE_INLINE;
    seen[resource_index] = true;
  } else if (options->deduplicate_inline && seen[resource_index]) {
    *applied |= LAGHU_IMAGE_DEDUP_INLINE;
  }
  prefix_length = (size_t)(source - tag);
  if (!laghu_builder_append(builder, tag, prefix_length) ||
      !laghu_builder_append(builder, replacement, strlen(replacement)) ||
      !laghu_builder_append(builder, source + url_length,
                            tag_length - prefix_length - url_length)) {
    return false;
  }
  if (builder->length == 0U || builder->data[builder->length - 1U] != '>') {
    return false;
  }
  --builder->length;
  if (options->insert_dimensions && resource->width > 0U &&
      resource->height > 0U) {
    if (!laghu_tag_has(tag, tag_length, " width=") &&
        !laghu_builder_format(builder, " width=\"%u\"", resource->width)) {
      return false;
    }
    if (!laghu_tag_has(tag, tag_length, " height=") &&
        !laghu_builder_format(builder, " height=\"%u\"", resource->height)) {
      return false;
    }
    *applied |= LAGHU_IMAGE_INSERT_DIMENSIONS;
  }
  if (options->responsive && resource->responsive_1x_url != NULL &&
      resource->responsive_2x_url != NULL &&
      !laghu_tag_has(tag, tag_length, " srcset=")) {
    if (!laghu_builder_format(builder, " srcset=\"%s 1x, %s 2x\"",
                              resource->responsive_1x_url,
                              resource->responsive_2x_url)) {
      return false;
    }
    *applied |= LAGHU_IMAGE_RESPONSIVE;
    if (options->responsive_zoom) {
      *applied |= LAGHU_IMAGE_RESPONSIVE_ZOOM;
    }
  }
  if (options->lazyload && image_index > 0U &&
      !laghu_tag_has(tag, tag_length, " loading=")) {
    if (!laghu_builder_append(builder, " loading=\"lazy\"", 15U)) {
      return false;
    }
    *applied |= LAGHU_IMAGE_LAZYLOAD;
  }
  if (options->inline_previews && resource->preview_data_uri != NULL) {
    if (!laghu_builder_format(builder, " data-laghu-preview=\"%s\"",
                              resource->preview_data_uri)) {
      return false;
    }
    *applied |= LAGHU_IMAGE_INLINE_PREVIEW;
  }
  return laghu_builder_append(builder, ">", 1U);
}

bool laghu_image_rewrite_html(laghu_buffer input,
                              const laghu_image_markup_options *options,
                              laghu_image_markup_result *result) {
  laghu_markup_builder builder = {0};
  const unsigned char *cursor;
  const unsigned char *end;
  bool *seen;
  static const unsigned char empty[] = "";
  size_t image_index = 0U;

  if (result == NULL || options == NULL ||
      (input.data == NULL && input.length != 0U) ||
      (options->resources == NULL && options->resource_count != 0U)) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  seen = calloc(options->resource_count == 0U ? 1U : options->resource_count,
                sizeof(*seen));
  if (seen == NULL) {
    return false;
  }
  cursor = input.length == 0U ? empty : input.data;
  end = cursor + input.length;
  while (cursor < end) {
    const unsigned char *tag =
        laghu_find(cursor, (size_t)(end - cursor), "<img");
    const unsigned char *tag_end;
    if (tag == NULL) {
      if (!laghu_builder_append(&builder, cursor, (size_t)(end - cursor))) {
        goto failed;
      }
      break;
    }
    if (!laghu_builder_append(&builder, cursor, (size_t)(tag - cursor))) {
      goto failed;
    }
    tag_end = memchr(tag, '>', (size_t)(end - tag));
    if (tag_end == NULL ||
        !laghu_rewrite_img_tag(&builder, tag, (size_t)(tag_end - tag + 1U),
                               options, seen, image_index,
                               &result->applied_filters)) {
      goto failed;
    }
    ++image_index;
    cursor = tag_end + 1;
  }
  free(seen);
  result->data = builder.data;
  result->length = builder.length;
  if (!laghu_markup_dependency_key(options, result->dependency_key)) {
    goto failed_result;
  }
  return true;

failed:
  free(seen);
failed_result:
  free(builder.data);
  memset(result, 0, sizeof(*result));
  return false;
}

bool laghu_image_rewrite_css(laghu_buffer input,
                             const laghu_image_markup_options *options,
                             laghu_image_markup_result *result) {
  laghu_markup_builder builder = {0};
  const unsigned char *cursor;
  const unsigned char *end;
  static const unsigned char empty[] = "";

  if (result == NULL || options == NULL ||
      (input.data == NULL && input.length != 0U) ||
      (options->resources == NULL && options->resource_count != 0U)) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  cursor = input.length == 0U ? empty : input.data;
  end = cursor + input.length;
  while (cursor < end) {
    size_t index;
    const laghu_image_resource *match = NULL;
    const unsigned char *position = NULL;
    for (index = 0U; index < options->resource_count; ++index) {
      const char *source = options->resources[index].source_url;
      const unsigned char *found;
      if (source == NULL) {
        continue;
      }
      found = laghu_find(cursor, (size_t)(end - cursor), source);
      if (found != NULL && (position == NULL || found < position)) {
        position = found;
        match = &options->resources[index];
      }
    }
    if (match == NULL) {
      if (!laghu_builder_append(&builder, cursor, (size_t)(end - cursor))) {
        goto failed;
      }
      break;
    }
    if (!laghu_builder_append(&builder, cursor, (size_t)(position - cursor))) {
      goto failed;
    }
    if (options->sprites && match->sprite_url != NULL) {
      const unsigned char *source_end = position + strlen(match->source_url);
      const unsigned char *closing =
          memchr(source_end, ')', (size_t)(end - source_end));
      if (!laghu_builder_append(&builder, match->sprite_url,
                                strlen(match->sprite_url))) {
        goto failed;
      }
      if (closing != NULL) {
        if (!laghu_builder_append(&builder, source_end,
                                  (size_t)(closing - source_end + 1U)) ||
            !laghu_builder_format(&builder, ";background-position:-%upx -%upx",
                                  match->sprite_x, match->sprite_y)) {
          goto failed;
        }
        cursor = closing + 1;
      } else {
        cursor = source_end;
      }
      result->applied_filters |= LAGHU_IMAGE_SPRITE;
    } else if (match->optimized_url != NULL) {
      if (!laghu_builder_append(&builder, match->optimized_url,
                                strlen(match->optimized_url))) {
        goto failed;
      }
    } else if (!laghu_builder_append(&builder, match->source_url,
                                     strlen(match->source_url))) {
      goto failed;
    }
    if (!(options->sprites && match->sprite_url != NULL)) {
      cursor = position + strlen(match->source_url);
    }
  }
  result->data = builder.data;
  result->length = builder.length;
  if (!laghu_markup_dependency_key(options, result->dependency_key)) {
    goto failed_result;
  }
  return true;

failed:
failed_result:
  free(builder.data);
  memset(result, 0, sizeof(*result));
  return false;
}

void laghu_image_markup_result_release(laghu_image_markup_result *result) {
  if (result != NULL) {
    free(result->data);
    memset(result, 0, sizeof(*result));
  }
}

bool laghu_image_data_uri(laghu_image_format format, laghu_buffer input,
                          size_t max_input_bytes,
                          laghu_image_markup_result *result) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const char *content_type;
  size_t prefix_length;
  size_t encoded_length;
  size_t input_offset;
  size_t output_offset;

  if (result == NULL || format == LAGHU_IMAGE_FORMAT_UNKNOWN ||
      input.data == NULL || input.length == 0U ||
      input.length > max_input_bytes ||
      input.length > (SIZE_MAX - 2U) / 4U * 3U) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  content_type = laghu_image_content_type(format);
  prefix_length = strlen("data:") + strlen(content_type) + strlen(";base64,");
  encoded_length = ((input.length + 2U) / 3U) * 4U;
  if (encoded_length > SIZE_MAX - prefix_length - 1U) {
    return false;
  }
  result->data = malloc(prefix_length + encoded_length + 1U);
  if (result->data == NULL) {
    return false;
  }
  output_offset = (size_t)snprintf((char *)result->data,
                                   prefix_length + encoded_length + 1U,
                                   "data:%s;base64,", content_type);
  for (input_offset = 0U; input_offset < input.length; input_offset += 3U) {
    uint32_t value = (uint32_t)input.data[input_offset] << 16U;
    size_t remaining = input.length - input_offset;
    if (remaining > 1U) {
      value |= (uint32_t)input.data[input_offset + 1U] << 8U;
    }
    if (remaining > 2U) {
      value |= input.data[input_offset + 2U];
    }
    result->data[output_offset++] = alphabet[(value >> 18U) & 63U];
    result->data[output_offset++] = alphabet[(value >> 12U) & 63U];
    result->data[output_offset++] =
        remaining > 1U ? alphabet[(value >> 6U) & 63U] : '=';
    result->data[output_offset++] =
        remaining > 2U ? alphabet[value & 63U] : '=';
  }
  result->data[output_offset] = '\0';
  result->length = output_offset;
  result->applied_filters = LAGHU_IMAGE_INLINE;
  return true;
}

bool laghu_image_preview_data_uri(const laghu_image_backend *backend,
                                  const laghu_image_request *request,
                                  unsigned int max_dimension,
                                  laghu_image_markup_result *result) {
  laghu_image_request preview;
  laghu_image_result optimized;
  bool success;

  if (backend == NULL || request == NULL || result == NULL ||
      max_dimension == 0U) {
    return false;
  }
  preview = *request;
  preview.target_width = max_dimension;
  preview.target_height = max_dimension;
  preview.resize_filter = LAGHU_IMAGE_RESIZE_ATTRIBUTE;
  preview.filters |= LAGHU_IMAGE_RESIZE_ATTRIBUTE;
  if (preview.allow_lossy && preview.quality > 35U) {
    preview.quality = 35U;
  }
  if (!laghu_image_optimize(backend, &preview, &optimized) ||
      !optimized.used_candidate) {
    return false;
  }
  success = laghu_image_data_uri(optimized.output_format, optimized.selected,
                                 LAGHU_IMAGE_MAX_INPUT_BYTES, result);
  if (success) {
    result->applied_filters = LAGHU_IMAGE_INLINE_PREVIEW;
  }
  laghu_image_result_release(&optimized);
  return success;
}
