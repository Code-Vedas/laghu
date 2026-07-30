// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/runtime.h"

#define LAGHU_FONT_CONFIG_LINE 2048U
#define LAGHU_FONT_TTL_MAX 604800U

static bool laghu_font_copy(char *output, size_t capacity, const char *input) {
  size_t length = input == NULL ? 0U : strlen(input);
  if (length == 0U || length >= capacity) return false;
  memcpy(output, input, length + 1U);
  return true;
}

static bool laghu_font_identifier(const char *value) {
  size_t index;
  if (value == NULL || *value == '\0') return false;
  for (index = 0U; value[index] != '\0'; ++index)
    if (!(value[index] >= 'a' && value[index] <= 'z') &&
        !(value[index] >= '0' && value[index] <= '9') && value[index] != '_' &&
        value[index] != '-')
      return false;
  return true;
}

static bool laghu_font_host(const char *value) {
  size_t index;
  bool label = false;
  if (value == NULL || *value == '\0' || strchr(value, '.') == NULL ||
      strpbrk(value, "*:@/\\[]") != NULL)
    return false;
  for (index = 0U; value[index] != '\0'; ++index) {
    unsigned char byte = (unsigned char)value[index];
    if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')) {
      label = true;
    } else if (byte == '-' && label && value[index + 1U] != '\0' &&
               value[index + 1U] != '.') {
      continue;
    } else if (byte == '.' && label && value[index + 1U] != '\0') {
      label = false;
    } else {
      return false;
    }
  }
  return label;
}

static bool laghu_font_prefix(const char *value) {
  size_t index;
  if (value == NULL || value[0] != '/' || strstr(value, "..") != NULL ||
      strchr(value, '?') != NULL || strchr(value, '#') != NULL ||
      strchr(value, '\\') != NULL)
    return false;
  for (index = 0U; value[index] != '\0'; ++index)
    if ((unsigned char)value[index] <= 32U ||
        (unsigned char)value[index] >= 127U)
      return false;
  return true;
}

static void laghu_font_error(char *error, size_t capacity, unsigned int line,
                             const char *message) {
  if (error != NULL && capacity != 0U)
    (void)snprintf(error, capacity, "line %u: %s", line, message);
}

static char *laghu_font_token(char **cursor) {
  char *start;
  while (**cursor != '\0' && isspace((unsigned char)**cursor)) ++*cursor;
  if (**cursor == '\0' || **cursor == '#') return NULL;
  start = *cursor;
  while (**cursor != '\0' && !isspace((unsigned char)**cursor) &&
         **cursor != '#')
    ++*cursor;
  if (**cursor != '\0') *(*cursor)++ = '\0';
  return start;
}

static bool laghu_font_rule_add(laghu_font_provider_rule *rules,
                                unsigned int *count, const char *host,
                                const char *prefix) {
  unsigned int index;
  if (*count == LAGHU_FONT_PROVIDER_RULE_MAX || !laghu_font_host(host) ||
      !laghu_font_prefix(prefix))
    return false;
  for (index = 0U; index < *count; ++index)
    if (strcmp(rules[index].host, host) == 0 &&
        strcmp(rules[index].path_prefix, prefix) == 0)
      return false;
  if (!laghu_font_copy(rules[*count].host, sizeof(rules[*count].host), host) ||
      !laghu_font_copy(rules[*count].path_prefix,
                       sizeof(rules[*count].path_prefix), prefix))
    return false;
  ++*count;
  return true;
}

static bool laghu_font_number(const char *value, unsigned int minimum,
                              unsigned int maximum, unsigned int *output) {
  char *end = NULL;
  unsigned long parsed;
  if (value == NULL || *value == '\0') return false;
  parsed = strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed < minimum || parsed > maximum)
    return false;
  *output = (unsigned int)parsed;
  return true;
}

