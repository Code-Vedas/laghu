// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/layout.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { unsigned char *data; size_t length; size_t capacity; } laghu_layout_builder;

static bool laghu_layout_append(laghu_layout_builder *builder, const void *data, size_t length) {
  size_t needed;
  unsigned char *grown;
  if (length > SIZE_MAX - builder->length - 1U) return false;
  needed = builder->length + length + 1U;
  if (needed > builder->capacity) {
    size_t capacity = builder->capacity == 0U ? 1024U : builder->capacity;
    while (capacity < needed) capacity = capacity > SIZE_MAX / 2U ? needed : capacity * 2U;
    grown = realloc(builder->data, capacity);
    if (grown == NULL) return false;
    builder->data = grown;
    builder->capacity = capacity;
  }
  memcpy(builder->data + builder->length, data, length);
  builder->length += length;
  builder->data[builder->length] = '\0';
  return true;
}

static bool laghu_layout_id_valid(const char *value) {
  size_t index;
  if (value == NULL || !isalpha((unsigned char)value[0])) return false;
  for (index = 1U; value[index] != '\0'; ++index)
    if (!isalnum((unsigned char)value[index]) && value[index] != '-' && value[index] != '_') return false;
  return index < LAGHU_LAYOUT_RESERVATION_ID_SIZE;
}

bool laghu_layout_reservations_load(const char *path, laghu_layout_reservation_set *set, char *error, size_t error_size) {
  FILE *file;
  char line[512U];
  unsigned int line_no = 0U;
  if (set == NULL || path == NULL || (file = fopen(path, "rb")) == NULL) goto invalid;
  memset(set, 0, sizeof(*set));
  while (fgets(line, sizeof(line), file) != NULL) {
    laghu_layout_reservation_rule *rule;
    char kind[8U], id[LAGHU_LAYOUT_RESERVATION_ID_SIZE], extra[2U];
    unsigned int first, second = 0U;
    ++line_no;
    if (line[0] == '#' || strspn(line, " \t\r\n") == strlen(line)) continue;
    if (set->count == LAGHU_LAYOUT_RESERVATION_MAX_RULES ||
        sscanf(line, "%7s %127s %u %u %1s", kind, id, &first, &second, extra) < 3 || !laghu_layout_id_valid(id))
      goto invalid_close;
    {
      unsigned int index;
      for (index = 0U; index < set->count; ++index)
        if (strcmp(set->rules[index].id, id) == 0) goto invalid_close;
    }
    rule = &set->rules[set->count];
    if (strcmp(kind, "box") == 0 && second > 0U && first <= 10000U && second <= 10000U &&
        sscanf(line, "%7s %127s %u %u %1s", kind, id, &first, &second, extra) == 4) {
      rule->kind = LAGHU_LAYOUT_RESERVATION_BOX;
      rule->width = first;
      rule->height = second;
    } else if (strcmp(kind, "font") == 0 && first >= 100U && first <= 10000U &&
               sscanf(line, "%7s %127s %u %1s", kind, id, &first, extra) == 3) {
      rule->kind = LAGHU_LAYOUT_RESERVATION_FONT;
      rule->font_size_adjust_milli = first;
    } else {
      goto invalid_close;
    }
    strcpy(rule->id, id);
    ++set->count;
  }
  if (ferror(file)) goto invalid_close;
  fclose(file);
  return true;
invalid_close:
  fclose(file);
invalid:
  if (error != NULL && error_size != 0U) (void)snprintf(error, error_size, "layout reservation config line %u is invalid", line_no);
  if (set != NULL) memset(set, 0, sizeof(*set));
  return false;
}

static const laghu_layout_reservation_rule *laghu_layout_rule(const laghu_layout_reservation_set *set, const unsigned char *tag, size_t length) {
  const unsigned char *id = NULL;
  size_t cursor = 0U, id_length = 0U, index;
  while (cursor + 3U < length) {
    if ((cursor == 0U || tag[cursor - 1U] == '<' || isspace(tag[cursor - 1U])) &&
        (tag[cursor] == 'i' || tag[cursor] == 'I') && (tag[cursor + 1U] == 'd' || tag[cursor + 1U] == 'D') && tag[cursor + 2U] == '=') {
      unsigned char quote = tag[cursor + 3U];
      size_t start = cursor + (quote == '\'' || quote == '\"' ? 4U : 3U);
      size_t end = start;
      while (end < length && (quote == '\'' || quote == '\"' ? tag[end] != quote : !isspace(tag[end]) && tag[end] != '>')) ++end;
      id = tag + start;
      id_length = end - start;
      break;
    }
    ++cursor;
  }
  if (id == NULL || id_length == 0U) return NULL;
  for (index = 0U; index < set->count; ++index)
    if (strlen(set->rules[index].id) == id_length && memcmp(set->rules[index].id, id, id_length) == 0) return &set->rules[index];
  return NULL;
}

