// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/cache.h"
#include "laghu/catalog.h"
#include "laghu/css.h"
#include "laghu/html.h"
#include "laghu/types.h"

typedef struct {
  unsigned char *data;
  size_t length;
  size_t capacity;
} laghu_hint_builder;

static bool laghu_hint_append(laghu_hint_builder *builder, const void *data, size_t length) {
  unsigned char *grown;
  size_t needed;
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

static bool laghu_hint_equal(const unsigned char *value, size_t length, const char *expected) {
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

static bool laghu_hint_case_equal(const char *left, const char *right) {
  size_t index = 0U;
  if (left == NULL || right == NULL) {
    return left == right;
  }
  while (left[index] != '\0' && right[index] != '\0') {
    if (tolower((unsigned char)left[index]) != tolower((unsigned char)right[index])) {
      return false;
    }
    ++index;
  }
  return left[index] == right[index];
}

static bool laghu_hint_attribute(const unsigned char *tag, size_t length, const char *name, const unsigned char **value, size_t *value_length,
                                 unsigned int *matches) {
  size_t cursor = 1U;
  *value = NULL;
  *value_length = 0U;
  *matches = 0U;
  while (cursor < length && !isspace(tag[cursor]) && tag[cursor] != '>') {
    ++cursor;
  }
  while (cursor < length) {
    size_t attribute_start;
    size_t attribute_end;
    size_t start;
    unsigned char quote = 0U;
    while (cursor < length && (isspace(tag[cursor]) || tag[cursor] == '/')) {
      ++cursor;
    }
    attribute_start = cursor;
    while (cursor < length && (isalnum(tag[cursor]) || tag[cursor] == '-' || tag[cursor] == '_' || tag[cursor] == ':')) {
      ++cursor;
    }
    attribute_end = cursor;
    if (attribute_end == attribute_start) {
      break;
    }
    while (cursor < length && isspace(tag[cursor])) {
      ++cursor;
    }
    if (cursor >= length || tag[cursor] != '=') {
      if (laghu_hint_equal(tag + attribute_start, attribute_end - attribute_start, name)) {
        ++*matches;
      }
      continue;
    }
    ++cursor;
    while (cursor < length && isspace(tag[cursor])) {
      ++cursor;
    }
    if (cursor < length && (tag[cursor] == '\'' || tag[cursor] == '"')) {
      quote = tag[cursor++];
    }
    start = cursor;
    while (cursor < length && ((quote != 0U && tag[cursor] != quote) || (quote == 0U && !isspace(tag[cursor]) && tag[cursor] != '>'))) {
      ++cursor;
    }
    if (quote != 0U && cursor == length) {
      return false;
    }
    if (laghu_hint_equal(tag + attribute_start, attribute_end - attribute_start, name)) {
      ++*matches;
      *value = tag + start;
      *value_length = cursor - start;
    }
    if (quote != 0U && cursor < length) {
      ++cursor;
    }
  }
  return true;
}

static bool laghu_hint_language(const unsigned char *value, size_t length, char output[LAGHU_HTML_LANGUAGE_SIZE]) {
  size_t index;
  if (length == 0U || length >= LAGHU_HTML_LANGUAGE_SIZE) {
    return false;
  }
  for (index = 0U; index < length; ++index) {
    unsigned char byte = value[index];
    if (byte < 0x21U || byte > 0x7eU || byte == ',' || byte == ';') {
      return false;
    }
    output[index] = (char)byte;
  }
  output[length] = '\0';
  return true;
}

static bool laghu_hint_mime(const char *value) {
  size_t index;
  bool slash = false;
  if (value == NULL || value[0] == '\0') {
    return false;
  }
  for (index = 0U; value[index] != '\0'; ++index) {
    unsigned char byte = (unsigned char)value[index];
    if (byte == '/' && !slash && index != 0U && value[index + 1U] != '\0') {
      slash = true;
    } else if (!(isalnum(byte) || byte == '!' || byte == '#' || byte == '$' || byte == '&' || byte == '^' || byte == '_' || byte == '.' ||
                 byte == '+' || byte == '-')) {
      return false;
    }
  }
  return slash && index < 80U;
}

static bool laghu_hint_normalize_url(const char *page_path, const char *page_origin, const unsigned char *url, size_t length,
                                     char output[LAGHU_RUNTIME_PATH_SIZE], bool *same_origin) {
  *same_origin = false;
  *same_origin = laghu_base_url_resolve_same_origin(page_path, page_origin, (laghu_buffer){url, length},
                                                    LAGHU_URL_REJECT_TRAVERSAL | LAGHU_URL_REJECT_API, output, LAGHU_RUNTIME_PATH_SIZE);
  return *same_origin;
}

static bool laghu_hint_origin(const unsigned char *url, size_t length, const char *page_origin, char output[LAGHU_RUNTIME_PATH_SIZE]) {
  size_t scheme_length;
  size_t cursor;
  size_t host_start;
  size_t end;
  size_t index;
  size_t port = 0U;
  size_t host_end;
  if (length >= 8U && memcmp(url, "https://", 8U) == 0) {
    scheme_length = 8U;
  } else if (length >= 7U && memcmp(url, "http://", 7U) == 0) {
    scheme_length = 7U;
  } else {
    return false;
  }
  host_start = scheme_length;
  end = host_start;
  while (end < length && url[end] != '/' && url[end] != '?' && url[end] != '#') {
    if (url[end] == '@') {
      return false;
    }
    ++end;
  }
  if (end == host_start || end >= LAGHU_RUNTIME_PATH_SIZE) {
    return false;
  }
  host_end = end;
  for (cursor = host_start; cursor < end; ++cursor) {
    if (url[cursor] == ':') {
      size_t digit;
      if (port != 0U || cursor == host_start || cursor + 1U == end) {
        return false;
      }
      host_end = cursor;
      for (digit = cursor + 1U; digit < end; ++digit) {
        if (!isdigit(url[digit])) {
          return false;
        }
        port = port * 10U + (size_t)(url[digit] - '0');
        if (port > 65535U) {
          return false;
        }
      }
      break;
    }
  }
  if (host_end == host_start || url[host_start] == '.' || url[host_end - 1U] == '.' || url[host_start] == '-' || url[host_end - 1U] == '-') {
    return false;
  }
  for (cursor = host_start; cursor < host_end; ++cursor) {
    unsigned char byte = url[cursor];
    if (!(isalnum(byte) || byte == '.' || byte == '-') || (byte == '.' && cursor + 1U < host_end && url[cursor + 1U] == '.') ||
        (byte == '-' && cursor > host_start && url[cursor - 1U] == '.')) {
      return false;
    }
  }
  if ((scheme_length == 7U && port == 80U) || (scheme_length == 8U && port == 443U)) {
    end = host_end;
  }
  memcpy(output, url, end);
  output[end] = '\0';
  for (index = 0U; index < end; ++index) {
    output[index] = (char)tolower((unsigned char)output[index]);
  }
  if (page_origin != NULL) {
    char normalized_page[LAGHU_RUNTIME_PATH_SIZE];
    if (laghu_hint_origin((const unsigned char *)page_origin, strlen(page_origin), NULL, normalized_page) &&
        laghu_hint_case_equal(output, normalized_page)) {
      return false;
    }
  }
  return true;
}

static bool laghu_hint_existing_seen(const char *existing, const char *target) { return existing != NULL && strstr(existing, target) != NULL; }

static bool laghu_hint_added(const laghu_runtime_html_result *result, const char *target, const char *relation) {
  unsigned int index;
  for (index = 0U; index < result->link_header_count; ++index) {
    char expected[LAGHU_HTML_HEADER_VALUE_SIZE];
    int length = snprintf(expected, sizeof(expected), "<%s>; %s", target, relation);
    if (length > 0 && (size_t)length < sizeof(expected) && strcmp(result->link_headers[index], expected) == 0) {
      return true;
    }
  }
  return false;
}

static bool laghu_hint_link_headers_valid(const char *headers) {
  size_t cursor = 0U;
  if (headers == NULL || headers[0] == '\0') {
    return true;
  }
  while (headers[cursor] != '\0') {
    bool closed = false;
    bool quoted = false;
    while (headers[cursor] == ' ' || headers[cursor] == '\t' || headers[cursor] == ',') {
      ++cursor;
    }
    if (headers[cursor] == '\0') {
      return true;
    }
    if (headers[cursor++] != '<') {
      return false;
    }
    while (headers[cursor] != '\0') {
      unsigned char byte = (unsigned char)headers[cursor++];
      if (byte < 0x20U || byte == 0x7fU) {
        return false;
      }
      if (byte == '>') {
        closed = true;
        break;
      }
    }
    if (!closed) {
      return false;
    }
    while (headers[cursor] != '\0') {
      unsigned char byte = (unsigned char)headers[cursor];
      if (byte < 0x20U && byte != '\t') {
        return false;
      }
      if (byte == '"') {
        quoted = !quoted;
      } else if (byte == ',' && !quoted) {
        ++cursor;
        break;
      }
      ++cursor;
    }
    if (quoted) {
      return false;
    }
  }
  return true;
}

static void laghu_hint_existing_markup(laghu_buffer html, char *output, size_t capacity) {
  size_t cursor = 0U;
  size_t used = 0U;
  output[0] = '\0';
  while (cursor < html.length) {
    const unsigned char *open = memchr(html.data + cursor, '<', html.length - cursor);
    size_t start;
    size_t end;
    const unsigned char *rel;
    const unsigned char *href;
    size_t rel_length;
    size_t href_length;
    unsigned int rel_matches;
    unsigned int href_matches;
    if (open == NULL) {
      break;
    }
    start = (size_t)(open - html.data);
    end = start + 1U;
    while (end < html.length && html.data[end] != '>') {
      ++end;
    }
    if (end == html.length) {
      return;
    }
    if (start + 5U < end && laghu_hint_equal(html.data + start + 1U, 4U, "link") &&
        laghu_hint_attribute(html.data + start, end - start + 1U, "rel", &rel, &rel_length, &rel_matches) &&
        laghu_hint_attribute(html.data + start, end - start + 1U, "href", &href, &href_length, &href_matches) && rel_matches == 1U &&
        href_matches == 1U &&
        (laghu_hint_equal(rel, rel_length, "preload") || laghu_hint_equal(rel, rel_length, "dns-prefetch") ||
         laghu_hint_equal(rel, rel_length, "preconnect"))) {
      if (href_length + 2U >= capacity - used) {
        output[0] = '\0';
        return;
      }
      memcpy(output + used, href, href_length);
      used += href_length;
      output[used++] = '\n';
      output[used] = '\0';
    }
    cursor = end + 1U;
  }
}

static bool laghu_hint_add(laghu_runtime_html_result *result, const char *existing, const char *target, const char *relation) {
  char value[LAGHU_HTML_HEADER_VALUE_SIZE];
  char *owned;
  int length;
  if (result->link_header_count == LAGHU_HTML_MAX_LINK_HEADERS || laghu_hint_existing_seen(existing, target) ||
      laghu_hint_added(result, target, relation)) {
    return true;
  }
  length = snprintf(value, sizeof(value), "<%s>; %s", target, relation);
  if (length <= 0 || (size_t)length >= LAGHU_HTML_HEADER_VALUE_SIZE) {
    return false;
  }
  owned = malloc((size_t)length + 1U);
  if (owned == NULL) {
    return false;
  }
  memcpy(owned, value, (size_t)length + 1U);
  result->link_headers[result->link_header_count] = owned;
  ++result->link_header_count;
  return true;
}

static bool laghu_hint_add_third_party(laghu_runtime_html_result *result, const char *existing, const char *target, unsigned int *preconnect_count,
                                       unsigned int *dns_count) {
  unsigned int before;
  if (laghu_hint_existing_seen(existing, target)) {
    return true;
  }
  if (*preconnect_count < LAGHU_HTML_MAX_PRECONNECT) {
    before = result->link_header_count;
    if (!laghu_hint_add(result, existing, target, "rel=preconnect")) {
      return false;
    }
    if (result->link_header_count != before) {
      ++*preconnect_count;
    }
  }
  if (*dns_count < LAGHU_HTML_MAX_DNS_PREFETCH) {
    before = result->link_header_count;
    if (!laghu_hint_add(result, existing, target, "rel=dns-prefetch")) {
      return false;
    }
    if (result->link_header_count != before) {
      ++*dns_count;
    }
  }
  return true;
}

static bool laghu_hint_ready_image(const char *cache_path, const char *url, const char *policy_key, uint32_t capability_mask, uint64_t now,
                                   unsigned int ttl_seconds, char *target, const char **type) {
  laghu_catalog_record record;
  unsigned int index;
  if (!laghu_catalog_lookup_url(cache_path, url, policy_key, capability_mask, now, ttl_seconds, &record)) {
    return false;
  }
  for (index = 0U; index < record.variant_count; ++index) {
    laghu_catalog_variant *variant = &record.variants[index];
    laghu_runtime_cache_entry entry;
    if (variant->ready && !variant->terminally_excluded && laghu_runtime_cache_lookup_variant(cache_path, variant->variant_key, &entry) &&
        laghu_image_variant_url(variant->variant_key, target)) {
      *type = variant->content_type[0] != '\0' ? variant->content_type : NULL;
      return true;
    }
  }
  return false;
}

bool laghu_runtime_finalize_html_headers(const char *cache_path, laghu_buffer html, const char *page_path, const char *page_origin,
                                         const char *policy_key, uint32_t capability_mask, uint64_t now, unsigned int ttl_seconds,
                                         laghu_html_planner_mask plan, const char *existing_content_language, const char *existing_link_headers,
                                         unsigned int css_inline_limit, unsigned int css_outline_threshold, bool already_warm,
                                         laghu_runtime_html_result *result) {
  laghu_hint_builder builder = {0};
  size_t cursor = 0U;
  unsigned int tokens = 0U;
  unsigned int head_depth = 0U;
  unsigned int preload_count = 0U;
  unsigned int preconnect_count = 0U;
  unsigned int dns_count = 0U;
  unsigned int image_index = 0U;
  bool changed = false;
  bool invalid = false;
  char language[LAGHU_HTML_LANGUAGE_SIZE] = {0};
  char all_existing_links[16384U];
  char markup_links[8192U];
  if (result == NULL || cache_path == NULL || policy_key == NULL || html.length > LAGHU_IMAGE_MAX_INPUT_BYTES) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  if (!laghu_hint_link_headers_valid(existing_link_headers)) {
    plan &= ~LAGHU_HTML_PLAN_RESOURCE_HINTS;
    existing_link_headers = NULL;
  }
  laghu_hint_existing_markup(html, markup_links, sizeof(markup_links));
  {
    int combined =
        snprintf(all_existing_links, sizeof(all_existing_links), "%s\n%s", existing_link_headers == NULL ? "" : existing_link_headers, markup_links);
    if (combined < 0 || (size_t)combined >= sizeof(all_existing_links)) {
      all_existing_links[0] = '\0';
    }
  }
  if (all_existing_links[0] == '\0' && existing_link_headers != NULL && existing_link_headers[0] != '\0') {
    return false;
  }
  existing_link_headers = all_existing_links;
  while (cursor < html.length) {
    const unsigned char *open = memchr(html.data + cursor, '<', html.length - cursor);
    size_t start;
    size_t end;
    const unsigned char *name;
    size_t name_length;
    bool closing;
    if (open == NULL) {
      if (!laghu_hint_append(&builder, html.data + cursor, html.length - cursor)) {
        invalid = true;
      }
      break;
    }
    start = (size_t)(open - html.data);
    if (!laghu_hint_append(&builder, html.data + cursor, start - cursor)) {
      invalid = true;
      break;
    }
    if (start + 4U <= html.length && memcmp(html.data + start, "<!--", 4U) == 0) {
      const unsigned char *close = NULL;
      size_t scan;
      for (scan = start + 4U; scan + 3U <= html.length; ++scan) {
        if (memcmp(html.data + scan, "-->", 3U) == 0) {
          close = html.data + scan;
          break;
        }
      }
      if (close == NULL || !laghu_hint_append(&builder, html.data + start, (size_t)(close - html.data) + 3U - start)) {
        invalid = true;
        break;
      }
      cursor = (size_t)(close - html.data) + 3U;
      continue;
    }
    end = start + 1U;
    {
      unsigned char quote = 0U;
      for (; end < html.length; ++end) {
        if (quote != 0U) {
          if (html.data[end] == quote) {
            quote = 0U;
          }
        } else if (html.data[end] == '\'' || html.data[end] == '"') {
          quote = html.data[end];
        } else if (html.data[end] == '>') {
          break;
        }
      }
      if (end == html.length || quote != 0U) {
        invalid = true;
        break;
      }
    }
    if (++tokens > LAGHU_HTML_MAX_TOKENS) {
      invalid = true;
      break;
    }
    closing = start + 1U < end && html.data[start + 1U] == '/';
    name = html.data + start + (closing ? 2U : 1U);
    name_length = 0U;
    while (name + name_length < html.data + end && (isalnum(name[name_length]) || name[name_length] == '-')) {
      ++name_length;
    }
    if (closing && laghu_hint_equal(name, name_length, "head")) {
      if (head_depth == 0U) {
        invalid = true;
        break;
      }
      --head_depth;
    }
    if (!closing && laghu_hint_equal(name, name_length, "head")) {
      ++head_depth;
    }
    if (!closing && head_depth > 0U && (plan & LAGHU_HTML_PLAN_CONVERT_META_TAGS) != 0U && laghu_hint_equal(name, name_length, "meta")) {
      const unsigned char *http_equiv;
      const unsigned char *content;
      size_t http_length;
      size_t content_length;
      unsigned int http_matches;
      unsigned int content_matches;
      if (!laghu_hint_attribute(html.data + start, end - start + 1U, "http-equiv", &http_equiv, &http_length, &http_matches) ||
          !laghu_hint_attribute(html.data + start, end - start + 1U, "content", &content, &content_length, &content_matches)) {
        invalid = true;
        break;
      }
      if (http_matches > 1U || content_matches > 1U) {
        invalid = true;
        break;
      }
      if (http_matches == 1U && content_matches == 1U && laghu_hint_equal(http_equiv, http_length, "content-language")) {
        char current[LAGHU_HTML_LANGUAGE_SIZE];
        if (!laghu_hint_language(content, content_length, current) || (language[0] != '\0' && !laghu_hint_case_equal(language, current)) ||
            (existing_content_language != NULL && existing_content_language[0] != '\0' &&
             !laghu_hint_case_equal(existing_content_language, current))) {
          invalid = true;
          break;
        }
        memcpy(language, current, sizeof(language));
        changed = true;
        cursor = end + 1U;
        continue;
      }
    }
    if (!closing && (plan & LAGHU_HTML_PLAN_RESOURCE_HINTS) != 0U) {
      const char *attributes[] = {"src", "href", "poster"};
      size_t attribute_index;
      bool resource_tag = laghu_hint_equal(name, name_length, "script") || laghu_hint_equal(name, name_length, "link") ||
                          laghu_hint_equal(name, name_length, "img") || laghu_hint_equal(name, name_length, "source") ||
                          laghu_hint_equal(name, name_length, "video") || laghu_hint_equal(name, name_length, "audio") ||
                          laghu_hint_equal(name, name_length, "iframe");
      if (laghu_hint_equal(name, name_length, "img")) {
        ++image_index;
      }
      for (attribute_index = 0U; resource_tag && attribute_index < 3U; ++attribute_index) {
        const unsigned char *url;
        size_t url_length;
        unsigned int matches;
        char origin[LAGHU_RUNTIME_PATH_SIZE];
        if (!laghu_hint_attribute(html.data + start, end - start + 1U, attributes[attribute_index], &url, &url_length, &matches)) {
          invalid = true;
          break;
        }
        if (matches == 1U && (preconnect_count < LAGHU_HTML_MAX_PRECONNECT || dns_count < LAGHU_HTML_MAX_DNS_PREFETCH) &&
            laghu_hint_origin(url, url_length, page_origin, origin)) {
          if (!laghu_hint_add_third_party(result, existing_link_headers, origin, &preconnect_count, &dns_count)) {
            invalid = true;
            break;
          }
        }
      }
      if (resource_tag && (preconnect_count < LAGHU_HTML_MAX_PRECONNECT || dns_count < LAGHU_HTML_MAX_DNS_PREFETCH) &&
          (laghu_hint_equal(name, name_length, "img") || laghu_hint_equal(name, name_length, "source"))) {
        const unsigned char *srcset;
        size_t srcset_length;
        unsigned int srcset_matches;
        if (!laghu_hint_attribute(html.data + start, end - start + 1U, "srcset", &srcset, &srcset_length, &srcset_matches)) {
          invalid = true;
          break;
        }
        if (srcset_matches == 1U) {
          size_t item = 0U;
          while (item < srcset_length && dns_count < LAGHU_HTML_MAX_DNS_PREFETCH) {
            size_t url_start;
            size_t url_end;
            char origin[LAGHU_RUNTIME_PATH_SIZE];
            while (item < srcset_length && (isspace(srcset[item]) || srcset[item] == ',')) {
              ++item;
            }
            url_start = item;
            while (item < srcset_length && !isspace(srcset[item]) && srcset[item] != ',') {
              ++item;
            }
            url_end = item;
            while (item < srcset_length && srcset[item] != ',') {
              ++item;
            }
            if (url_end > url_start && laghu_hint_origin(srcset + url_start, url_end - url_start, page_origin, origin)) {
              if (!laghu_hint_add_third_party(result, existing_link_headers, origin, &preconnect_count, &dns_count)) {
                invalid = true;
                break;
              }
            }
          }
        }
      }
      if (invalid) {
        break;
      }
      if (preload_count < LAGHU_HTML_MAX_PRELOADS && laghu_hint_equal(name, name_length, "link")) {
        const unsigned char *rel;
        const unsigned char *href;
        size_t rel_length;
        size_t href_length;
        unsigned int rel_matches;
        unsigned int href_matches;
        if (!laghu_hint_attribute(html.data + start, end - start + 1U, "rel", &rel, &rel_length, &rel_matches) ||
            !laghu_hint_attribute(html.data + start, end - start + 1U, "href", &href, &href_length, &href_matches)) {
          invalid = true;
          break;
        }
        if (rel_matches == 1U && href_matches == 1U && laghu_hint_equal(rel, rel_length, "stylesheet")) {
          char normalized[LAGHU_RUNTIME_PATH_SIZE];
          bool same_origin;
          laghu_stylesheet_record record;
          if (laghu_hint_normalize_url(page_path, page_origin, href, href_length, normalized, &same_origin) && same_origin &&
              laghu_stylesheet_lookup(cache_path, normalized, policy_key, capability_mask, css_inline_limit, css_outline_threshold, now, ttl_seconds,
                                      &record) &&
              record.ready && record.derived_key[0] != '\0') {
            laghu_runtime_cache_entry stylesheet_entry;
            char target[sizeof("/.laghu/css/") + LAGHU_RUNTIME_KEY_SIZE];
            if (laghu_runtime_cache_lookup_variant(cache_path, record.derived_key, &stylesheet_entry) &&
                strcmp(stylesheet_entry.content_type, "text/css") == 0) {
              (void)snprintf(target, sizeof(target), "/.laghu/css/%s", record.derived_key);
              if (!laghu_hint_add(result, existing_link_headers, target, "rel=preload; as=style")) {
                invalid = true;
                break;
              }
              ++preload_count;
            }
          }
        }
      } else if (preload_count < LAGHU_HTML_MAX_PRELOADS && laghu_hint_equal(name, name_length, "img")) {
        const unsigned char *src;
        const unsigned char *priority;
        size_t src_length;
        size_t priority_length;
        unsigned int src_matches;
        unsigned int priority_matches;
        char normalized[LAGHU_RUNTIME_PATH_SIZE];
        bool same_origin;
        bool critical = image_index == 1U;
        if (!laghu_hint_attribute(html.data + start, end - start + 1U, "src", &src, &src_length, &src_matches) ||
            !laghu_hint_attribute(html.data + start, end - start + 1U, "fetchpriority", &priority, &priority_length, &priority_matches)) {
          invalid = true;
          break;
        }
        critical = critical || (priority_matches == 1U && laghu_hint_equal(priority, priority_length, "high"));
        if (src_matches == 1U && laghu_hint_normalize_url(page_path, page_origin, src, src_length, normalized, &same_origin) && same_origin) {
          laghu_catalog_record record;
          if (!critical && laghu_catalog_lookup_url(cache_path, normalized, policy_key, capability_mask, now, ttl_seconds, &record)) {
            critical = record.learned_above_fold;
          }
          if (critical) {
            char target[sizeof("/.laghu/image/") + LAGHU_RUNTIME_KEY_SIZE];
            const char *type = NULL;
            if (laghu_hint_ready_image(cache_path, normalized, policy_key, capability_mask, now, ttl_seconds, target, &type)) {
              char relation[160U];
              if (laghu_hint_mime(type)) {
                (void)snprintf(relation, sizeof(relation), "rel=preload; as=image; type=\"%s\"", type);
              } else {
                memcpy(relation, "rel=preload; as=image", sizeof("rel=preload; as=image"));
              }
              if (!laghu_hint_add(result, existing_link_headers, target, relation)) {
                invalid = true;
                break;
              }
              ++preload_count;
            }
          }
        }
      }
    }
    if (!laghu_hint_append(&builder, html.data + start, end - start + 1U)) {
      invalid = true;
      break;
    }
    cursor = end + 1U;
  }
  if (invalid || head_depth != 0U) {
    free(builder.data);
    laghu_runtime_html_result_release(result);
    result->invalid = true;
    return true;
  }
  if (language[0] != '\0' && (existing_content_language == NULL || existing_content_language[0] == '\0')) {
    memcpy(result->content_language, language, sizeof(language));
    result->set_content_language = true;
  }
  if (!changed && result->link_header_count == 0U) {
    free(builder.data);
    return true;
  }
  {
    char source_hash[LAGHU_RUNTIME_KEY_SIZE];
    char material[8192U];
    size_t material_length;
    unsigned int index;
    laghu_runtime_cache_entry entry;
    int written;
    if (!laghu_sha256_hex(html, source_hash)) {
      free(builder.data);
      laghu_runtime_html_result_release(result);
      return false;
    }
    written =
        snprintf(material, sizeof(material), "laghu-html-hints-v1\n%s\n%s\n%08x\n%s\n%s\n", source_hash, policy_key, (unsigned int)plan,
                 existing_content_language == NULL ? "" : existing_content_language, existing_link_headers == NULL ? "" : existing_link_headers);
    if (written <= 0 || (size_t)written >= sizeof(material)) {
      free(builder.data);
      laghu_runtime_html_result_release(result);
      return false;
    }
    material_length = (size_t)written;
    for (index = 0U; index < result->link_header_count; ++index) {
      size_t length = strlen(result->link_headers[index]);
      if (length + 1U > sizeof(material) - material_length) {
        free(builder.data);
        laghu_runtime_html_result_release(result);
        return false;
      }
      memcpy(material + material_length, result->link_headers[index], length);
      material_length += length;
      material[material_length++] = '\n';
    }
    if (!laghu_sha256_hex((laghu_buffer){(const unsigned char *)material, material_length}, result->dependency_key)) {
      free(builder.data);
      laghu_runtime_html_result_release(result);
      return false;
    }
    if (!already_warm && !laghu_runtime_cache_lookup_variant(cache_path, result->dependency_key, &entry)) {
      if (!laghu_runtime_cache_publish(cache_path, result->dependency_key, result->dependency_key, source_hash, "text/html", "laghu-html-hints-v1",
                                       (laghu_buffer){builder.data, builder.length}, &entry)) {
        free(builder.data);
        laghu_runtime_html_result_release(result);
        return false;
      }
      free(builder.data);
      laghu_runtime_html_result_release(result);
      result->dependencies_pending = true;
      return true;
    }
  }
  result->data = builder.data;
  result->length = builder.length;
  result->rewritten = changed;
  return true;
}
