// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
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

static bool laghu_builder_append(laghu_markup_builder *builder, const void *data, size_t length);
static bool laghu_attribute(const unsigned char *tag, size_t length, const char *name, const unsigned char **value, size_t *value_length);

static bool laghu_svg_contains(laghu_buffer input, const char *needle) {
  size_t index, length = strlen(needle);
  if (length == 0U || length > input.length) return false;
  for (index = 0U; index + length <= input.length; ++index) {
    size_t letter;
    for (letter = 0U; letter < length; ++letter)
      if (tolower(input.data[index + letter]) != (unsigned char)needle[letter]) break;
    if (letter == length) return true;
  }
  return false;
}

static const unsigned char *laghu_svg_find(laghu_buffer input, size_t from, const char *needle, bool folded) {
  size_t index, length = strlen(needle);
  if (length == 0U || from > input.length || length > input.length - from) return NULL;
  for (index = from; index + length <= input.length; ++index) {
    size_t letter;
    for (letter = 0U; letter < length; ++letter) {
      unsigned char value = input.data[index + letter];
      if ((folded ? tolower(value) : value) != (unsigned char)needle[letter]) break;
    }
    if (letter == length) return input.data + index;
  }
  return NULL;
}

typedef struct {
  const unsigned char *name;
  size_t length;
} laghu_svg_element;

static bool laghu_svg_name_char(unsigned char value) { return isalnum(value) || value == ':' || value == '-' || value == '_'; }

static bool laghu_svg_editor_attribute(const unsigned char *name, size_t length) {
  static const char *const prefixes[] = {"inkscape:", "sodipodi:"};
  size_t prefix;
  for (prefix = 0U; prefix < sizeof(prefixes) / sizeof(prefixes[0]); ++prefix) {
    size_t index, prefix_length = strlen(prefixes[prefix]);
    if (length < prefix_length) continue;
    for (index = 0U; index < prefix_length; ++index)
      if (tolower(name[index]) != (unsigned char)prefixes[prefix][index]) break;
    if (index == prefix_length) return true;
  }
  return false;
}

static bool laghu_svg_append_tag(laghu_markup_builder *builder, laghu_buffer input, size_t *index) {
  size_t cursor = *index, end = cursor, copy = cursor;
  unsigned char quote = 0U;
  while (end < input.length) {
    if (quote != 0U) {
      if (input.data[end] == quote) quote = 0U;
    } else if (input.data[end] == '\'' || input.data[end] == '"') {
      quote = input.data[end];
    } else if (input.data[end] == '>') {
      break;
    }
    ++end;
  }
  if (end == input.length || quote != 0U) return false;
  cursor += 1U;
  if (cursor < end && input.data[cursor] == '/') ++cursor;
  while (cursor < end && isspace(input.data[cursor])) ++cursor;
  while (cursor < end && laghu_svg_name_char(input.data[cursor])) ++cursor;
  while (cursor < end) {
    size_t name_start, name_end, value_end;
    while (cursor < end && isspace(input.data[cursor])) ++cursor;
    if (cursor >= end || input.data[cursor] == '/') break;
    name_start = cursor;
    while (cursor < end && laghu_svg_name_char(input.data[cursor])) ++cursor;
    name_end = cursor;
    while (cursor < end && isspace(input.data[cursor])) ++cursor;
    if (cursor < end && input.data[cursor] == '=') {
      unsigned char value_quote;
      ++cursor;
      while (cursor < end && isspace(input.data[cursor])) ++cursor;
      if (cursor >= end || (input.data[cursor] != '\'' && input.data[cursor] != '"')) return false;
      value_quote = input.data[cursor++];
      while (cursor < end && input.data[cursor] != value_quote) ++cursor;
      if (cursor >= end) return false;
      ++cursor;
    }
    value_end = cursor;
    if (laghu_svg_editor_attribute(input.data + name_start, name_end - name_start)) {
      size_t leading = name_start;
      while (leading > *index && isspace(input.data[leading - 1U])) --leading;
      if (!laghu_builder_append(builder, input.data + copy, leading - copy)) return false;
      copy = value_end;
    }
  }
  if (!laghu_builder_append(builder, input.data + copy, end + 1U - copy)) return false;
  *index = end + 1U;
  return true;
}