static bool laghu_layout_has_style(const unsigned char *tag, size_t length) {
  size_t index;
  for (index = 0U; index + 6U <= length; ++index)
    if ((index == 0U || tag[index - 1U] == '<' || isspace(tag[index - 1U])) && tolower(tag[index]) == 's' && tolower(tag[index + 1U]) == 't' && tolower(tag[index + 2U]) == 'y' &&
        tolower(tag[index + 3U]) == 'l' && tolower(tag[index + 4U]) == 'e' && tag[index + 5U] == '=')
      return true;
  return false;
}

static size_t laghu_layout_raw_tag_name(const unsigned char *tag, size_t length) {
  static const char *const names[] = {"script", "style", "textarea", "title", "template"};
  size_t index;
  if (length < 3U || tag[0] != '<' || tag[1] == '/' || tag[1] == '!' || tag[1] == '?') return 0U;
  for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
    size_t name_length = strlen(names[index]);
    if (length > name_length + 1U && strncasecmp((const char *)tag + 1U, names[index], name_length) == 0 &&
        (isspace(tag[name_length + 1U]) || tag[name_length + 1U] == '>'))
      return name_length;
  }
  return 0U;
}

static const unsigned char *laghu_layout_raw_tag_end(const unsigned char *start, size_t remaining, const unsigned char *html, size_t html_length,
                                                       size_t name_length) {
  size_t index;
  for (index = 0U; index + name_length + 3U <= remaining; ++index)
    if (start[index] == '<' && start[index + 1U] == '/' && strncasecmp((const char *)start + index + 2U, (const char *)start + 1U, name_length) == 0 &&
        (isspace(start[index + name_length + 2U]) || start[index + name_length + 2U] == '>')) {
      const unsigned char *end = memchr(start + index, '>', html_length - (size_t)(start + index - html));
      if (end != NULL) return end;
    }
  return NULL;
}

bool laghu_layout_reservations_apply(laghu_buffer html, const laghu_layout_reservation_set *set, const laghu_csp_policy *csp,
                                     laghu_runtime_html_result *result) {
  laghu_layout_builder builder = {0};
  size_t cursor = 0U;
  bool changed = false;
  if (result == NULL || html.data == NULL || set == NULL) return false;
  memset(result, 0, sizeof(*result));
  if (set->count == 0U || !laghu_csp_allows_style_attribute(csp)) return true;
  while (cursor < html.length) {
    const unsigned char *start = memchr(html.data + cursor, '<', html.length - cursor);
    const unsigned char *end;
    const laghu_layout_reservation_rule *rule;
    char style[160U];
    int style_length;
    if (start == NULL) {
      if (!laghu_layout_append(&builder, html.data + cursor, html.length - cursor)) goto failed;
      break;
    }
    if (start > html.data + cursor && !laghu_layout_append(&builder, html.data + cursor, (size_t)(start - html.data - cursor))) goto failed;
    end = memchr(start, '>', html.length - (size_t)(start - html.data));
    if (end == NULL) goto failed;
    {
      size_t raw_name_length = laghu_layout_raw_tag_name(start, (size_t)(end - start + 1U));
      if (raw_name_length != 0U) {
        const unsigned char *raw_end = laghu_layout_raw_tag_end(start, html.length - (size_t)(start - html.data), html.data, html.length, raw_name_length);
        if (raw_end == NULL || !laghu_layout_append(&builder, start, (size_t)(raw_end - start + 1U))) goto failed;
        cursor = (size_t)(raw_end - html.data) + 1U;
        continue;
      }
    }
    rule = laghu_layout_rule(set, start, (size_t)(end - start + 1U));
    if (rule == NULL || laghu_layout_has_style(start, (size_t)(end - start + 1U))) {
      if (!laghu_layout_append(&builder, start, (size_t)(end - start + 1U))) goto failed;
    } else {
      if (rule->kind == LAGHU_LAYOUT_RESERVATION_BOX)
        style_length = snprintf(style, sizeof(style), " style=\"box-sizing:border-box;width:%upx;min-height:%upx;aspect-ratio:%u / %u\"",
                                rule->width, rule->height, rule->width, rule->height);
      else
        style_length = snprintf(style, sizeof(style), " style=\"font-size-adjust:%u.%03u\"", rule->font_size_adjust_milli / 1000U,
                                rule->font_size_adjust_milli % 1000U);
      if (style_length <= 0 || (size_t)style_length >= sizeof(style) ||
          !laghu_layout_append(&builder, start, (size_t)(end - start)) || !laghu_layout_append(&builder, style, (size_t)style_length) ||
          !laghu_layout_append(&builder, ">", 1U))
        goto failed;
      changed = true;
    }
    cursor = (size_t)(end - html.data) + 1U;
  }
  if (!changed) { free(builder.data); return true; }
  result->data = builder.data;
  result->length = builder.length;
  result->rewritten = true;
  return true;
failed:
  free(builder.data);
  return false;
}