static bool laghu_font_provider_digest(laghu_font_provider *provider) {
  char material[16384U];
  size_t length = 0U;
  unsigned int index;
#define APPEND(...)                                                          \
  do {                                                                       \
    int written =                                                            \
        snprintf(material + length, sizeof(material) - length, __VA_ARGS__); \
    if (written < 0 || (size_t)written >= sizeof(material) - length)         \
      return false;                                                          \
    length += (size_t)written;                                               \
  } while (0)
  APPEND("font-provider-v1\n%s\n%u\n%u\n", provider->id,
         provider->max_css_bytes, provider->ttl_seconds);
  for (index = 0U; index < provider->stylesheet_count; ++index)
    APPEND("s %s %s\n", provider->stylesheets[index].host,
           provider->stylesheets[index].path_prefix);
  for (index = 0U; index < provider->redirect_count; ++index)
    APPEND("r %s %s\n", provider->redirects[index].host,
           provider->redirects[index].path_prefix);
  for (index = 0U; index < provider->asset_count; ++index)
    APPEND("a %s %s\n", provider->assets[index].host,
           provider->assets[index].path_prefix);
#undef APPEND
  return laghu_sha256_hex(
      (laghu_buffer){(const unsigned char *)material, length},
      provider->digest);
}

static bool laghu_font_overlap(const laghu_font_provider_set *set) {
  unsigned int left, right, a, b;
  for (left = 0U; left < set->count; ++left)
    for (right = left + 1U; right < set->count; ++right)
      for (a = 0U; a < set->providers[left].stylesheet_count; ++a)
        for (b = 0U; b < set->providers[right].stylesheet_count; ++b) {
          const laghu_font_provider_rule *one =
              &set->providers[left].stylesheets[a];
          const laghu_font_provider_rule *two =
              &set->providers[right].stylesheets[b];
          size_t shorter;
          if (strcmp(one->host, two->host) != 0) continue;
          shorter = strlen(one->path_prefix) < strlen(two->path_prefix)
                        ? strlen(one->path_prefix)
                        : strlen(two->path_prefix);
          if (memcmp(one->path_prefix, two->path_prefix, shorter) == 0)
            return true;
        }
  return false;
}