static bool laghu_svg_well_formed(laghu_buffer input) {
  laghu_svg_element stack[64U];
  size_t depth = 0U, index = 0U;
  bool root = false;
  while (index < input.length) {
    size_t name_start, name_length, end;
    bool closing, self_closing = false;
    if (input.data[index] != '<') {
      ++index;
      continue;
    }
    if (index + 4U <= input.length && memcmp(input.data + index, "<!--", 4U) == 0) {
      const unsigned char *comment_end = laghu_svg_find(input, index + 4U, "-->", false);
      if (comment_end == NULL) return false;
      index = (size_t)(comment_end - input.data) + 3U;
      continue;
    }
    if (index + 2U >= input.length || input.data[index + 1U] == '!' || input.data[index + 1U] == '?') return false;
    closing = input.data[index + 1U] == '/';
    name_start = index + (closing ? 2U : 1U);
    end = name_start;
    while (end < input.length && laghu_svg_name_char(input.data[end])) ++end;
    name_length = end - name_start;
    if (name_length == 0U || end == input.length) return false;
    {
      unsigned char quote = 0U;
      for (; end < input.length; ++end) {
        unsigned char value = input.data[end];
        if (quote != 0U) {
          if (value == quote) quote = 0U;
        } else if (value == '\'' || value == '"') {
          quote = value;
        } else if (value == '<') {
          return false;
        } else if (value == '>') {
          break;
        }
      }
      if (quote != 0U || end == input.length) return false;
    }
    if (closing) {
      if (depth == 0U || stack[depth - 1U].length != name_length ||
          memcmp(stack[depth - 1U].name, input.data + name_start, name_length) != 0)
        return false;
      --depth;
    } else {
      size_t cursor = end;
      while (cursor > name_start && isspace(input.data[cursor - 1U])) --cursor;
      self_closing = cursor > name_start && input.data[cursor - 1U] == '/';
      if (!root) {
        if (name_length != 3U || memcmp(input.data + name_start, "svg", 3U) != 0) return false;
        root = true;
      }
      if (!self_closing) {
        if (depth == sizeof(stack) / sizeof(stack[0])) return false;
        stack[depth++] = (laghu_svg_element){input.data + name_start, name_length};
      }
    }
    index = end + 1U;
  }
  return root && depth == 0U;
}

