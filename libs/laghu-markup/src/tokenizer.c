// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <string.h>

#include "laghu/markup.h"

static bool laghu_markup_name_char(unsigned char value) { return isalnum(value) || value == '-' || value == ':' || value == '_'; }

bool laghu_html_next_tag(laghu_buffer html, size_t *cursor, laghu_html_tag *tag) {
  size_t start, at, name_start;
  unsigned char quote = 0U;
  if (cursor == NULL || tag == NULL || (html.data == NULL && html.length != 0U) || *cursor > html.length) return false;
  start = *cursor;
next_candidate:
  while (start < html.length && html.data[start] != '<') ++start;
  if (start == html.length) {
    *cursor = html.length;
    return false;
  }
  if (html.length - start >= 4U && memcmp(html.data + start, "<!--", 4U) == 0) {
    size_t end = start + 4U;
    while (end + 3U <= html.length && memcmp(html.data + end, "-->", 3U) != 0) ++end;
    if (end + 3U > html.length) {
      *cursor = html.length;
      return false;
    }
    start = end + 3U;
    goto next_candidate;
  }
  at = start + 1U;
  while (at < html.length && isspace(html.data[at])) ++at;
  memset(tag, 0, sizeof(*tag));
  tag->closing = at < html.length && html.data[at] == '/';
  if (tag->closing) ++at;
  while (at < html.length && isspace(html.data[at])) ++at;
  name_start = at;
  while (at < html.length && laghu_markup_name_char(html.data[at])) ++at;
  if (at == name_start) {
    start += 1U;
    goto next_candidate;
  }
  tag->name = (laghu_buffer){html.data + name_start, at - name_start};
  while (at < html.length) {
    unsigned char value = html.data[at];
    if (quote != 0U) {
      if (value == quote) quote = 0U;
    } else if (value == '\'' || value == '"') {
      quote = value;
    } else if (value == '>') {
      size_t before = at;
      while (before > name_start && isspace(html.data[before - 1U])) --before;
      tag->self_closing = before > name_start && html.data[before - 1U] == '/';
      tag->source = (laghu_buffer){html.data + start, at - start + 1U};
      tag->start = start;
      tag->end = at + 1U;
      *cursor = tag->end;
      return true;
    }
    ++at;
  }
  *cursor = html.length;
  return false;
}

bool laghu_html_tag_attribute(const laghu_html_tag *tag, const char *name, laghu_html_attribute *attribute) {
  size_t cursor;
  if (tag == NULL || name == NULL || attribute == NULL || tag->source.data == NULL) return false;
  cursor = 1U;
  if (cursor < tag->source.length && tag->source.data[cursor] == '/') ++cursor;
  while (cursor < tag->source.length && laghu_markup_name_char(tag->source.data[cursor])) ++cursor;
  while (cursor < tag->source.length) {
    size_t start, name_end, value_start = 0U;
    unsigned char quote = 0U;
    while (cursor < tag->source.length && (isspace(tag->source.data[cursor]) || tag->source.data[cursor] == '/')) ++cursor;
    if (cursor >= tag->source.length || tag->source.data[cursor] == '>') break;
    start = cursor;
    while (cursor < tag->source.length && laghu_markup_name_char(tag->source.data[cursor])) ++cursor;
    name_end = cursor;
    if (name_end == start) {
      ++cursor;
      continue;
    }
    while (cursor < tag->source.length && isspace(tag->source.data[cursor])) ++cursor;
    memset(attribute, 0, sizeof(*attribute));
    attribute->name = (laghu_buffer){tag->source.data + start, name_end - start};
    attribute->start = start;
    if (cursor < tag->source.length && tag->source.data[cursor] == '=') {
      attribute->has_value = true;
      ++cursor;
      while (cursor < tag->source.length && isspace(tag->source.data[cursor])) ++cursor;
      if (cursor < tag->source.length && (tag->source.data[cursor] == '\'' || tag->source.data[cursor] == '"')) quote = tag->source.data[cursor++];
      value_start = cursor;
      while (cursor < tag->source.length &&
             (quote != 0U ? tag->source.data[cursor] != quote : !isspace(tag->source.data[cursor]) && tag->source.data[cursor] != '>'))
        ++cursor;
      attribute->value = (laghu_buffer){tag->source.data + value_start, cursor - value_start};
      if (quote != 0U && cursor < tag->source.length) ++cursor;
    }
    attribute->end = cursor;
    if (laghu_base_ascii_equal(attribute->name, name)) return true;
  }
  return false;
}

bool laghu_html_tag_has_attribute(const laghu_html_tag *tag, const char *name) {
  laghu_html_attribute attribute;
  return laghu_html_tag_attribute(tag, name, &attribute);
}

bool laghu_srcset_next(laghu_buffer srcset, size_t *cursor, laghu_srcset_candidate *candidate) {
  size_t start, end, descriptor_start, descriptor_end;
  if (cursor == NULL || candidate == NULL || (srcset.data == NULL && srcset.length != 0U) || *cursor > srcset.length) return false;
  while (*cursor < srcset.length && (isspace(srcset.data[*cursor]) || srcset.data[*cursor] == ',')) ++*cursor;
  if (*cursor == srcset.length) return false;
  start = *cursor;
  while (*cursor < srcset.length && !isspace(srcset.data[*cursor]) && srcset.data[*cursor] != ',') ++*cursor;
  end = *cursor;
  while (*cursor < srcset.length && isspace(srcset.data[*cursor])) ++*cursor;
  descriptor_start = *cursor;
  while (*cursor < srcset.length && srcset.data[*cursor] != ',') ++*cursor;
  descriptor_end = *cursor;
  while (descriptor_end > descriptor_start && isspace(srcset.data[descriptor_end - 1U])) --descriptor_end;
  if (*cursor < srcset.length) ++*cursor;
  candidate->url = (laghu_buffer){srcset.data + start, end - start};
  candidate->descriptor = (laghu_buffer){srcset.data + descriptor_start, descriptor_end - descriptor_start};
  return end > start;
}