bool laghu_font_providers_load(const char *path, laghu_font_provider_set *set,
                               char *error, size_t error_size) {
  FILE *file;
  char line[LAGHU_FONT_CONFIG_LINE];
  unsigned int line_number = 0U;
  laghu_font_provider *current = NULL;
  if (path == NULL || set == NULL || (file = fopen(path, "rb")) == NULL) {
    laghu_font_error(error, error_size, 0U, "cannot open provider config");
    return false;
  }
  memset(set, 0, sizeof(*set));
  while (fgets(line, sizeof(line), file) != NULL) {
    char *cursor = line;
    char *name, *first, *second, *extra;
    size_t length;
    ++line_number;
    length = strlen(line);
    if (length != 0U && line[length - 1U] != '\n' && !feof(file)) {
      laghu_font_error(error, error_size, line_number, "line is too long");
      goto failed;
    }
    while (length != 0U &&
           (line[length - 1U] == '\n' || line[length - 1U] == '\r'))
      line[--length] = '\0';
    name = laghu_font_token(&cursor);
    if (name == NULL) continue;
    first = laghu_font_token(&cursor);
    second = laghu_font_token(&cursor);
    extra = laghu_font_token(&cursor);
    if (strcmp(name, "provider") == 0) {
      unsigned int index;
      if (current != NULL || first == NULL || second != NULL || extra != NULL ||
          !laghu_font_identifier(first) ||
          set->count == LAGHU_FONT_PROVIDER_MAX)
        goto invalid;
      for (index = 0U; index < set->count; ++index)
        if (strcmp(set->providers[index].id, first) == 0) goto invalid;
      current = &set->providers[set->count++];
      if (!laghu_font_copy(current->id, sizeof(current->id), first))
        goto invalid;
      current->max_css_bytes = LAGHU_FONT_CSS_MAX_BYTES;
      current->ttl_seconds = LAGHU_FONT_TTL_MAX;
    } else if (strcmp(name, "end") == 0) {
      if (current == NULL || first != NULL || current->stylesheet_count == 0U ||
          current->asset_count == 0U || !laghu_font_provider_digest(current))
        goto invalid;
      current = NULL;
    } else if (current == NULL) {
      goto invalid;
    } else if (strcmp(name, "stylesheet") == 0) {
      if (first == NULL || second == NULL || extra != NULL ||
          !laghu_font_rule_add(current->stylesheets, &current->stylesheet_count,
                               first, second))
        goto invalid;
    } else if (strcmp(name, "redirect") == 0) {
      if (first == NULL || second == NULL || extra != NULL ||
          !laghu_font_rule_add(current->redirects, &current->redirect_count,
                               first, second))
        goto invalid;
    } else if (strcmp(name, "asset") == 0) {
      if (first == NULL || second == NULL || extra != NULL ||
          !laghu_font_rule_add(current->assets, &current->asset_count, first,
                               second))
        goto invalid;
    } else if (strcmp(name, "max_css_bytes") == 0) {
      if (second != NULL || extra != NULL ||
          !laghu_font_number(first, 1024U, LAGHU_FONT_CSS_MAX_BYTES,
                             &current->max_css_bytes))
        goto invalid;
    } else if (strcmp(name, "ttl_seconds") == 0) {
      if (second != NULL || extra != NULL ||
          !laghu_font_number(first, 60U, LAGHU_FONT_TTL_MAX,
                             &current->ttl_seconds))
        goto invalid;
    } else {
      goto invalid;
    }
    continue;
  invalid:
    laghu_font_error(error, error_size, line_number,
                     "invalid or unknown provider directive");
    goto failed;
  }
  if (ferror(file) || current != NULL || set->count == 0U ||
      laghu_font_overlap(set)) {
    laghu_font_error(error, error_size, line_number,
                     current != NULL ? "unterminated provider"
                                     : "empty or overlapping provider config");
    goto failed;
  }
  fclose(file);
  {
    char material[LAGHU_FONT_PROVIDER_MAX * LAGHU_RUNTIME_KEY_SIZE];
    size_t length = 0U;
    unsigned int index;
    for (index = 0U; index < set->count; ++index) {
      memcpy(material + length, set->providers[index].digest,
             LAGHU_RUNTIME_KEY_SIZE - 1U);
      length += LAGHU_RUNTIME_KEY_SIZE - 1U;
    }
    return laghu_sha256_hex(
        (laghu_buffer){(const unsigned char *)material, length}, set->digest);
  }
failed:
  fclose(file);
  memset(set, 0, sizeof(*set));
  return false;
}

static bool laghu_font_parse_url(const char *url, char *host,
                                 size_t host_capacity, const char **path) {
  const char *start;
  const char *slash;
  size_t length;
  if (url == NULL || strncmp(url, "https://", 8U) != 0) return false;
  start = url + 8U;
  slash = strchr(start, '/');
  if (slash == NULL) return false;
  length = (size_t)(slash - start);
  if (length == 0U || length >= host_capacity || memchr(start, ':', length))
    return false;
  if (memchr(start, '@', length) != NULL || strchr(slash, '#') != NULL)
    return false;
  memcpy(host, start, length);
  host[length] = '\0';
  if (!laghu_font_host(host)) return false;
  *path = slash;
  return true;
}

static bool laghu_font_rules_match(const laghu_font_provider_rule *rules,
                                   unsigned int count, const char *url) {
  char host[LAGHU_FONT_PROVIDER_HOST_SIZE];
  const char *path;
  unsigned int index;
  if (!laghu_font_parse_url(url, host, sizeof(host), &path)) return false;
  for (index = 0U; index < count; ++index) {
    size_t prefix = strlen(rules[index].path_prefix);
    if (strcmp(host, rules[index].host) == 0 &&
        strncmp(path, rules[index].path_prefix, prefix) == 0 &&
        (rules[index].path_prefix[prefix - 1U] == '/' || path[prefix] == '\0' ||
         path[prefix] == '?' || path[prefix] == '/'))
      return true;
  }
  return false;
}