bool laghu_image_optimize_svg(laghu_buffer input, laghu_image_markup_result *result) {
  laghu_markup_builder builder = {0};
  size_t index = 0U;
  bool root = false;
  if (result == NULL || input.data == NULL || input.length == 0U || input.length > LAGHU_IMAGE_MAX_INPUT_BYTES) return false;
  memset(result, 0, sizeof(*result));
  if (!laghu_svg_well_formed(input)) return false;
  if (laghu_svg_contains(input, "<script") || laghu_svg_contains(input, "<foreignobject") ||
      laghu_svg_contains(input, "<!doctype") || laghu_svg_contains(input, "<!entity") ||
      laghu_svg_contains(input, "javascript:") || laghu_svg_contains(input, "data:") || laghu_svg_contains(input, "url(") ||
      laghu_svg_contains(input, " onload=") || laghu_svg_contains(input, " onclick=") ||
      laghu_svg_contains(input, " onerror=") || laghu_svg_contains(input, " onbegin=") ||
      laghu_svg_contains(input, "xlink:href") || laghu_svg_contains(input, "href="))
    return false;
  while (index < input.length) {
    if (index + 4U <= input.length && memcmp(input.data + index, "<!--", 4U) == 0) {
      const unsigned char *end = laghu_svg_find(input, index + 4U, "-->", false);
      if (end == NULL) goto failed;
      index = (size_t)(end - input.data) + 3U;
      continue;
    }
    if (index + 9U <= input.length && laghu_svg_find(input, index, "<metadata", true) == input.data + index) {
      const unsigned char *end = laghu_svg_find(input, index + 9U, "</metadata>", true);
      if (end == NULL || (size_t)(end - input.data) >= input.length) goto failed;
      index = (size_t)(end - input.data) + 11U;
      continue;
    }
    if (input.data[index] == '<' && index + 4U <= input.length &&
        laghu_svg_find(input, index, "<svg", true) == input.data + index)
      root = true;
    if (input.data[index] == '<') {
      if (!laghu_svg_append_tag(&builder, input, &index)) goto failed;
      continue;
    }
    if (!isspace(input.data[index]) || (builder.length != 0U && !isspace(builder.data[builder.length - 1U]))) {
      if (!laghu_builder_append(&builder, input.data + index, 1U)) goto failed;
    }
    ++index;
  }
  if (!root || !laghu_svg_contains((laghu_buffer){builder.data, builder.length}, "</svg>") || builder.length >= input.length)
    goto failed;
  result->data = builder.data;
  result->length = builder.length;
  return true;
failed:
  free(builder.data);
  return false;
}

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

