// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/image.h"

typedef struct {
  unsigned char *data;
  size_t length;
  size_t capacity;
} laghu_css_builder;

typedef struct {
  size_t start;
  size_t end;
  size_t value_start;
  size_t value_end;
  unsigned char quote;
  char normalized[LAGHU_IMAGE_URL_SIZE];
  bool background_image;
  bool sprite_eligible;
} laghu_css_url;

typedef struct {
  laghu_css_url urls[LAGHU_CSS_MAX_URLS];
  laghu_css_import imports[LAGHU_CSS_MAX_IMPORTS];
  size_t url_count;
  size_t import_count;
  size_t token_count;
  bool valid;
  bool bounded;
  bool fallback_safe;
  bool has_imports;
  bool imports_supported;
  bool import_graph_forbidden;
} laghu_css_scan;

static bool laghu_css_reserve(laghu_css_builder *builder, size_t extra) {
  size_t needed;
  size_t capacity;
  unsigned char *grown;
  if (extra > SIZE_MAX - builder->length - 1U) {
    return false;
  }
  needed = builder->length + extra + 1U;
  if (needed <= builder->capacity) {
    return true;
  }
  capacity = builder->capacity == 0U ? 256U : builder->capacity;
  while (capacity < needed) {
    capacity = capacity > SIZE_MAX / 2U ? needed : capacity * 2U;
  }
  grown = realloc(builder->data, capacity);
  if (grown == NULL) {
    return false;
  }
  builder->data = grown;
  builder->capacity = capacity;
  return true;
}

static bool laghu_css_append(laghu_css_builder *builder, const void *data,
                             size_t length) {
  if (!laghu_css_reserve(builder, length)) {
    return false;
  }
  if (length != 0U) {
    memcpy(builder->data + builder->length, data, length);
  }
  builder->length += length;
  builder->data[builder->length] = '\0';
  return true;
}