const laghu_font_provider *laghu_font_provider_match(
    const laghu_font_provider_set *set, const char *url) {
  unsigned int index;
  if (set == NULL) return NULL;
  for (index = 0U; index < set->count; ++index)
    if (laghu_font_rules_match(set->providers[index].stylesheets,
                               set->providers[index].stylesheet_count, url))
      return &set->providers[index];
  return NULL;
}

const laghu_font_provider *laghu_font_provider_by_id(
    const laghu_font_provider_set *set, const char *id) {
  unsigned int index;
  if (set == NULL || id == NULL) return NULL;
  for (index = 0U; index < set->count; ++index)
    if (strcmp(set->providers[index].id, id) == 0)
      return &set->providers[index];
  return NULL;
}

bool laghu_font_provider_url_allowed(const laghu_font_provider *provider,
                                     const char *url, bool redirect,
                                     bool asset) {
  if (provider == NULL || (redirect && asset)) return false;
  if (asset)
    return laghu_font_rules_match(provider->assets, provider->asset_count, url);
  if (redirect)
    return laghu_font_rules_match(provider->redirects, provider->redirect_count,
                                  url) ||
           laghu_font_rules_match(provider->stylesheets,
                                  provider->stylesheet_count, url);
  return laghu_font_rules_match(provider->stylesheets,
                                provider->stylesheet_count, url);
}

bool laghu_font_stylesheet_key(const char *url, const char *provider_digest,
                               char output[LAGHU_RUNTIME_KEY_SIZE]) {
  char material[LAGHU_RUNTIME_PATH_SIZE + LAGHU_RUNTIME_KEY_SIZE + 64U];
  int length;
  if (url == NULL || provider_digest == NULL || output == NULL) return false;
  length = snprintf(material, sizeof(material), "font-css-v%u\n%s\n%s",
                    LAGHU_FONT_FETCH_PROFILE_VERSION, provider_digest, url);
  return length > 0 && (size_t)length < sizeof(material) &&
         laghu_sha256_hex(
             (laghu_buffer){(const unsigned char *)material, (size_t)length},
             output);
}

static bool laghu_font_css_equal(const unsigned char *value, size_t available,
                                 const char *expected) {
  size_t index, length = strlen(expected);
  if (available < length) return false;
  for (index = 0U; index < length; ++index)
    if (tolower(value[index]) != (unsigned char)expected[index]) return false;
  return true;
}

static bool laghu_font_css_comment(laghu_buffer css, size_t *cursor) {
  if (*cursor + 2U > css.length || css.data[*cursor] != '/' ||
      css.data[*cursor + 1U] != '*')
    return false;
  *cursor += 2U;
  while (*cursor + 1U < css.length &&
         !(css.data[*cursor] == '*' && css.data[*cursor + 1U] == '/'))
    ++*cursor;
  if (*cursor + 1U >= css.length) return false;
  *cursor += 2U;
  return true;
}

