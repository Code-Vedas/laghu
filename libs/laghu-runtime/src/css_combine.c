// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/cache.h"
#include "laghu/css.h"
#include "laghu/types.h"

#define LAGHU_CSS_COMBINE_VERSION 1U
#define LAGHU_CSS_COMBINE_MAX_INPUTS 32U
#define LAGHU_CSS_COMBINE_MAX_PAGE_LINKS 64U

typedef struct {
  unsigned char *data;
  size_t length;
  size_t capacity;
} laghu_combine_builder;

typedef struct {
  size_t start;
  size_t end;
  char url[LAGHU_RUNTIME_PATH_SIZE];
  char media[128U];
  laghu_stylesheet_record record;
  unsigned char *css;
  size_t css_length;
} laghu_combine_link;

static bool laghu_combine_append(laghu_combine_builder *builder, const void *data, size_t length) {
  size_t needed;
  unsigned char *grown;
  if (length > SIZE_MAX - builder->length - 1U) {
    return false;
  }
  needed = builder->length + length + 1U;
  if (needed > builder->capacity) {
    size_t capacity = builder->capacity == 0U ? 1024U : builder->capacity;
    while (capacity < needed) {
      capacity = capacity > SIZE_MAX / 2U ? needed : capacity * 2U;
    }
    grown = realloc(builder->data, capacity);
    if (grown == NULL) {
      return false;
    }
    builder->data = grown;
    builder->capacity = capacity;
  }
  memcpy(builder->data + builder->length, data, length);
  builder->length += length;
  builder->data[builder->length] = '\0';
  return true;
}