static bool laghu_css_case_equal(const unsigned char *value, size_t length,
                                 const char *expected) {
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

static const unsigned char *laghu_css_case_find(const unsigned char *data,
                                                size_t length,
                                                const char *needle) {
  size_t needle_length = strlen(needle);
  size_t index;
  if (needle_length == 0U || needle_length > length) {
    return NULL;
  }
  for (index = 0U; index + needle_length <= length; ++index) {
    if (laghu_css_case_equal(data + index, needle_length, needle)) {
      return data + index;
    }
  }
  return NULL;
}

static bool laghu_css_normalize_url(const unsigned char *url, size_t length,
                                    const char *base_path,
                                    const char *page_origin,
                                    char output[LAGHU_IMAGE_URL_SIZE]) {
  const char *base;
  const char *slash;
  size_t prefix = 0U;
  size_t origin_length = page_origin == NULL ? 0U : strlen(page_origin);
  size_t index;
  while (length > 0U && isspace(url[0])) {
    ++url;
    --length;
  }
  while (length > 0U && isspace(url[length - 1U])) {
    --length;
  }
  if (length == 0U || length >= LAGHU_IMAGE_URL_SIZE || url[0] == '#' ||
      (length >= 2U && url[0] == '/' && url[1] == '/') ||
      (length >= 5U && laghu_css_case_equal(url, 5U, "data:")) ||
      (length >= 5U && laghu_css_case_equal(url, 5U, "blob:"))) {
    return false;
  }
  for (index = 0U; index + 2U < length; ++index) {
    if (url[index] == ':' && url[index + 1U] == '/' && url[index + 2U] == '/') {
      if (origin_length == 0U || length <= origin_length ||
          memcmp(url, page_origin, origin_length) != 0 ||
          url[origin_length] != '/') {
        return false;
      }
      url += origin_length;
      length -= origin_length;
      break;
    }
  }
  if (url[0] != '/') {
    base = base_path != NULL && base_path[0] == '/' ? base_path : "/";
    slash = strrchr(base, '/');
    prefix = slash == NULL ? 1U : (size_t)(slash - base + 1U);
    if (prefix + length >= LAGHU_IMAGE_URL_SIZE) {
      return false;
    }
    memcpy(output, base, prefix);
  }
  memcpy(output + prefix, url, length);
  output[prefix + length] = '\0';
  {
    char normalized[LAGHU_IMAGE_URL_SIZE];
    size_t segment_start[LAGHU_IMAGE_URL_SIZE / 2U];
    size_t segment_count = 0U;
    size_t source = 1U;
    size_t destination = 1U;
    normalized[0] = '/';
    while (output[source] != '\0') {
      size_t end = source;
      size_t segment_length;
      while (output[end] != '\0' && output[end] != '/') {
        ++end;
      }
      segment_length = end - source;
      if (segment_length == 2U && output[source] == '.' &&
          output[source + 1U] == '.') {
        if (segment_count == 0U) {
          return false;
        }
        destination = segment_start[--segment_count];
      } else if (!(segment_length == 0U ||
                   (segment_length == 1U && output[source] == '.'))) {
        if (segment_count >= sizeof(segment_start) / sizeof(segment_start[0]) ||
            destination + segment_length + 1U >= sizeof(normalized)) {
          return false;
        }
        segment_start[segment_count++] = destination;
        if (destination > 1U) {
          normalized[destination++] = '/';
        }
        memcpy(normalized + destination, output + source, segment_length);
        destination += segment_length;
      }
      source = output[end] == '/' ? end + 1U : end;
    }
    normalized[destination] = '\0';
    memcpy(output, normalized, destination + 1U);
  }
  if ((strncmp(output, "/api", 4U) == 0 &&
       (output[4] == '\0' || output[4] == '/')) ||
      (strncmp(output, "/graphql", 8U) == 0 &&
       (output[8] == '\0' || output[8] == '/'))) {
    return false;
  }
  return true;
}

static bool laghu_css_preserve_comment(const unsigned char *comment,
                                       size_t length) {
  return (length >= 3U && comment[2] == '!') ||
         laghu_css_case_find(comment, length, "sourceMappingURL") != NULL ||
         laghu_css_case_find(comment, length, "sourceURL") != NULL;
}

static bool laghu_css_block_sprite_eligible(const unsigned char *data,
                                            size_t length) {
  return laghu_css_case_find(data, length, "background-repeat") != NULL &&
         laghu_css_case_find(data, length, "no-repeat") != NULL &&
         laghu_css_case_find(data, length, "background-size") == NULL &&
         laghu_css_case_find(data, length, "background-position") == NULL &&
         laghu_css_case_find(data, length, "background:") == NULL;
}

static bool laghu_css_import_media_valid(const unsigned char *data,
                                         size_t length) {
  size_t index;
  if (length >= LAGHU_CSS_IMPORT_MEDIA_SIZE ||
      laghu_css_case_find(data, length, "layer") != NULL ||
      laghu_css_case_find(data, length, "supports") != NULL) {
    return false;
  }
  for (index = 0U; index < length; ++index) {
    unsigned char value = data[index];
    if (value == '{' || value == '}' || value == ';' || value == '"' ||
        value == '\'') {
      return false;
    }
  }
  return true;
}

static bool laghu_css_decode_import_url(
    const unsigned char *data, size_t length,
    unsigned char output[LAGHU_IMAGE_URL_SIZE], size_t *output_length) {
  size_t source = 0U;
  size_t destination = 0U;
  while (source < length) {
    unsigned char value = data[source++];
    if (value == '\\') {
      unsigned int decoded = 0U;
      unsigned int digits = 0U;
      if (source == length || data[source] == '\n' || data[source] == '\r' ||
          data[source] == '\f') {
        return false;
      }
      while (source < length && digits < 6U && isxdigit(data[source])) {
        unsigned char digit = data[source++];
        decoded = decoded * 16U +
                  (isdigit(digit) ? (unsigned int)(digit - (unsigned char)'0')
                                  : (unsigned int)(tolower(digit) - 'a' + 10));
        ++digits;
      }
      if (digits != 0U) {
        if (source < length && isspace(data[source])) {
          ++source;
        }
        if (decoded == 0U || decoded > 0x7fU) {
          return false;
        }
        value = (unsigned char)decoded;
      } else {
        value = data[source++];
      }
    }
    if (destination + 1U >= LAGHU_IMAGE_URL_SIZE) {
      return false;
    }
    output[destination++] = value;
  }
  output[destination] = '\0';
  *output_length = destination;
  return true;
}

static bool laghu_css_parse_import(laghu_buffer input, size_t start,
                                   const char *base_path,
                                   const char *page_origin,
                                   laghu_css_import *result, size_t *next) {
  size_t cursor = start + sizeof("@import") - 1U;
  size_t value_start;
  size_t value_end;
  size_t media_start;
  size_t media_end;
  unsigned char quote = 0U;
  unsigned char decoded_url[LAGHU_IMAGE_URL_SIZE];
  size_t decoded_length;
  unsigned int paren = 0U;
  if (cursor >= input.length ||
      (cursor < input.length &&
       (isalnum(input.data[cursor]) || input.data[cursor] == '-' ||
        input.data[cursor] == '_'))) {
    return false;
  }
  while (cursor < input.length && isspace(input.data[cursor])) {
    ++cursor;
  }
  if (cursor >= input.length) {
    return false;
  }
  if (input.data[cursor] == '\'' || input.data[cursor] == '"') {
    quote = input.data[cursor++];
    value_start = cursor;
    while (cursor < input.length && input.data[cursor] != quote) {
      if (input.data[cursor] == '\\') {
        cursor += cursor + 1U < input.length ? 2U : 1U;
        continue;
      }
      ++cursor;
    }
    if (cursor == input.length) {
      return false;
    }
    value_end = cursor++;
  } else if (cursor + 4U <= input.length &&
             laghu_css_case_equal(input.data + cursor, 3U, "url") &&
             input.data[cursor + 3U] == '(') {
    cursor += 4U;
    while (cursor < input.length && isspace(input.data[cursor])) {
      ++cursor;
    }
    if (cursor < input.length &&
        (input.data[cursor] == '\'' || input.data[cursor] == '"')) {
      quote = input.data[cursor++];
    }
    value_start = cursor;
    while (cursor < input.length &&
           ((quote != 0U && input.data[cursor] != quote) ||
            (quote == 0U && input.data[cursor] != ')'))) {
      if (input.data[cursor] == '\\') {
        cursor += cursor + 1U < input.length ? 2U : 1U;
        continue;
      }
      ++cursor;
    }
    if (cursor == input.length) {
      return false;
    }
    value_end = cursor;
    if (quote != 0U) {
      ++cursor;
      while (cursor < input.length && isspace(input.data[cursor])) {
        ++cursor;
      }
      if (cursor == input.length || input.data[cursor] != ')') {
        return false;
      }
    }
    ++cursor;
  } else {
    return false;
  }
  while (cursor < input.length && isspace(input.data[cursor])) {
    ++cursor;
  }
  media_start = cursor;
  while (cursor < input.length) {
    unsigned char value = input.data[cursor];
    if (value == '(') {
      ++paren;
    } else if (value == ')') {
      if (paren == 0U) {
        return false;
      }
      --paren;
    } else if (value == ';' && paren == 0U) {
      break;
    } else if (value == '{' || value == '}' || value == '\'' || value == '"') {
      return false;
    }
    ++cursor;
  }
  if (cursor == input.length || paren != 0U) {
    return false;
  }
  media_end = cursor;
  while (media_end > media_start && isspace(input.data[media_end - 1U])) {
    --media_end;
  }
  if (!laghu_css_import_media_valid(input.data + media_start,
                                    media_end - media_start) ||
      !laghu_css_decode_import_url(input.data + value_start,
                                   value_end - value_start, decoded_url,
                                   &decoded_length) ||
      !laghu_css_normalize_url(decoded_url, decoded_length, base_path,
                               page_origin, result->source_url)) {
    return false;
  }
  result->start = start;
  result->end = cursor + 1U;
  if (media_end > media_start &&
      !laghu_css_case_equal(input.data + media_start, media_end - media_start,
                            "all")) {
    memcpy(result->media, input.data + media_start, media_end - media_start);
    result->media[media_end - media_start] = '\0';
  }
  *next = cursor + 1U;
  return true;
}

static bool laghu_css_scan_input(laghu_buffer input, const char *base_path,
                                 const char *page_origin, bool declaration_list,
                                 laghu_css_scan *scan) {
  size_t cursor = 0U;
  unsigned int curly = declaration_list ? 1U : 0U;
  unsigned int paren = 0U;
  unsigned int square = 0U;
  size_t block_start = 0U;
  bool imports_allowed = !declaration_list;
  if (scan == NULL || input.length > LAGHU_CSS_MAX_INPUT_BYTES ||
      (input.data == NULL && input.length != 0U)) {
    return false;
  }
  memset(scan, 0, sizeof(*scan));
  scan->bounded = true;
  scan->fallback_safe = true;
  scan->imports_supported = true;
  while (cursor < input.length) {
    unsigned char current = input.data[cursor];
    if (++scan->token_count > LAGHU_CSS_MAX_TOKENS) {
      scan->bounded = false;
      return true;
    }
    if (current == '/' && cursor + 1U < input.length &&
        input.data[cursor + 1U] == '*') {
      const unsigned char *close = NULL;
      size_t index;
      for (index = cursor + 2U; index + 1U < input.length; ++index) {
        if (input.data[index] == '*' && input.data[index + 1U] == '/') {
          close = input.data + index;
          break;
        }
      }
      if (close == NULL) {
        scan->fallback_safe = false;
        return true;
      }
      if (laghu_css_preserve_comment(
              input.data + cursor,
              (size_t)(close - input.data) + 2U - cursor) &&
          (laghu_css_case_find(input.data + cursor,
                               (size_t)(close - input.data) + 2U - cursor,
                               "sourceMappingURL") != NULL ||
           laghu_css_case_find(input.data + cursor,
                               (size_t)(close - input.data) + 2U - cursor,
                               "sourceURL") != NULL)) {
        scan->import_graph_forbidden = true;
      }
      cursor = (size_t)(close - input.data) + 2U;
      continue;
    }
    if (!declaration_list && curly == 0U && paren == 0U && square == 0U &&
        current == '@' && cursor + sizeof("@import") - 1U <= input.length &&
        laghu_css_case_equal(input.data + cursor, sizeof("@import") - 1U,
                             "@import")) {
      size_t next = cursor;
      scan->has_imports = true;
      if (!imports_allowed || scan->import_count == LAGHU_CSS_MAX_IMPORTS ||
          !laghu_css_parse_import(input, cursor, base_path, page_origin,
                                  &scan->imports[scan->import_count], &next)) {
        scan->imports_supported = false;
      } else {
        ++scan->import_count;
        cursor = next;
        continue;
      }
    }
    if (!declaration_list && curly == 0U && paren == 0U && square == 0U &&
        current == '@') {
      static const char *forbidden[] = {"@charset", "@namespace", "@font-face"};
      size_t item;
      for (item = 0U; item < sizeof(forbidden) / sizeof(forbidden[0]); ++item) {
        size_t length = strlen(forbidden[item]);
        if (cursor + length <= input.length &&
            laghu_css_case_equal(input.data + cursor, length,
                                 forbidden[item])) {
          scan->import_graph_forbidden = true;
        }
      }
    }
    if (current == '\'' || current == '"') {
      unsigned char quote = current;
      bool closed = false;
      ++cursor;
      while (cursor < input.length) {
        if (input.data[cursor] == '\\') {
          cursor += cursor + 1U < input.length ? 2U : 1U;
        } else if (input.data[cursor++] == quote) {
          closed = true;
          break;
        }
      }
      if (!closed) {
        scan->fallback_safe = false;
        return true;
      }
      continue;
    }
    if (!isspace(current) && imports_allowed) {
      imports_allowed = false;
    }
    if (current == '{') {
      if (++curly > LAGHU_CSS_MAX_NESTING) {
        scan->bounded = false;
        return true;
      }
      block_start = cursor + 1U;
    } else if (current == '}') {
      if (declaration_list || curly == 0U) {
        return true;
      }
      --curly;
    } else if (current == '(') {
      if (++paren > LAGHU_CSS_MAX_NESTING) {
        scan->bounded = false;
        return true;
      }
    } else if (current == ')') {
      if (paren == 0U) {
        return true;
      }
      --paren;
    } else if (current == '[') {
      if (++square > LAGHU_CSS_MAX_NESTING) {
        scan->bounded = false;
        return true;
      }
    } else if (current == ']') {
      if (square == 0U) {
        return true;
      }
      --square;
    }
    if (cursor + 4U <= input.length &&
        laghu_css_case_equal(input.data + cursor, 3U, "url") &&
        input.data[cursor + 3U] == '(') {
      laghu_css_url *url;
      size_t value_start = cursor + 4U;
      size_t value_end;
      size_t end;
      unsigned char quote = 0U;
      while (value_start < input.length && isspace(input.data[value_start])) {
        ++value_start;
      }
      if (value_start < input.length &&
          (input.data[value_start] == '\'' || input.data[value_start] == '"')) {
        quote = input.data[value_start++];
      }
      value_end = value_start;
      while (value_end < input.length) {
        if (input.data[value_end] == '\\') {
          value_end += value_end + 1U < input.length ? 2U : 1U;
          continue;
        }
        if ((quote != 0U && input.data[value_end] == quote) ||
            (quote == 0U && input.data[value_end] == ')')) {
          break;
        }
        ++value_end;
      }
      if (value_end == input.length) {
        scan->fallback_safe = false;
        return true;
      }
      end = value_end + (quote != 0U ? 1U : 0U);
      while (end < input.length && isspace(input.data[end])) {
        ++end;
      }
      if (end >= input.length || input.data[end] != ')') {
        scan->fallback_safe = false;
        return true;
      }
      if (scan->url_count == LAGHU_CSS_MAX_URLS) {
        scan->bounded = false;
        return true;
      }
      url = &scan->urls[scan->url_count];
      if (laghu_css_normalize_url(input.data + value_start,
                                  value_end - value_start, base_path,
                                  page_origin, url->normalized)) {
        size_t property_start = cursor;
        size_t block_end = end;
        while (property_start > block_start &&
               input.data[property_start - 1U] != ';' &&
               input.data[property_start - 1U] != '{') {
          --property_start;
        }
        while (block_end < input.length && input.data[block_end] != '}') {
          ++block_end;
        }
        url->start = cursor;
        url->end = end + 1U;
        url->value_start = value_start;
        url->value_end = value_end;
        url->quote = quote;
        url->background_image = laghu_css_case_find(input.data + property_start,
                                                    cursor - property_start,
                                                    "background-image") != NULL;
        url->sprite_eligible =
            url->background_image && block_end < input.length &&
            laghu_css_block_sprite_eligible(input.data + block_start,
                                            block_end - block_start);
        ++scan->url_count;
      }
      cursor = end + 1U;
      continue;
    }
    ++cursor;
  }
  scan->valid =
      curly == (declaration_list ? 1U : 0U) && paren == 0U && square == 0U;
  return true;
}

static const laghu_image_resource *laghu_css_resource(
    const laghu_image_markup_options *options, const char *url) {
  size_t index;
  for (index = 0U; index < options->resource_count; ++index) {
    if (options->resources[index].source_url != NULL &&
        strcmp(options->resources[index].source_url, url) == 0) {
      return &options->resources[index];
    }
  }
  return NULL;
}

static bool laghu_css_word(unsigned char value) {
  return isalnum(value) || value == '_' || value == '-' || value >= 0x80U;
}

static bool laghu_css_emit_minified(laghu_buffer input,
                                    const laghu_css_scan *scan,
                                    const laghu_image_markup_options *options,
                                    bool minify, laghu_css_builder *builder,
                                    laghu_image_filter_mask *applied) {
  size_t cursor = 0U;
  size_t url_index = 0U;
  bool pending_space = false;
  while (cursor < input.length) {
    const laghu_css_url *url =
        url_index < scan->url_count ? &scan->urls[url_index] : NULL;
    if (minify && cursor + 2U < input.length && input.data[cursor] == '-' &&
        input.data[cursor + 1U] == '-') {
      size_t previous = cursor;
      size_t end = cursor;
      unsigned int depth = 0U;
      while (previous > 0U && isspace(input.data[previous - 1U])) {
        --previous;
      }
      if (previous == 0U || input.data[previous - 1U] == '{' ||
          input.data[previous - 1U] == ';') {
        while (end < input.length) {
          if (input.data[end] == '(') {
            ++depth;
          } else if (input.data[end] == ')' && depth > 0U) {
            --depth;
          } else if (input.data[end] == ';' && depth == 0U) {
            ++end;
            break;
          }
          ++end;
        }
        if (pending_space && !laghu_css_append(builder, " ", 1U)) {
          return false;
        }
        pending_space = false;
        if (!laghu_css_append(builder, input.data + cursor, end - cursor)) {
          return false;
        }
        while (url_index < scan->url_count &&
               scan->urls[url_index].start < end) {
          ++url_index;
        }
        cursor = end;
        continue;
      }
    }
    if (url != NULL && cursor == url->start) {
      const laghu_image_resource *resource =
          laghu_css_resource(options, url->normalized);
      const char *replacement =
          resource == NULL ? NULL : resource->optimized_url;
      if (options->sprites && resource != NULL &&
          resource->sprite_url != NULL && url->sprite_eligible) {
        replacement = resource->sprite_url;
      }
      if (pending_space && builder->length > 0U &&
          laghu_css_word(builder->data[builder->length - 1U])) {
        if (!laghu_css_append(builder, " ", 1U)) {
          return false;
        }
      }
      pending_space = false;
      if (!laghu_css_append(builder, "url(", 4U) ||
          !laghu_css_append(builder, "\"", 1U) ||
          !laghu_css_append(
              builder, replacement != NULL ? replacement : url->normalized,
              strlen(replacement != NULL ? replacement : url->normalized)) ||
          !laghu_css_append(builder, "\")", 2U)) {
        return false;
      }
      if (replacement != NULL) {
        *applied |= LAGHU_IMAGE_REWRITE_IMAGES;
      }
      if (options->sprites && resource != NULL &&
          resource->sprite_url != NULL && url->sprite_eligible) {
        if (!laghu_css_append(builder, ";background-position:", 21U)) {
          return false;
        }
        {
          char position[64];
          int length = snprintf(position, sizeof(position), "-%upx -%upx",
                                resource->sprite_x, resource->sprite_y);
          if (length <= 0 || (size_t)length >= sizeof(position) ||
              !laghu_css_append(builder, position, (size_t)length)) {
            return false;
          }
        }
        *applied |= LAGHU_IMAGE_SPRITE;
      }
      cursor = url->end;
      ++url_index;
      continue;
    }
    if (input.data[cursor] == '/' && cursor + 1U < input.length &&
        input.data[cursor + 1U] == '*') {
      size_t end = cursor + 2U;
      while (end + 1U < input.length &&
             !(input.data[end] == '*' && input.data[end + 1U] == '/')) {
        ++end;
      }
      end += 2U;
      if (!minify ||
          laghu_css_preserve_comment(input.data + cursor, end - cursor)) {
        if (!laghu_css_append(builder, input.data + cursor, end - cursor)) {
          return false;
        }
      } else {
        *applied |= LAGHU_CSS_MINIFY_APPLIED;
      }
      cursor = end;
      continue;
    }
    if (input.data[cursor] == '\'' || input.data[cursor] == '"') {
      unsigned char quote = input.data[cursor];
      size_t end = cursor + 1U;
      while (end < input.length) {
        if (input.data[end] == '\\') {
          end += end + 1U < input.length ? 2U : 1U;
        } else if (input.data[end++] == quote) {
          break;
        }
      }
      if (pending_space && builder->length > 0U &&
          laghu_css_word(builder->data[builder->length - 1U]) &&
          !laghu_css_append(builder, " ", 1U)) {
        return false;
      }
      pending_space = false;
      if (!laghu_css_append(builder, input.data + cursor, end - cursor)) {
        return false;
      }
      cursor = end;
      continue;
    }
    if (minify && isspace(input.data[cursor])) {
      pending_space = true;
      ++cursor;
      *applied |= LAGHU_CSS_MINIFY_APPLIED;
      continue;
    }
    if (pending_space) {
      static const char punctuation[] = "{}:;,>+~()[]";
      unsigned char previous =
          builder->length == 0U ? 0U : builder->data[builder->length - 1U];
      if (strchr(punctuation, input.data[cursor]) == NULL &&
          strchr(punctuation, previous) == NULL &&
          (laghu_css_word(previous) || laghu_css_word(input.data[cursor])) &&
          !laghu_css_append(builder, " ", 1U)) {
        return false;
      }
      pending_space = false;
    }
    if (!laghu_css_append(builder, input.data + cursor, 1U)) {
      return false;
    }
    ++cursor;
  }
  return true;
}

bool laghu_css_discover(laghu_buffer input, const char *base_path,
                        const char *page_origin,
                        laghu_css_parse_result *result) {
  laghu_css_scan *scan = NULL;
  size_t index;
  if (result == NULL || (scan = calloc(1U, sizeof(*scan))) == NULL ||
      !laghu_css_scan_input(input, base_path, page_origin, false, scan)) {
    free(scan);
    return false;
  }
  memset(result, 0, sizeof(*result));
  result->valid = scan->valid;
  result->bounded = scan->bounded;
  result->token_count = scan->token_count;
  result->has_imports = scan->has_imports;
  result->imports_supported = scan->imports_supported;
  result->import_graph_forbidden = scan->import_graph_forbidden;
  result->import_count = scan->import_count;
  memcpy(result->imports, scan->imports,
         scan->import_count * sizeof(result->imports[0]));
  for (index = 0U; index < scan->url_count; ++index) {
    size_t existing;
    bool duplicate = false;
    for (existing = 0U; existing < result->dependency_count; ++existing) {
      if (strcmp(result->dependencies[existing].source_url,
                 scan->urls[index].normalized) == 0) {
        result->dependencies[existing].sprite_eligible |=
            scan->urls[index].sprite_eligible;
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      laghu_css_dependency *dependency =
          &result->dependencies[result->dependency_count++];
      memcpy(dependency->source_url, scan->urls[index].normalized,
             sizeof(dependency->source_url));
      dependency->background_image = scan->urls[index].background_image;
      dependency->sprite_eligible = scan->urls[index].sprite_eligible;
    }
  }
  free(scan);
  return true;
}

bool laghu_css_discover_style_attributes(laghu_buffer html,
                                         const char *page_path,
                                         const char *page_origin,
                                         laghu_css_parse_result *result) {
  size_t cursor = 0U;
  if (result == NULL || (html.data == NULL && html.length != 0U)) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  result->valid = true;
  result->bounded = true;
  while (cursor < html.length) {
    const unsigned char *attribute =
        laghu_css_case_find(html.data + cursor, html.length - cursor, "style=");
    size_t value_start;
    size_t value_end;
    unsigned char quote;
    laghu_css_scan *scan = calloc(1U, sizeof(*scan));
    size_t index;
    if (scan == NULL) {
      return false;
    }
    if (attribute == NULL) {
      free(scan);
      break;
    }
    value_start = (size_t)(attribute - html.data) + 6U;
    if (value_start >= html.length) {
      free(scan);
      result->valid = false;
      return true;
    }
    quote = (html.data[value_start] == '\'' || html.data[value_start] == '"')
                ? html.data[value_start++]
                : 0U;
    value_end = value_start;
    while (value_end < html.length &&
           ((quote != 0U && html.data[value_end] != quote) ||
            (quote == 0U && !isspace(html.data[value_end]) &&
             html.data[value_end] != '>'))) {
      ++value_end;
    }
    if ((quote != 0U && value_end == html.length) ||
        !laghu_css_scan_input(
            (laghu_buffer){html.data + value_start, value_end - value_start},
            page_path, page_origin, true, scan)) {
      free(scan);
      result->valid = false;
      return true;
    }
    result->valid &= scan->valid;
    result->bounded &= scan->bounded;
    if (scan->token_count > LAGHU_CSS_MAX_TOKENS - result->token_count) {
      free(scan);
      result->bounded = false;
      return true;
    }
    result->token_count += scan->token_count;
    for (index = 0U; index < scan->url_count; ++index) {
      size_t existing;
      bool duplicate = false;
      for (existing = 0U; existing < result->dependency_count; ++existing) {
        if (strcmp(result->dependencies[existing].source_url,
                   scan->urls[index].normalized) == 0) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        laghu_css_dependency *dependency;
        if (result->dependency_count == LAGHU_CSS_MAX_URLS) {
          free(scan);
          result->bounded = false;
          return true;
        }
        dependency = &result->dependencies[result->dependency_count++];
        memcpy(dependency->source_url, scan->urls[index].normalized,
               sizeof(dependency->source_url));
        dependency->background_image = scan->urls[index].background_image;
      }
    }
    free(scan);
    cursor = value_end + (quote != 0U ? 1U : 0U);
  }
  return true;
}

bool laghu_css_minify_and_rewrite(laghu_buffer input, const char *base_path,
                                  const char *page_origin,
                                  const laghu_image_markup_options *options,
                                  bool declaration_list,
                                  laghu_image_markup_result *result) {
  laghu_css_scan *scan = calloc(1U, sizeof(*scan));
  laghu_css_builder builder = {0};
  if (result == NULL || options == NULL || scan == NULL ||
      !laghu_css_scan_input(input, base_path, page_origin, declaration_list,
                            scan)) {
    free(scan);
    return false;
  }
  memset(result, 0, sizeof(*result));
  if (!scan->valid || !scan->bounded) {
    free(scan);
    return false;
  }
  if (!laghu_css_emit_minified(input, scan, options, true, &builder,
                               &result->applied_filters)) {
    free(builder.data);
    free(scan);
    return false;
  }
  free(scan);
  result->data = builder.data;
  result->length = builder.length;
  return laghu_sha256_hex((laghu_buffer){result->data, result->length},
                          result->dependency_key);
}

bool laghu_css_fallback_rewrite_urls(laghu_buffer input, const char *base_path,
                                     const char *page_origin,
                                     const laghu_image_markup_options *options,
                                     laghu_image_markup_result *result) {
  laghu_css_scan *scan = calloc(1U, sizeof(*scan));
  laghu_css_builder builder = {0};
  if (result == NULL || options == NULL || scan == NULL ||
      !laghu_css_scan_input(input, base_path, page_origin, false, scan) ||
      !scan->bounded || !scan->fallback_safe || scan->url_count == 0U) {
    free(scan);
    return false;
  }
  memset(result, 0, sizeof(*result));
  if (!laghu_css_emit_minified(input, scan, options, false, &builder,
                               &result->applied_filters)) {
    free(builder.data);
    free(scan);
    return false;
  }
  free(scan);
  result->data = builder.data;
  result->length = builder.length;
  return laghu_sha256_hex((laghu_buffer){result->data, result->length},
                          result->dependency_key);
}

bool laghu_css_rebase_urls(laghu_buffer input, const char *base_path,
                           const char *page_origin,
                           laghu_image_markup_result *result) {
  laghu_css_scan *scan = NULL;
  laghu_css_builder builder = {0};
  size_t cursor = 0U;
  size_t index;
  bool success = false;
  if (result == NULL) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  scan = calloc(1U, sizeof(*scan));
  if (scan == NULL ||
      !laghu_css_scan_input(input, base_path, page_origin, false, scan) ||
      !scan->valid || !scan->bounded) {
    goto finished;
  }
  for (index = 0U; index < scan->url_count; ++index) {
    const laghu_css_url *url = &scan->urls[index];
    if (!laghu_css_append(&builder, input.data + cursor,
                          url->value_start - cursor) ||
        !laghu_css_append(&builder, url->normalized, strlen(url->normalized))) {
      goto finished;
    }
    cursor = url->value_end;
  }
  if (!laghu_css_append(&builder, input.data + cursor, input.length - cursor)) {
    goto finished;
  }
  result->data = builder.data;
  result->length = builder.length;
  builder.data = NULL;
  success = true;
finished:
  free(scan);
  free(builder.data);
  if (!success) {
    laghu_image_markup_result_release(result);
  }
  return success;
}

bool laghu_css_rewrite_style_attributes(
    laghu_buffer html, const char *page_path, const char *page_origin,
    const laghu_image_markup_options *options,
    laghu_image_markup_result *result) {
  laghu_css_builder builder = {0};
  size_t cursor = 0U;
  if (result == NULL || options == NULL ||
      (html.data == NULL && html.length != 0U)) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  while (cursor < html.length) {
    const unsigned char *attribute =
        laghu_css_case_find(html.data + cursor, html.length - cursor, "style=");
    size_t start;
    size_t value_start;
    size_t value_end;
    unsigned char quote;
    laghu_image_markup_result rewritten;
    if (attribute == NULL) {
      if (!laghu_css_append(&builder, html.data + cursor,
                            html.length - cursor)) {
        goto failed;
      }
      break;
    }
    start = (size_t)(attribute - html.data);
    if (!laghu_css_append(&builder, html.data + cursor, start + 6U - cursor)) {
      goto failed;
    }
    value_start = start + 6U;
    if (value_start >= html.length) {
      goto failed;
    }
    quote = (html.data[value_start] == '\'' || html.data[value_start] == '"')
                ? html.data[value_start++]
                : 0U;
    if (quote != 0U && !laghu_css_append(&builder, &quote, 1U)) {
      goto failed;
    }
    value_end = value_start;
    while (value_end < html.length &&
           ((quote != 0U && html.data[value_end] != quote) ||
            (quote == 0U && !isspace(html.data[value_end]) &&
             html.data[value_end] != '>'))) {
      value_end += html.data[value_end] == '\\' && value_end + 1U < html.length
                       ? 2U
                       : 1U;
    }
    if ((quote != 0U && value_end == html.length) ||
        !laghu_css_minify_and_rewrite(
            (laghu_buffer){html.data + value_start, value_end - value_start},
            page_path, page_origin, options, true, &rewritten)) {
      goto failed;
    }
    if (!laghu_css_append(&builder, rewritten.data, rewritten.length) ||
        (quote != 0U && !laghu_css_append(&builder, &quote, 1U))) {
      laghu_image_markup_result_release(&rewritten);
      goto failed;
    }
    result->applied_filters |= rewritten.applied_filters;
    laghu_image_markup_result_release(&rewritten);
    cursor = value_end + (quote != 0U ? 1U : 0U);
  }
  result->data = builder.data;
  result->length = builder.length;
  return laghu_sha256_hex((laghu_buffer){result->data, result->length},
                          result->dependency_key);

failed:
  free(builder.data);
  memset(result, 0, sizeof(*result));
  return false;
}