bool laghu_font_css_validate(const laghu_font_provider *provider,
                             laghu_buffer css) {
  size_t cursor = 0U;
  unsigned int faces = 0U;
  if (provider == NULL || css.data == NULL || css.length == 0U ||
      css.length > provider->max_css_bytes)
    return false;
  while (cursor < css.length) {
    while (cursor < css.length && isspace(css.data[cursor])) ++cursor;
    if (cursor == css.length) break;
    if (cursor + 2U <= css.length && css.data[cursor] == '/' &&
        css.data[cursor + 1U] == '*') {
      if (!laghu_font_css_comment(css, &cursor)) return false;
      continue;
    }
    if (!laghu_font_css_equal(css.data + cursor, css.length - cursor,
                              "@font-face"))
      return false;
    cursor += 10U;
    while (cursor < css.length && isspace(css.data[cursor])) ++cursor;
    if (cursor == css.length || css.data[cursor++] != '{') return false;
    ++faces;
    for (;;) {
      if (cursor >= css.length || css.data[cursor] == '\0') return false;
      if (css.data[cursor] == '}') {
        ++cursor;
        break;
      }
      if (cursor + 2U <= css.length && css.data[cursor] == '/' &&
          css.data[cursor + 1U] == '*') {
        if (!laghu_font_css_comment(css, &cursor)) return false;
        continue;
      }
      if (css.data[cursor] == '\'' || css.data[cursor] == '"') {
        unsigned char quote = css.data[cursor++];
        while (cursor < css.length && css.data[cursor] != quote) {
          if (css.data[cursor] == '\\') {
            if (++cursor == css.length) return false;
          }
          ++cursor;
        }
        if (cursor == css.length) return false;
        ++cursor;
        continue;
      }
      if (css.data[cursor] == '{') return false;
      if (laghu_font_css_equal(css.data + cursor, css.length - cursor,
                               "expression(") ||
          laghu_font_css_equal(css.data + cursor, css.length - cursor,
                               "javascript:"))
        return false;
      if (laghu_font_css_equal(css.data + cursor, css.length - cursor,
                               "url(")) {
        size_t start, end;
        char url[LAGHU_RUNTIME_PATH_SIZE];
        cursor += 4U;
        while (cursor < css.length && isspace(css.data[cursor])) ++cursor;
        start = cursor;
        if (cursor < css.length &&
            (css.data[cursor] == '\'' || css.data[cursor] == '"')) {
          unsigned char quote = css.data[cursor++];
          start = cursor;
          while (cursor < css.length && css.data[cursor] != quote) {
            if (css.data[cursor] == '\\') return false;
            ++cursor;
          }
          if (cursor == css.length) return false;
          end = cursor++;
        } else {
          while (cursor < css.length && css.data[cursor] != ')' &&
                 !isspace(css.data[cursor])) {
            if (css.data[cursor] == '\\') return false;
            ++cursor;
          }
          end = cursor;
        }
        while (cursor < css.length && isspace(css.data[cursor])) ++cursor;
        if (cursor == css.length || css.data[cursor++] != ')' || end == start ||
            end - start >= sizeof(url))
          return false;
        memcpy(url, css.data + start, end - start);
        url[end - start] = '\0';
        if (!laghu_font_provider_url_allowed(provider, url, false, true))
          return false;
        continue;
      }
      ++cursor;
    }
  }
  return faces != 0U;
}

bool laghu_font_stylesheet_publish(const char *cache_path,
                                   const laghu_font_stylesheet_record *record) {
  char key[LAGHU_RUNTIME_KEY_SIZE];
  laghu_runtime_cache_entry entry;
  if (record == NULL ||
      !laghu_font_stylesheet_key(record->normalized_url,
                                 record->provider_digest, key))
    return false;
  return laghu_runtime_cache_publish(
      cache_path, key, key, key, "application/x-laghu-font-catalog",
      "laghu-font-catalog-v1",
      (laghu_buffer){(const unsigned char *)record, sizeof(*record)}, &entry);
}

bool laghu_font_stylesheet_lookup(const char *cache_path, const char *url,
                                  const laghu_font_provider *provider,
                                  uint64_t now,
                                  laghu_font_stylesheet_record *record) {
  char key[LAGHU_RUNTIME_KEY_SIZE];
  laghu_runtime_cache_entry entry;
  if (record == NULL || provider == NULL ||
      !laghu_font_stylesheet_key(url, provider->digest, key) ||
      !laghu_runtime_cache_lookup_variant(cache_path, key, &entry) ||
      entry.length != sizeof(*record) ||
      !laghu_runtime_cache_read(&entry, (unsigned char *)record,
                                sizeof(*record)) ||
      strcmp(record->provider_id, provider->id) != 0 ||
      strcmp(record->provider_digest, provider->digest) != 0 ||
      strcmp(record->normalized_url, url) != 0)
    return false;
  if (record->ready && (record->fetched_at > now ||
                        now - record->fetched_at > record->ttl_seconds))
    record->ready = false;
  return true;
}