static bool laghu_combine_equal(const unsigned char *value, size_t length, const char *expected) {
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

static bool laghu_combine_contains(const unsigned char *data, size_t length, const char *needle) {
  size_t index;
  size_t needle_length = strlen(needle);
  for (index = 0U; index + needle_length <= length; ++index) {
    if (laghu_combine_equal(data + index, needle_length, needle)) {
      return true;
    }
  }
  return false;
}

static bool laghu_combine_starts_with(const unsigned char *data, size_t length, const char *prefix) {
  size_t prefix_length = strlen(prefix);
  return length >= prefix_length && laghu_combine_equal(data, prefix_length, prefix);
}

static bool laghu_combine_normalize_url(const unsigned char *url, size_t length, const char *page_path, const char *page_origin,
                                        char output[LAGHU_RUNTIME_PATH_SIZE]) {
  return laghu_base_url_resolve_same_origin(page_path, page_origin, (laghu_buffer){url, length},
                                            LAGHU_URL_REJECT_TRAVERSAL | LAGHU_URL_REJECT_API | LAGHU_URL_REJECT_QUERY | LAGHU_URL_REJECT_FRAGMENT,
                                            output, LAGHU_RUNTIME_PATH_SIZE);
}

static bool laghu_combine_media(const unsigned char *value, size_t length, char output[128U]) {
  size_t start = 0U;
  size_t end = length;
  size_t written = 0U;
  bool whitespace = false;
  while (start < end && isspace(value[start])) {
    ++start;
  }
  while (end > start && isspace(value[end - 1U])) {
    --end;
  }
  while (start < end) {
    unsigned char byte = value[start++];
    if (byte == '"' || byte == '\'' || byte == '<' || byte == '>') {
      return false;
    }
    if (isspace(byte)) {
      whitespace = written != 0U;
      continue;
    }
    if (whitespace) {
      if (written + 1U >= 128U) {
        return false;
      }
      output[written++] = ' ';
      whitespace = false;
    }
    if (written + 1U >= 128U) {
      return false;
    }
    output[written++] = (char)tolower(byte);
  }
  output[written] = '\0';
  if (strcmp(output, "all") == 0) {
    output[0] = '\0';
  }
  return true;
}

static bool laghu_combine_parse_link(laghu_buffer html, size_t start, const char *page_path, const char *page_origin, laghu_combine_link *link) {
  size_t cursor = start + 1U;
  bool have_rel = false;
  bool have_href = false;
  bool have_media = false;
  bool have_type = false;
  if (start >= html.length || html.data[start] != '<' || start + 5U > html.length || !laghu_combine_equal(html.data + start + 1U, 4U, "link")) {
    return false;
  }
  cursor += 4U;
  if (cursor < html.length && !isspace(html.data[cursor]) && html.data[cursor] != '>') {
    return false;
  }
  memset(link, 0, sizeof(*link));
  link->start = start;
  while (cursor < html.length) {
    size_t name_start;
    size_t name_end;
    size_t value_start;
    size_t value_end;
    unsigned char quote = 0U;
    while (cursor < html.length && isspace(html.data[cursor])) {
      ++cursor;
    }
    if (cursor >= html.length) {
      return false;
    }
    if (html.data[cursor] == '>') {
      link->end = cursor;
      break;
    }
    if (html.data[cursor] == '/') {
      ++cursor;
      continue;
    }
    name_start = cursor;
    while (cursor < html.length && (isalnum(html.data[cursor]) || html.data[cursor] == '-' || html.data[cursor] == '_')) {
      ++cursor;
    }
    name_end = cursor;
    while (cursor < html.length && isspace(html.data[cursor])) {
      ++cursor;
    }
    if (name_end == name_start || cursor >= html.length || html.data[cursor] != '=') {
      return false;
    }
    ++cursor;
    while (cursor < html.length && isspace(html.data[cursor])) {
      ++cursor;
    }
    if (cursor < html.length && (html.data[cursor] == '\'' || html.data[cursor] == '"')) {
      quote = html.data[cursor++];
    }
    value_start = cursor;
    while (cursor < html.length &&
           ((quote != 0U && html.data[cursor] != quote) || (quote == 0U && !isspace(html.data[cursor]) && html.data[cursor] != '>'))) {
      ++cursor;
    }
    value_end = cursor;
    if (quote != 0U) {
      if (cursor >= html.length) {
        return false;
      }
      ++cursor;
    }
    if (laghu_combine_equal(html.data + name_start, name_end - name_start, "rel")) {
      if (have_rel || !laghu_combine_equal(html.data + value_start, value_end - value_start, "stylesheet")) {
        return false;
      }
      have_rel = true;
    } else if (laghu_combine_equal(html.data + name_start, name_end - name_start, "href")) {
      if (have_href || !laghu_combine_normalize_url(html.data + value_start, value_end - value_start, page_path, page_origin, link->url)) {
        return false;
      }
      have_href = true;
    } else if (laghu_combine_equal(html.data + name_start, name_end - name_start, "media")) {
      if (have_media || !laghu_combine_media(html.data + value_start, value_end - value_start, link->media)) {
        return false;
      }
      have_media = true;
    } else if (laghu_combine_equal(html.data + name_start, name_end - name_start, "type")) {
      if (have_type || !laghu_combine_equal(html.data + value_start, value_end - value_start, "text/css")) {
        return false;
      }
      have_type = true;
    } else {
      return false;
    }
  }
  return have_rel && have_href && link->end > link->start;
}

static bool laghu_combine_css_safe(laghu_buffer css) {
  size_t cursor;
  static const char *const forbidden[] = {
      "@import", "@font-face", "@charset", "@namespace", "sourcemappingurl", "sourceurl",
  };
  size_t index;
  for (index = 0U; index < sizeof(forbidden) / sizeof(forbidden[0]); ++index) {
    if (laghu_combine_contains(css.data, css.length, forbidden[index])) {
      return false;
    }
  }
  for (cursor = 0U; cursor + 4U <= css.length; ++cursor) {
    size_t value;
    unsigned char quote = 0U;
    if (!laghu_combine_equal(css.data + cursor, 4U, "url(")) {
      continue;
    }
    value = cursor + 4U;
    while (value < css.length && isspace(css.data[value])) {
      ++value;
    }
    if (value < css.length && (css.data[value] == '\'' || css.data[value] == '"')) {
      quote = css.data[value++];
    }
    if (value >= css.length ||
        !(css.data[value] == '/' || css.data[value] == '#' || laghu_combine_starts_with(css.data + value, css.length - value, "data:") ||
          laghu_combine_starts_with(css.data + value, css.length - value, "blob:") ||
          laghu_combine_starts_with(css.data + value, css.length - value, "http://") ||
          laghu_combine_starts_with(css.data + value, css.length - value, "https://"))) {
      return false;
    }
    (void)quote;
  }
  return true;
}

static bool laghu_combine_read(const char *cache_path, laghu_combine_link *link) {
  laghu_runtime_cache_entry entry;
  const char *key = link->record.derived_key;
  if (!laghu_runtime_cache_lookup_variant(cache_path, key, &entry) || entry.length == 0U || entry.length > LAGHU_CSS_MAX_INPUT_BYTES) {
    return false;
  }
  link->css = malloc(entry.length + 1U);
  if (link->css == NULL || !laghu_runtime_cache_read(&entry, link->css, entry.length)) {
    free(link->css);
    link->css = NULL;
    return false;
  }
  link->css[entry.length] = '\0';
  link->css_length = entry.length;
  return true;
}

static bool laghu_combine_key(const laghu_combine_link *links, size_t count, laghu_buffer combined, const char *policy_key, uint32_t capability_mask,
                              unsigned int inline_limit, unsigned int outline_threshold, char output[LAGHU_RUNTIME_KEY_SIZE]) {
  size_t capacity = 256U + count * (LAGHU_RUNTIME_PATH_SIZE + LAGHU_RUNTIME_KEY_SIZE * 3U + 160U);
  char combined_hash[LAGHU_RUNTIME_KEY_SIZE];
  char *material = malloc(capacity);
  size_t length;
  size_t index;
  int written;
  if (material == NULL || !laghu_sha256_hex(combined, combined_hash)) {
    free(material);
    return false;
  }
  written = snprintf(material, capacity, "laghu-css-combine-v%u\n%s\n%s\n%u\n%u\n%u\n%u\n%u", LAGHU_CSS_COMBINE_VERSION, combined_hash, policy_key,
                     capability_mask, LAGHU_CSS_DERIVATION_VERSION, inline_limit, outline_threshold, LAGHU_CSS_COMBINE_MAX_INPUTS);
  if (written <= 0 || (size_t)written >= capacity) {
    free(material);
    return false;
  }
  length = (size_t)written;
  for (index = 0U; index < count; ++index) {
    written = snprintf(material + length, capacity - length, "\n%s\n%s\n%s\n%s\n%s", links[index].url, links[index].record.source_hash,
                       links[index].record.derived_key, links[index].record.dependency_key, links[index].media);
    if (written <= 0 || (size_t)written >= capacity - length) {
      free(material);
      return false;
    }
    length += (size_t)written;
  }
  if (!laghu_sha256_hex((laghu_buffer){(const unsigned char *)material, length}, output)) {
    free(material);
    return false;
  }
  free(material);
  return true;
}

bool laghu_runtime_combine_css_markup(const char *cache_path, laghu_buffer html, const char *page_path, const char *page_origin,
                                      const char *policy_key, uint32_t capability_mask, uint64_t now, unsigned int ttl_seconds,
                                      unsigned int inline_limit, unsigned int outline_threshold, laghu_runtime_css_combine_result *result) {
  typedef char laghu_seen_path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_combine_builder output = {0};
  laghu_seen_path *seen = NULL;
  laghu_combine_link *links = NULL;
  size_t seen_count = 0U;
  size_t cursor = 0U;
  bool changed = false;
  if (result == NULL || cache_path == NULL || page_path == NULL || policy_key == NULL || ttl_seconds == 0U) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  seen = calloc(LAGHU_CSS_COMBINE_MAX_PAGE_LINKS, sizeof(*seen));
  if (seen == NULL) {
    return false;
  }
  while (cursor < html.length) {
    const unsigned char *open = memchr(html.data + cursor, '<', html.length - cursor);
    size_t count = 0U;
    size_t group_end;
    size_t next;
    bool overflow = false;
    bool blocked = false;
    size_t index;
    laghu_combine_builder combined = {0};
    char key[LAGHU_RUNTIME_KEY_SIZE];
    laghu_runtime_cache_entry entry;
    char link_markup[384U];
    int link_length;
    links = calloc(LAGHU_CSS_COMBINE_MAX_INPUTS, sizeof(*links));
    if (links == NULL) {
      goto failed;
    }
    if (open == NULL) {
      if (!laghu_combine_append(&output, html.data + cursor, html.length - cursor)) {
        goto failed;
      }
      free(links);
      links = NULL;
      break;
    }
    next = (size_t)(open - html.data);
    if (!laghu_combine_parse_link(html, next, page_path, page_origin, &links[0])) {
      if (!laghu_combine_append(&output, html.data + cursor, next + 1U - cursor)) {
        goto failed;
      }
      cursor = next + 1U;
      free(links);
      links = NULL;
      continue;
    }
    count = 1U;
    group_end = links[0].end + 1U;
    while (group_end < html.length) {
      next = group_end;
      while (next < html.length && isspace(html.data[next])) {
        ++next;
      }
      if (next >= html.length || html.data[next] != '<') {
        break;
      }
      if (count == LAGHU_CSS_COMBINE_MAX_INPUTS) {
        laghu_combine_link extra;
        overflow = laghu_combine_parse_link(html, next, page_path, page_origin, &extra) && strcmp(extra.media, links[0].media) == 0;
        while (overflow) {
          group_end = extra.end + 1U;
          next = group_end;
          while (next < html.length && isspace(html.data[next])) {
            ++next;
          }
          if (next >= html.length || html.data[next] != '<' || !laghu_combine_parse_link(html, next, page_path, page_origin, &extra) ||
              strcmp(extra.media, links[0].media) != 0) {
            break;
          }
        }
        break;
      }
      if (!laghu_combine_parse_link(html, next, page_path, page_origin, &links[count]) || strcmp(links[count].media, links[0].media) != 0) {
        break;
      }
      group_end = links[count].end + 1U;
      ++count;
    }
    if (count < 2U || overflow) {
      if (!laghu_combine_append(&output, html.data + cursor, group_end - cursor)) {
        goto failed;
      }
      cursor = group_end;
      free(links);
      links = NULL;
      continue;
    }
    for (index = 0U; index < count; ++index) {
      if (!laghu_stylesheet_lookup(cache_path, links[index].url, policy_key, capability_mask, inline_limit, outline_threshold, now, ttl_seconds,
                                   &links[index].record)) {
        result->dependencies_pending = true;
        for (index = 0U; index < count; ++index) {
          free(links[index].css);
        }
        free(combined.data);
        free(links);
        links = NULL;
        goto unchanged;
      }
      if (!links[index].record.ready || links[index].record.terminally_excluded || !laghu_combine_read(cache_path, &links[index]) ||
          !laghu_combine_css_safe((laghu_buffer){links[index].css, links[index].css_length}) ||
          (index != 0U && !laghu_combine_append(&combined, "\n", 1U)) ||
          !laghu_combine_append(&combined, links[index].css, links[index].css_length) || combined.length > LAGHU_CSS_MAX_INPUT_BYTES) {
        blocked = true;
        break;
      }
    }
    if (!blocked) {
      laghu_css_parse_result *parsed = calloc(1U, sizeof(*parsed));
      if (parsed == NULL) {
        blocked = true;
      } else {
        blocked = !laghu_css_discover((laghu_buffer){combined.data, combined.length}, "/.laghu/css/combined.css", page_origin, parsed) ||
                  !parsed->valid || !parsed->bounded ||
                  !laghu_combine_key(links, count, (laghu_buffer){combined.data, combined.length}, policy_key, capability_mask, inline_limit,
                                     outline_threshold, key);
        free(parsed);
      }
    }
    if (blocked) {
      for (index = 0U; index < count; ++index) {
        free(links[index].css);
      }
      free(combined.data);
      if (!laghu_combine_append(&output, html.data + cursor, group_end - cursor)) {
        goto failed;
      }
      cursor = group_end;
      free(links);
      links = NULL;
      continue;
    }
    if (!laghu_runtime_cache_lookup_variant(cache_path, key, &entry)) {
      if (!laghu_runtime_cache_publish(cache_path, key, key, key, "text/css", "laghu-css-combine", (laghu_buffer){combined.data, combined.length},
                                       &entry)) {
        for (index = 0U; index < count; ++index) {
          free(links[index].css);
        }
        free(combined.data);
        goto failed;
      }
    }
    if (!laghu_combine_append(&output, html.data + cursor, links[0].start - cursor)) {
      for (index = 0U; index < count; ++index) {
        free(links[index].css);
      }
      free(combined.data);
      goto failed;
    }
    link_length = snprintf(link_markup, sizeof(link_markup), "<link rel=\"stylesheet\" href=\"/.laghu/css/%s\"%s%s%s>", key,
                           links[0].media[0] == '\0' ? "" : " media=\"", links[0].media, links[0].media[0] == '\0' ? "" : "\"");
    if (link_length <= 0 || (size_t)link_length >= sizeof(link_markup) || !laghu_combine_append(&output, link_markup, (size_t)link_length)) {
      for (index = 0U; index < count; ++index) {
        free(links[index].css);
      }
      free(combined.data);
      goto failed;
    }
    for (index = 0U; index < count; ++index) {
      size_t seen_index;
      bool duplicate = false;
      for (seen_index = 0U; seen_index < seen_count; ++seen_index) {
        if (strcmp(seen[seen_index], links[index].url) == 0) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate && seen_count < LAGHU_CSS_COMBINE_MAX_PAGE_LINKS) {
        strcpy(seen[seen_count++], links[index].url);
        result->original_external_bytes += links[index].css_length;
      }
      free(links[index].css);
    }
    result->combined_external_bytes += combined.length;
    free(combined.data);
    free(links);
    links = NULL;
    cursor = group_end;
    changed = true;
  }
  if (changed) {
    result->data = output.data;
    result->length = output.length;
    result->rewritten = true;
    free(seen);
    return true;
  }

unchanged:
  free(output.data);
  free(seen);
  return true;

failed:
  free(links);
  free(output.data);
  free(seen);
  memset(result, 0, sizeof(*result));
  return false;
}

void laghu_runtime_css_combine_result_release(laghu_runtime_css_combine_result *result) {
  if (result != NULL) {
    free(result->data);
    memset(result, 0, sizeof(*result));
  }
}