static bool laghu_builder_append(laghu_markup_builder *builder, const void *data, size_t length) {
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

static bool laghu_builder_format(laghu_markup_builder *builder, const char *format, ...) {
  char stack[512];
  va_list arguments;
  int length;

  va_start(arguments, format);
  length = vsnprintf(stack, sizeof(stack), format, arguments);
  va_end(arguments);
  return length >= 0 && (size_t)length < sizeof(stack) && laghu_builder_append(builder, stack, (size_t)length);
}

static bool laghu_builder_string(laghu_markup_builder *builder, const char *value) {
  const char *resolved = value != NULL ? value : "";
  return laghu_builder_append(builder, resolved, strlen(resolved)) && laghu_builder_append(builder, "\n", 1U);
}

static bool laghu_markup_dependency_key(const laghu_image_markup_options *options, char output[LAGHU_SHA256_HEX_SIZE]) {
  laghu_markup_builder builder = {0};
  size_t index;
  bool success;

  for (index = 0U; index < options->resource_count; ++index) {
    const laghu_image_resource *resource = &options->resources[index];
    if (resource->source_url != NULL && (resource->source_hash == NULL || strlen(resource->source_hash) != LAGHU_SHA256_HEX_SIZE - 1U)) {
      free(builder.data);
      output[0] = '\0';
      return false;
    }
    if (!laghu_builder_string(&builder, resource->source_url) || !laghu_builder_string(&builder, resource->source_hash) ||
        !laghu_builder_string(&builder, resource->optimized_url) || !laghu_builder_string(&builder, resource->responsive_1x_url) ||
        !laghu_builder_string(&builder, resource->responsive_2x_url) || !laghu_builder_string(&builder, resource->video_mp4_url) ||
        !laghu_builder_string(&builder, resource->video_webm_url) || !laghu_builder_string(&builder, resource->inline_data_uri) ||
        !laghu_builder_string(&builder, resource->preview_data_uri) || !laghu_builder_string(&builder, resource->sprite_url) ||
        !laghu_builder_format(&builder, "%u:%u:%u:%u\n", resource->width, resource->height, resource->sprite_x, resource->sprite_y)) {
      free(builder.data);
      output[0] = '\0';
      return false;
    }
  }
  success = laghu_sha256_hex((laghu_buffer){builder.data, builder.length}, output);
  free(builder.data);
  return success;
}

static const unsigned char *laghu_find(const unsigned char *start, size_t length, const char *needle) {
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

static const unsigned char *laghu_find_case(const unsigned char *start, size_t length, const char *needle) {
  size_t needle_length = strlen(needle);
  size_t index;
  size_t character;
  for (index = 0U; needle_length != 0U && index + needle_length <= length; ++index) {
    for (character = 0U; character < needle_length; ++character) {
      if (tolower(start[index + character]) != tolower((unsigned char)needle[character])) {
        break;
      }
    }
    if (character == needle_length) {
      return start + index;
    }
  }
  return NULL;
}

static bool laghu_tag_has(const unsigned char *tag, size_t length, const char *attribute) { return laghu_find_case(tag, length, attribute) != NULL; }

static const laghu_image_resource *laghu_find_resource(const laghu_image_markup_options *options, const unsigned char *url, size_t url_length,
                                                       size_t *resource_index) {
  size_t index;
  for (index = 0U; index < options->resource_count; ++index) {
    const char *source = options->resources[index].source_url;
    if (source != NULL && strlen(source) == url_length && memcmp(source, url, url_length) == 0) {
      if (resource_index != NULL) {
        *resource_index = index;
      }
      return &options->resources[index];
    }
  }
  return NULL;
}

static bool laghu_rewrite_img_tag(laghu_markup_builder *builder, const unsigned char *tag, size_t tag_length,
                                  const laghu_image_markup_options *options, bool *seen, size_t image_index, laghu_image_filter_mask *applied) {
  const unsigned char *source = laghu_find_case(tag, tag_length, "src=");
  const laghu_image_resource *resource;
  const char *replacement;
  size_t resource_index;
  size_t prefix_length;
  size_t url_length;
  unsigned char quote;

  if (source == NULL || (size_t)(source - tag) + 5U >= tag_length) {
    return laghu_builder_append(builder, tag, tag_length);
  }
  source += 4;
  while ((size_t)(source - tag) < tag_length && isspace(*source)) {
    ++source;
  }
  quote = (size_t)(source - tag) < tag_length ? *source : 0U;
  if (quote == '\'' || quote == '"') {
    ++source;
  } else {
    quote = 0U;
  }
  url_length = 0U;
  while ((size_t)(source - tag) + url_length < tag_length &&
         (quote != 0U ? source[url_length] != quote : !isspace(source[url_length]) && source[url_length] != '>')) {
    ++url_length;
  }
  if ((size_t)(source - tag) + url_length >= tag_length) {
    return laghu_builder_append(builder, tag, tag_length);
  }
  resource = laghu_find_resource(options, source, url_length, &resource_index);
  if (resource == NULL || (resource->optimized_url == NULL && !resource->video_ready)) {
    return laghu_builder_append(builder, tag, tag_length);
  }

  if (resource->video_ready && resource->video_mp4_url != NULL && resource->video_webm_url != NULL) {
    const unsigned char *alt = NULL;
    size_t alt_length = 0U;
    (void)laghu_attribute(tag, tag_length, "alt", &alt, &alt_length);
    if (!laghu_builder_format(builder, "<video autoplay muted loop playsinline preload=\"metadata\" width=\"%u\" height=\"%u\"><source src=\"%s\" type=\"video/webm\"><source src=\"%s\" type=\"video/mp4\"><img src=\"",
                              resource->declared_width != 0U ? resource->declared_width : resource->width,
                              resource->declared_height != 0U ? resource->declared_height : resource->height, resource->video_webm_url,
                              resource->video_mp4_url) ||
        !laghu_builder_append(builder, source, url_length) || !laghu_builder_append(builder, "\" alt=\"", 7U) ||
        (alt != NULL && !laghu_builder_append(builder, alt, alt_length)) || !laghu_builder_append(builder, "\"></video>", 11U))
      return false;
    *applied |= LAGHU_IMAGE_GIF_TO_VIDEO;
    return true;
  }

  replacement = resource->optimized_url;
  *applied |= LAGHU_IMAGE_REWRITE_IMAGES;
  if (options->inline_images && options->csp_allows_data_images && resource->inline_data_uri != NULL &&
      resource->inline_payload_length <= options->inline_limit && (!options->deduplicate_inline || !seen[resource_index])) {
    replacement = resource->inline_data_uri;
    *applied |= LAGHU_IMAGE_INLINE;
    seen[resource_index] = true;
  } else if (options->deduplicate_inline && seen[resource_index]) {
    *applied |= LAGHU_IMAGE_DEDUP_INLINE;
  }
  prefix_length = (size_t)(source - tag);
  if (!laghu_builder_append(builder, tag, prefix_length) || !laghu_builder_append(builder, replacement, strlen(replacement)) ||
      !laghu_builder_append(builder, source + url_length, tag_length - prefix_length - url_length)) {
    return false;
  }
  if (builder->length == 0U || builder->data[builder->length - 1U] != '>') {
    return false;
  }
  --builder->length;
  if (options->insert_dimensions && resource->width > 0U && resource->height > 0U) {
    unsigned int inserted_width = resource->width;
    unsigned int inserted_height = resource->height;
    if (resource->declared_width > 0U && resource->declared_height == 0U) {
      inserted_width = resource->declared_width;
      inserted_height = (unsigned int)(((uint64_t)resource->declared_width * resource->height + resource->width / 2U) / resource->width);
    } else if (resource->declared_height > 0U && resource->declared_width == 0U) {
      inserted_height = resource->declared_height;
      inserted_width = (unsigned int)(((uint64_t)resource->declared_height * resource->width + resource->height / 2U) / resource->height);
    }
    if (!laghu_tag_has(tag, tag_length, " width=") && !laghu_builder_format(builder, " width=\"%u\"", inserted_width)) {
      return false;
    }
    if (!laghu_tag_has(tag, tag_length, " height=") && !laghu_builder_format(builder, " height=\"%u\"", inserted_height)) {
      return false;
    }
    *applied |= LAGHU_IMAGE_INSERT_DIMENSIONS;
  }
  if (options->responsive && resource->responsive_1x_url != NULL && resource->responsive_2x_url != NULL && resource->responsive_1x_width > 0U &&
      resource->responsive_2x_width >= resource->responsive_1x_width && !laghu_tag_has(tag, tag_length, " srcset=")) {
    if (!laghu_builder_format(builder, " srcset=\"%s %uw, %s %uw\" sizes=\"%upx\"", resource->responsive_1x_url, resource->responsive_1x_width,
                              resource->responsive_2x_url, resource->responsive_2x_width,
                              resource->declared_width > 0U ? resource->declared_width : resource->responsive_1x_width)) {
      return false;
    }
    *applied |= LAGHU_IMAGE_RESPONSIVE;
    if (options->responsive_zoom) {
      *applied |= LAGHU_IMAGE_RESPONSIVE_ZOOM;
    }
  }
  if (options->lazyload && image_index > 0U && !resource->above_fold && !laghu_tag_has(tag, tag_length, "fetchpriority=\"high\"") &&
      !laghu_tag_has(tag, tag_length, "fetchpriority='high'") && !laghu_tag_has(tag, tag_length, " loading=")) {
    if (!laghu_builder_append(builder, " loading=\"lazy\"", 15U)) {
      return false;
    }
    *applied |= LAGHU_IMAGE_LAZYLOAD;
  }
  if (options->inline_previews && options->csp_allows_data_images && resource->preview_data_uri != NULL) {
    if (!laghu_builder_format(builder, " data-laghu-preview=\"%s\"", resource->preview_data_uri)) {
      return false;
    }
    *applied |= LAGHU_IMAGE_INLINE_PREVIEW;
  }
  return laghu_builder_append(builder, ">", 1U);
}

bool laghu_image_rewrite_html(laghu_buffer input, const laghu_image_markup_options *options, laghu_image_markup_result *result) {
  laghu_markup_builder builder = {0};
  const unsigned char *cursor;
  const unsigned char *end;
  bool *seen;
  static const unsigned char empty[] = "";
  size_t image_index = 0U;

  if (result == NULL || options == NULL || (input.data == NULL && input.length != 0U) ||
      (options->resources == NULL && options->resource_count != 0U)) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  seen = calloc(options->resource_count == 0U ? 1U : options->resource_count, sizeof(*seen));
  if (seen == NULL) {
    return false;
  }
  cursor = input.length == 0U ? empty : input.data;
  end = cursor + input.length;
  while (cursor < end) {
    const unsigned char *tag = laghu_find_case(cursor, (size_t)(end - cursor), "<img");
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
        !laghu_rewrite_img_tag(&builder, tag, (size_t)(tag_end - tag + 1U), options, seen, image_index, &result->applied_filters)) {
      goto failed;
    }
    ++image_index;
    cursor = tag_end + 1;
  }
  free(seen);
  result->data = builder.data;
  result->length = builder.length;
  if (options->enforce_bundle_gate && (result->applied_filters & LAGHU_IMAGE_GIF_TO_VIDEO) == 0U && result->length > input.length) {
    size_t growth = result->length - input.length;
    size_t savings = options->unique_variant_savings_1x;
    if (options->unique_variant_savings_2x < savings) {
      savings = options->unique_variant_savings_2x;
    }
    if (growth >= savings) {
      goto failed_result;
    }
  }
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

bool laghu_image_rewrite_css(laghu_buffer input, const laghu_image_markup_options *options, laghu_image_markup_result *result) {
  return laghu_css_minify_and_rewrite(input, "/", NULL, options, false, result);
}

void laghu_image_markup_result_release(laghu_image_markup_result *result) {
  if (result != NULL) {
    free(result->data);
    memset(result, 0, sizeof(*result));
  }
}

static bool laghu_ascii_equal(const unsigned char *value, size_t length, const char *expected) {
  size_t index;
  if (strlen(expected) != length) {
    return false;
  }
  for (index = 0U; index < length; ++index) {
    if (tolower(value[index]) != tolower((unsigned char)expected[index])) {
      return false;
    }
  }
  return true;
}

static bool laghu_attribute(const unsigned char *tag, size_t length, const char *name, const unsigned char **value, size_t *value_length) {
  size_t cursor = 1U;
  while (cursor < length) {
    size_t attribute_start;
    size_t value_start;
    size_t name_end;
    unsigned char quote = 0U;
    while (cursor < length && (isspace(tag[cursor]) || tag[cursor] == '/' || tag[cursor] == '>')) {
      ++cursor;
    }
    attribute_start = cursor;
    while (cursor < length && !isspace(tag[cursor]) && tag[cursor] != '=' && tag[cursor] != '>' && tag[cursor] != '/') {
      ++cursor;
    }
    name_end = cursor;
    while (cursor < length && isspace(tag[cursor])) {
      ++cursor;
    }
    if (cursor >= length || tag[cursor] != '=') {
      continue;
    }
    ++cursor;
    while (cursor < length && isspace(tag[cursor])) {
      ++cursor;
    }
    if (cursor < length && (tag[cursor] == '\'' || tag[cursor] == '"')) {
      quote = tag[cursor++];
    }
    value_start = cursor;
    while (cursor < length && (quote != 0U ? tag[cursor] != quote : !isspace(tag[cursor]) && tag[cursor] != '>')) {
      ++cursor;
    }
    if (laghu_ascii_equal(tag + attribute_start, name_end - attribute_start, name)) {
      *value = tag + value_start;
      *value_length = cursor - value_start;
      return true;
    }
    if (quote != 0U && cursor < length) {
      ++cursor;
    }
  }
  return false;
}

static unsigned int laghu_positive_integer(const unsigned char *value, size_t length) {
  uint64_t parsed = 0U;
  size_t index;
  if (length == 0U) {
    return 0U;
  }
  for (index = 0U; index < length; ++index) {
    if (value[index] < '0' || value[index] > '9') {
      return 0U;
    }
    parsed = parsed * 10U + (uint64_t)(value[index] - '0');
    if (parsed > LAGHU_IMAGE_MAX_DIMENSION) {
      return 0U;
    }
  }
  return (unsigned int)parsed;
}

static bool laghu_normalize_image_url(const unsigned char *url, size_t length, const char *page_path, const char *page_origin,
                                      char output[LAGHU_IMAGE_URL_SIZE]) {
  return laghu_base_url_resolve_same_origin(page_path, page_origin, (laghu_buffer){url, length}, LAGHU_URL_REJECT_API, output, LAGHU_IMAGE_URL_SIZE);
}

bool laghu_image_discover_html(laghu_buffer input, const char *page_path, const char *page_origin, laghu_image_discovery_result *result) {
  size_t cursor = 0U;
  if (result == NULL || (input.data == NULL && input.length != 0U)) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  while (cursor < input.length) {
    size_t end;
    size_t name_start;
    size_t name_end;
    const unsigned char *source;
    size_t source_length;
    laghu_image_discovery resource = {0};
    if (input.data[cursor] != '<') {
      ++cursor;
      continue;
    }
    if (cursor + 3U < input.length && memcmp(input.data + cursor, "<!--", 4U) == 0) {
      const unsigned char *close = laghu_find(input.data + cursor + 4U, input.length - cursor - 4U, "-->");
      cursor = close == NULL ? input.length : (size_t)(close - input.data) + 3U;
      continue;
    }
    end = cursor + 1U;
    while (end < input.length && input.data[end] != '>') {
      ++end;
    }
    if (end == input.length) {
      break;
    }
    name_start = cursor + 1U;
    while (name_start < end && isspace(input.data[name_start])) {
      ++name_start;
    }
    name_end = name_start;
    while (name_end < end && isalpha(input.data[name_end])) {
      ++name_end;
    }
    if (!laghu_ascii_equal(input.data + name_start, name_end - name_start, "img")) {
      cursor = end + 1U;
      continue;
    }
    if (!laghu_attribute(input.data + cursor, end - cursor + 1U, "src", &source, &source_length) ||
        !laghu_normalize_image_url(source, source_length, page_path, page_origin, resource.source_url)) {
      cursor = end + 1U;
      continue;
    }
    {
      size_t index;
      bool duplicate = false;
      for (index = 0U; index < result->resource_count; ++index) {
        if (strcmp(result->resources[index].source_url, resource.source_url) == 0) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) {
        cursor = end + 1U;
        continue;
      }
    }
    if (result->resource_count == LAGHU_IMAGE_MAX_PAGE_RESOURCES) {
      result->truncated = true;
      return true;
    }
    if (laghu_attribute(input.data + cursor, end - cursor + 1U, "width", &source, &source_length)) {
      resource.declared_width = laghu_positive_integer(source, source_length);
    }
    if (laghu_attribute(input.data + cursor, end - cursor + 1U, "height", &source, &source_length)) {
      resource.declared_height = laghu_positive_integer(source, source_length);
    }
    resource.has_loading = laghu_attribute(input.data + cursor, end - cursor + 1U, "loading", &source, &source_length);
    resource.has_srcset = laghu_attribute(input.data + cursor, end - cursor + 1U, "srcset", &source, &source_length);
    resource.has_sizes = laghu_attribute(input.data + cursor, end - cursor + 1U, "sizes", &source, &source_length);
    resource.fetchpriority_high = laghu_attribute(input.data + cursor, end - cursor + 1U, "fetchpriority", &source, &source_length) &&
                                  laghu_ascii_equal(source, source_length, "high");
    result->resources[result->resource_count++] = resource;
    cursor = end + 1U;
  }
  return true;
}

bool laghu_image_variant_url(const char *hash, char output[sizeof("/.laghu/image/") + LAGHU_SHA256_HEX_SIZE]) {
  size_t index;
  if (hash == NULL || output == NULL || strlen(hash) != LAGHU_SHA256_HEX_SIZE - 1U) {
    return false;
  }
  for (index = 0U; index < LAGHU_SHA256_HEX_SIZE - 1U; ++index) {
    if (!((hash[index] >= '0' && hash[index] <= '9') || (hash[index] >= 'a' && hash[index] <= 'f'))) {
      return false;
    }
  }
  (void)snprintf(output, sizeof("/.laghu/image/") + LAGHU_SHA256_HEX_SIZE, "/.laghu/image/%s", hash);
  return true;
}

bool laghu_image_plan_geometry(const laghu_image_geometry_input *input, laghu_image_geometry_plan *plan) {
  unsigned int width;
  uint64_t doubled;
  if (input == NULL || plan == NULL || input->natural_width == 0U || input->natural_height == 0U) {
    return false;
  }
  memset(plan, 0, sizeof(*plan));
  if (input->use_rendered_dimensions && input->learned_width > 0U) {
    width = input->learned_width;
  } else if (input->declared_width > 0U) {
    width = input->declared_width;
  } else {
    width = input->natural_width;
  }
  if (input->use_mobile_dimensions) {
    unsigned int mobile = input->learned_width > 0U ? input->learned_width : input->viewport_width;
    if (mobile > 0U && mobile < width) {
      width = mobile;
    }
  }
  if (width > input->natural_width) {
    width = input->natural_width;
  }
  plan->width[0] = width;
  plan->height[0] = (unsigned int)(((uint64_t)width * input->natural_height + input->natural_width / 2U) / input->natural_width);
  plan->count = 1U;
  doubled = (uint64_t)width * 2U;
  if (doubled > input->natural_width) {
    doubled = input->natural_width;
  }
  if ((unsigned int)doubled > width) {
    plan->width[1] = (unsigned int)doubled;
    plan->height[1] = (unsigned int)((doubled * input->natural_height + input->natural_width / 2U) / input->natural_width);
    plan->count = 2U;
  }
  return true;
}

bool laghu_image_data_uri(laghu_image_format format, laghu_buffer input, size_t max_input_bytes, laghu_image_markup_result *result) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const char *content_type;
  size_t prefix_length;
  size_t encoded_length;
  size_t input_offset;
  size_t output_offset;

  if (result == NULL || format == LAGHU_IMAGE_FORMAT_UNKNOWN || input.data == NULL || input.length == 0U || input.length > max_input_bytes ||
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
  output_offset = (size_t)snprintf((char *)result->data, prefix_length + encoded_length + 1U, "data:%s;base64,", content_type);
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
    result->data[output_offset++] = remaining > 1U ? alphabet[(value >> 6U) & 63U] : '=';
    result->data[output_offset++] = remaining > 2U ? alphabet[value & 63U] : '=';
  }
  result->data[output_offset] = '\0';
  result->length = output_offset;
  result->applied_filters = LAGHU_IMAGE_INLINE;
  return true;
}

bool laghu_image_preview_data_uri(const laghu_image_backend *backend, const laghu_image_request *request, unsigned int max_dimension,
                                  laghu_image_markup_result *result) {
  laghu_image_request preview;
  laghu_image_result optimized;
  bool success;

  if (backend == NULL || request == NULL || result == NULL || max_dimension == 0U) {
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
  if (!laghu_image_optimize(backend, &preview, &optimized) || !optimized.used_candidate) {
    return false;
  }
  success = laghu_image_data_uri(optimized.output_format, optimized.selected, LAGHU_IMAGE_MAX_INPUT_BYTES, result);
  if (success) {
    result->applied_filters = LAGHU_IMAGE_INLINE_PREVIEW;
  }
  laghu_image_result_release(&optimized);
  return success;
}
