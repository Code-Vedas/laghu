// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "html_internal.h"

static bool laghu_html_comment_preserved(const unsigned char *data,
                                         size_t length) {
  return memchr(data, '!', length) != NULL ||
         laghu_html_contains_ci(data, length, "[if") ||
         laghu_html_contains_ci(data, length, "[endif]") ||
         laghu_html_contains_ci(data, length, "@license") ||
         laghu_html_contains_ci(data, length, "@preserve") ||
         laghu_html_contains_ci(data, length, "laghu:keep") ||
         laghu_html_contains_ci(data, length, "sourceMappingURL") ||
         laghu_html_contains_ci(data, length, "sourceURL");
}

static bool laghu_html_unquoted_safe(const unsigned char *value,
                                     size_t length) {
  size_t index;
  if (length == 0U) {
    return false;
  }
  for (index = 0U; index < length; ++index) {
    unsigned char byte = value[index];
    if (laghu_html_ascii_space(byte) || byte == '"' || byte == '\'' ||
        byte == '`' || byte == '=' || byte == '<' || byte == '>') {
      return false;
    }
  }
  return true;
}

static bool laghu_html_parse_tag(laghu_buffer html, size_t start, size_t end,
                                 char tag_name[16U], bool *closing,
                                 bool *self_closing,
                                 laghu_html_attribute *attributes,
                                 size_t *attribute_count) {
  size_t cursor = start + 1U;
  size_t name_start;
  size_t name_end;
  size_t count = 0U;
  *closing = false;
  *self_closing = false;
  *attribute_count = 0U;
  tag_name[0] = '\0';
  while (cursor < end && laghu_html_ascii_space(html.data[cursor])) {
    ++cursor;
  }
  if (cursor < end && html.data[cursor] == '/') {
    *closing = true;
    ++cursor;
  }
  name_start = cursor;
  while (cursor < end &&
         (isalnum(html.data[cursor]) || html.data[cursor] == '-' ||
          html.data[cursor] == '_')) {
    ++cursor;
  }
  name_end = cursor;
  if (name_end == name_start || name_end - name_start >= 16U) {
    return false;
  }
  for (cursor = 0U; cursor < name_end - name_start; ++cursor) {
    tag_name[cursor] =
        (char)tolower((unsigned char)html.data[name_start + cursor]);
  }
  tag_name[name_end - name_start] = '\0';
  cursor = name_end;
  if (*closing) {
    while (cursor < end && laghu_html_ascii_space(html.data[cursor])) {
      ++cursor;
    }
    return cursor == end;
  }
  while (cursor < end) {
    laghu_html_attribute *attribute;
    size_t previous;
    size_t leading = cursor;
    size_t index;
    while (cursor < end && laghu_html_ascii_space(html.data[cursor])) {
      ++cursor;
    }
    if (cursor == end) {
      break;
    }
    if (html.data[cursor] == '/' && cursor + 1U == end) {
      *self_closing = true;
      break;
    }
    if (count == LAGHU_HTML_MAX_ATTRIBUTES) {
      return false;
    }
    attribute = &attributes[count];
    memset(attribute, 0, sizeof(*attribute));
    attribute->leading_start = leading;
    attribute->name_start = cursor;
    while (cursor < end &&
           (isalnum(html.data[cursor]) || html.data[cursor] == '-' ||
            html.data[cursor] == '_' || html.data[cursor] == ':')) {
      ++cursor;
    }
    attribute->name_end = cursor;
    if (attribute->name_end == attribute->name_start ||
        attribute->name_end - attribute->name_start >=
            sizeof(attribute->name)) {
      return false;
    }
    for (index = 0U; index < attribute->name_end - attribute->name_start;
         ++index) {
      attribute->name[index] = (char)tolower(
          (unsigned char)html.data[attribute->name_start + index]);
    }
    for (previous = 0U; previous < count; ++previous) {
      if (strcmp(attributes[previous].name, attribute->name) == 0) {
        return false;
      }
    }
    while (cursor < end && laghu_html_ascii_space(html.data[cursor])) {
      ++cursor;
    }
    if (cursor < end && html.data[cursor] == '=') {
      attribute->has_value = true;
      ++cursor;
      while (cursor < end && laghu_html_ascii_space(html.data[cursor])) {
        ++cursor;
      }
      if (cursor == end) {
        return false;
      }
      if (html.data[cursor] == '\'' || html.data[cursor] == '"') {
        attribute->quote = html.data[cursor++];
        attribute->value_start = cursor;
        while (cursor < end && html.data[cursor] != attribute->quote) {
          ++cursor;
        }
        if (cursor == end) {
          return false;
        }
        attribute->value_end = cursor++;
      } else {
        attribute->value_start = cursor;
        while (cursor < end && !laghu_html_ascii_space(html.data[cursor]) &&
               html.data[cursor] != '>') {
          if (html.data[cursor] == '\'' || html.data[cursor] == '"' ||
              html.data[cursor] == '<' || html.data[cursor] == '=' ||
              html.data[cursor] == '`') {
            return false;
          }
          ++cursor;
        }
        attribute->value_end = cursor;
        if (attribute->value_end == attribute->value_start) {
          return false;
        }
      }
    }
    attribute->end = cursor;
    ++count;
  }
  *attribute_count = count;
  return true;
}

static const laghu_html_attribute *laghu_html_find_attribute(
    const laghu_html_attribute *attributes, size_t count, const char *name) {
  size_t index;
  for (index = 0U; index < count; ++index) {
    if (strcmp(attributes[index].name, name) == 0) {
      return &attributes[index];
    }
  }
  return NULL;
}

static bool laghu_html_attribute_equals(laghu_buffer html,
                                        const laghu_html_attribute *attribute,
                                        const char *value) {
  return attribute != NULL && attribute->has_value &&
         laghu_head_equal(html.data + attribute->value_start,
                          attribute->value_end - attribute->value_start, value);
}

static bool laghu_html_url_char_safe(unsigned char value) {
  return value > 0x20U && value != 0x7fU && value != '\\' && value != '&';
}

static bool laghu_html_url_copy(char *output, size_t capacity,
                                const unsigned char *value, size_t length) {
  size_t index;
  if (length == 0U || length >= capacity) return false;
  for (index = 0U; index < length; ++index) {
    if (!laghu_html_url_char_safe(value[index])) return false;
  }
  memcpy(output, value, length);
  output[length] = '\0';
  return true;
}

static bool laghu_html_bytes_equal_ci(const char *left, const char *right,
                                      size_t length) {
  size_t index;
  for (index = 0U; index < length; ++index) {
    if (tolower((unsigned char)left[index]) !=
        tolower((unsigned char)right[index]))
      return false;
  }
  return true;
}

static bool laghu_html_origin_equal(const char *left, const char *right) {
  const char *left_scheme = strstr(left, "://");
  const char *right_scheme = strstr(right, "://");
  const char *left_host;
  const char *right_host;
  const char *left_port = NULL;
  const char *right_port = NULL;
  size_t left_scheme_length;
  size_t right_scheme_length;
  size_t left_host_length;
  size_t right_host_length;
  unsigned long left_port_number;
  unsigned long right_port_number;
  char *end;
  if (left_scheme == NULL || right_scheme == NULL) return false;
  left_scheme_length = (size_t)(left_scheme - left);
  right_scheme_length = (size_t)(right_scheme - right);
  if (left_scheme_length != right_scheme_length ||
      !laghu_html_bytes_equal_ci(left, right, left_scheme_length))
    return false;
  left_host = left_scheme + 3U;
  right_host = right_scheme + 3U;
  if (*left_host == '[') {
    const char *close = strchr(left_host, ']');
    if (close == NULL) return false;
    left_host_length = (size_t)(close - left_host) + 1U;
    if (close[1] == ':')
      left_port = close + 2U;
    else if (close[1] != '\0')
      return false;
  } else {
    const char *colon = strrchr(left_host, ':');
    left_host_length =
        colon == NULL ? strlen(left_host) : (size_t)(colon - left_host);
    if (colon != NULL) left_port = colon + 1U;
  }
  if (*right_host == '[') {
    const char *close = strchr(right_host, ']');
    if (close == NULL) return false;
    right_host_length = (size_t)(close - right_host) + 1U;
    if (close[1] == ':')
      right_port = close + 2U;
    else if (close[1] != '\0')
      return false;
  } else {
    const char *colon = strrchr(right_host, ':');
    right_host_length =
        colon == NULL ? strlen(right_host) : (size_t)(colon - right_host);
    if (colon != NULL) right_port = colon + 1U;
  }
  if (left_host_length != right_host_length ||
      !laghu_html_bytes_equal_ci(left_host, right_host, left_host_length))
    return false;
  left_port_number = left_scheme_length == 5U ? 443U : 80U;
  right_port_number = right_scheme_length == 5U ? 443U : 80U;
  if (left_port != NULL) {
    left_port_number = strtoul(left_port, &end, 10);
    if (*left_port == '\0' || *end != '\0' || left_port_number > 65535U)
      return false;
  }
  if (right_port != NULL) {
    right_port_number = strtoul(right_port, &end, 10);
    if (*right_port == '\0' || *end != '\0' || right_port_number > 65535U)
      return false;
  }
  return left_port_number == right_port_number;
}

static bool laghu_html_url_normalize_path(const char *input, char *output,
                                          size_t capacity) {
  size_t suffix = strcspn(input, "?#");
  size_t cursor = 0U;
  size_t length = 1U;
  size_t segment_starts[LAGHU_RUNTIME_PATH_SIZE / 2U];
  size_t segment_count = 0U;
  bool trailing = suffix > 1U && input[suffix - 1U] == '/';
  if (input[0] != '/' || suffix >= capacity) return false;
  for (cursor = 1U; cursor < suffix; ++cursor) {
    if (input[cursor] == '/' && input[cursor - 1U] == '/') return false;
  }
  cursor = 0U;
  output[0] = '/';
  while (cursor < suffix) {
    size_t start;
    size_t part_length;
    while (cursor < suffix && input[cursor] == '/') ++cursor;
    if (cursor == suffix) break;
    start = cursor;
    while (cursor < suffix && input[cursor] != '/') ++cursor;
    part_length = cursor - start;
    if (part_length == 1U && input[start] == '.') continue;
    if (part_length == 2U && input[start] == '.' && input[start + 1U] == '.') {
      if (segment_count != 0U) length = segment_starts[--segment_count];
      continue;
    }
    if (segment_count >= sizeof(segment_starts) / sizeof(segment_starts[0]) ||
        length + part_length + 1U >= capacity)
      return false;
    segment_starts[segment_count++] = length;
    if (length > 1U) output[length++] = '/';
    memcpy(output + length, input + start, part_length);
    length += part_length;
  }
  if (trailing && length > 1U && output[length - 1U] != '/')
    output[length++] = '/';
  if (strlen(input + suffix) > capacity - length - 1U) return false;
  memcpy(output + length, input + suffix, strlen(input + suffix) + 1U);
  return true;
}

static bool laghu_html_url_resolve(const laghu_html_url_base *base,
                                   const unsigned char *value, size_t value_len,
                                   char origin[LAGHU_RUNTIME_PATH_SIZE],
                                   char path[LAGHU_RUNTIME_PATH_SIZE]) {
  char input[LAGHU_RUNTIME_PATH_SIZE];
  char joined[LAGHU_RUNTIME_PATH_SIZE];
  const char *path_value = NULL;
  const char *scheme;
  const char *authority_end;
  size_t origin_length;
  size_t base_path_length;
  size_t directory_length;
  if (base == NULL || !base->enabled ||
      !laghu_html_url_copy(input, sizeof(input), value, value_len))
    return false;
  scheme = strstr(input, "://");
  if (scheme != NULL) {
    size_t scheme_length = (size_t)(scheme - input);
    if (!((scheme_length == 4U &&
           laghu_head_equal((const unsigned char *)input, 4U, "http")) ||
          (scheme_length == 5U &&
           laghu_head_equal((const unsigned char *)input, 5U, "https"))))
      return false;
    authority_end = strpbrk(scheme + 3U, "/?#");
    origin_length =
        authority_end == NULL ? strlen(input) : (size_t)(authority_end - input);
    if (origin_length >= LAGHU_RUNTIME_PATH_SIZE ||
        memchr(scheme + 3U, '@',
               origin_length - (size_t)(scheme + 3U - input)) != NULL)
      return false;
    memcpy(origin, input, origin_length);
    origin[origin_length] = '\0';
    path_value = authority_end == NULL ? "/" : authority_end;
    if (*path_value == '?' || *path_value == '#') {
      if (strlen(path_value) + 1U >= sizeof(joined)) return false;
      joined[0] = '/';
      strcpy(joined + 1U, path_value);
      path_value = joined;
    }
  } else if (input[0] == '/' && input[1] == '/') {
    const char *origin_scheme = strstr(base->origin, "://");
    authority_end = strpbrk(input + 2U, "/?#");
    if (origin_scheme == NULL) return false;
    origin_length = (size_t)(origin_scheme - base->origin) + 1U;
    if (authority_end == NULL) authority_end = input + strlen(input);
    if (origin_length + (size_t)(authority_end - input) >=
        LAGHU_RUNTIME_PATH_SIZE)
      return false;
    memcpy(origin, base->origin, origin_length);
    memcpy(origin + origin_length, input, (size_t)(authority_end - input));
    origin[origin_length + (size_t)(authority_end - input)] = '\0';
    path_value = *authority_end == '\0' ? "/" : authority_end;
  } else {
    strcpy(origin, base->origin);
    if (input[0] == '/') {
      path_value = input;
    } else if (input[0] == '?' || input[0] == '#') {
      base_path_length = strcspn(base->path, "?#");
      if (base_path_length + strlen(input) >= sizeof(joined)) return false;
      memcpy(joined, base->path, base_path_length);
      strcpy(joined + base_path_length, input);
      path_value = joined;
    } else {
      base_path_length = strcspn(base->path, "?#");
      directory_length = base_path_length;
      while (directory_length > 0U && base->path[directory_length - 1U] != '/')
        --directory_length;
      if (directory_length + strlen(input) >= sizeof(joined)) return false;
      memcpy(joined, base->path, directory_length);
      strcpy(joined + directory_length, input);
      path_value = joined;
    }
  }
  return laghu_html_url_normalize_path(path_value, path,
                                       LAGHU_RUNTIME_PATH_SIZE);
}

static bool laghu_html_url_relative(const char *base_path,
                                    const char *target_path, char *output,
                                    size_t capacity) {
  size_t base_length = strcspn(base_path, "?#");
  size_t target_length = strcspn(target_path, "?#");
  size_t directory_length = base_length;
  size_t common = 1U;
  size_t cursor;
  size_t length = 0U;
  const char *suffix = target_path + target_length;
  while (directory_length > 0U && base_path[directory_length - 1U] != '/')
    --directory_length;
  while (common < directory_length && common < target_length) {
    size_t next_base = common;
    size_t next_target = common;
    while (next_base < directory_length && base_path[next_base] != '/')
      ++next_base;
    while (next_target < target_length && target_path[next_target] != '/')
      ++next_target;
    if (next_base - common != next_target - common ||
        memcmp(base_path + common, target_path + common, next_base - common) !=
            0)
      break;
    common = next_base + 1U;
  }
  cursor = common;
  while (cursor < directory_length) {
    while (cursor < directory_length && base_path[cursor] != '/') ++cursor;
    if (length + 3U >= capacity) return false;
    memcpy(output + length, "../", 3U);
    length += 3U;
    if (cursor < directory_length) ++cursor;
  }
  if (common < target_length) {
    size_t remainder = target_length - common;
    if (length + remainder + strlen(suffix) >= capacity) return false;
    memcpy(output + length, target_path + common, remainder);
    length += remainder;
  } else if (length == 0U) {
    if (capacity < 3U) return false;
    memcpy(output, "./", 2U);
    length = 2U;
  }
  if (length != 0U) {
    size_t first_segment = 0U;
    while (first_segment < length && output[first_segment] != '/')
      ++first_segment;
    if (memchr(output, ':', first_segment) == NULL) {
      strcpy(output + length, suffix);
      return true;
    }
    if (length + 2U + strlen(suffix) >= capacity) return false;
    memmove(output + 2U, output, length);
    output[0] = '.';
    output[1] = '/';
    length += 2U;
  }
  strcpy(output + length, suffix);
  return true;
}

static bool laghu_html_url_trim(const laghu_html_url_base *base,
                                const unsigned char *value, size_t value_len,
                                char output[LAGHU_RUNTIME_PATH_SIZE]) {
  char target_origin[LAGHU_RUNTIME_PATH_SIZE];
  char target_path[LAGHU_RUNTIME_PATH_SIZE];
  char candidate[LAGHU_RUNTIME_PATH_SIZE];
  char resolved_origin[LAGHU_RUNTIME_PATH_SIZE];
  char resolved_path[LAGHU_RUNTIME_PATH_SIZE];
  size_t best_length = value_len;
  bool changed = false;
  if (!laghu_html_url_resolve(base, value, value_len, target_origin,
                              target_path) ||
      !laghu_html_origin_equal(target_origin, base->page_origin))
    return false;
  if (laghu_html_origin_equal(base->origin, target_origin)) {
    size_t root_length = strlen(target_path);
    if (root_length < best_length &&
        laghu_html_url_resolve(base, (const unsigned char *)target_path,
                               root_length, resolved_origin, resolved_path) &&
        laghu_html_origin_equal(resolved_origin, target_origin) &&
        strcmp(resolved_path, target_path) == 0) {
      strcpy(output, target_path);
      best_length = root_length;
      changed = true;
    }
    if (laghu_html_url_relative(base->path, target_path, candidate,
                                sizeof(candidate)) &&
        strlen(candidate) < best_length &&
        laghu_html_url_resolve(base, (const unsigned char *)candidate,
                               strlen(candidate), resolved_origin,
                               resolved_path) &&
        laghu_html_origin_equal(resolved_origin, target_origin) &&
        strcmp(resolved_path, target_path) == 0) {
      strcpy(output, candidate);
      changed = true;
    }
  }
  return changed;
}

static bool laghu_html_token_list_has(laghu_buffer html,
                                      const laghu_html_attribute *attribute,
                                      const char *wanted) {
  size_t cursor;
  if (attribute == NULL || !attribute->has_value) return false;
  cursor = attribute->value_start;
  while (cursor < attribute->value_end) {
    size_t start;
    while (cursor < attribute->value_end &&
           laghu_html_ascii_space(html.data[cursor]))
      ++cursor;
    start = cursor;
    while (cursor < attribute->value_end &&
           !laghu_html_ascii_space(html.data[cursor]))
      ++cursor;
    if (cursor > start &&
        laghu_head_equal(html.data + start, cursor - start, wanted))
      return true;
  }
  return false;
}

static bool laghu_html_resource_attribute(
    laghu_buffer html, const char *tag_name,
    const laghu_html_attribute *attributes, size_t count,
    const laghu_html_attribute *attribute, bool *srcset) {
  const laghu_html_attribute *rel;
  const laghu_html_attribute *type;
  *srcset = false;
  if (strcmp(tag_name, "script") == 0)
    return strcmp(attribute->name, "src") == 0;
  if (strcmp(tag_name, "img") == 0 || strcmp(tag_name, "source") == 0) {
    *srcset = strcmp(attribute->name, "srcset") == 0;
    return *srcset || strcmp(attribute->name, "src") == 0;
  }
  if (strcmp(tag_name, "video") == 0)
    return strcmp(attribute->name, "src") == 0 ||
           strcmp(attribute->name, "poster") == 0;
  if (strcmp(tag_name, "audio") == 0 || strcmp(tag_name, "track") == 0)
    return strcmp(attribute->name, "src") == 0;
  if (strcmp(tag_name, "input") == 0) {
    type = laghu_html_find_attribute(attributes, count, "type");
    return strcmp(attribute->name, "src") == 0 &&
           laghu_html_attribute_equals(html, type, "image");
  }
  if (strcmp(tag_name, "link") != 0 || strcmp(attribute->name, "href") != 0)
    return false;
  rel = laghu_html_find_attribute(attributes, count, "rel");
  return laghu_html_token_list_has(html, rel, "stylesheet") ||
         laghu_html_token_list_has(html, rel, "preload") ||
         laghu_html_token_list_has(html, rel, "modulepreload") ||
         laghu_html_token_list_has(html, rel, "icon") ||
         laghu_html_token_list_has(html, rel, "manifest");
}

static bool laghu_html_srcset_descriptor(const unsigned char *data,
                                         size_t length) {
  size_t cursor = 0U;
  unsigned int descriptors = 0U;
  bool seen_w = false;
  bool seen_x = false;
  bool seen_h = false;
  while (cursor < length) {
    size_t start;
    bool dot = false;
    bool digit = false;
    bool nonzero = false;
    unsigned char suffix;
    while (cursor < length && laghu_html_ascii_space(data[cursor])) ++cursor;
    if (cursor == length) break;
    start = cursor;
    while (cursor < length && !laghu_html_ascii_space(data[cursor])) ++cursor;
    if (++descriptors > 2U || cursor - start < 2U) return false;
    suffix = data[cursor - 1U];
    while (start + 1U < cursor) {
      unsigned char value = data[start++];
      if (value == '.' && !dot && suffix == 'x') {
        dot = true;
      } else if (isdigit(value)) {
        digit = true;
        if (value != '0') nonzero = true;
      } else {
        return false;
      }
    }
    if (!digit || !nonzero || (suffix != 'w' && suffix != 'x' && suffix != 'h'))
      return false;
    if (suffix == 'w') {
      if (seen_w || seen_x || dot) return false;
      seen_w = true;
    } else if (suffix == 'x') {
      if (seen_w || seen_x || seen_h) return false;
      seen_x = true;
    } else {
      if (seen_h || seen_x || dot) return false;
      seen_h = true;
    }
  }
  return descriptors <= 2U && (!seen_h || seen_w);
}

static bool laghu_html_trim_srcset(const laghu_html_url_base *base,
                                   const unsigned char *value, size_t length,
                                   laghu_head_builder *output) {
  laghu_head_builder builder = {0};
  size_t cursor = 0U;
  size_t copied = 0U;
  unsigned int candidates = 0U;
  bool changed = false;
  while (cursor < length) {
    size_t url_start;
    size_t url_end;
    size_t candidate_end;
    char trimmed[LAGHU_RUNTIME_PATH_SIZE];
    while (cursor < length && laghu_html_ascii_space(value[cursor])) ++cursor;
    url_start = cursor;
    while (cursor < length && !laghu_html_ascii_space(value[cursor]) &&
           value[cursor] != ',')
      ++cursor;
    url_end = cursor;
    if (url_end - url_start >= 5U &&
        laghu_head_equal(value + url_start, 5U, "data:"))
      goto failed;
    while (cursor < length && value[cursor] != ',') ++cursor;
    candidate_end = cursor;
    if (++candidates > 32U || url_start == url_end ||
        !laghu_html_srcset_descriptor(value + url_end, candidate_end - url_end))
      goto failed;
    if (laghu_html_url_trim(base, value + url_start, url_end - url_start,
                            trimmed)) {
      if (!laghu_head_append(&builder, value + copied, url_start - copied) ||
          !laghu_head_append(&builder, trimmed, strlen(trimmed)))
        goto failed;
      copied = url_end;
      changed = true;
    }
    if (cursor < length) ++cursor;
  }
  if (!changed || !laghu_head_append(&builder, value + copied, length - copied))
    goto failed;
  *output = builder;
  return true;
failed:
  free(builder.data);
  return false;
}

static bool laghu_html_effective_base(laghu_buffer html,
                                      const laghu_head_token *tokens,
                                      size_t token_count, const char *page_path,
                                      const char *page_origin,
                                      laghu_html_url_base *base) {
  size_t index;
  laghu_html_attribute *attributes =
      calloc(LAGHU_HTML_MAX_ATTRIBUTES, sizeof(*attributes));
  if (attributes == NULL) return false;
  memset(base, 0, sizeof(*base));
  if (page_path == NULL || page_origin == NULL || page_path[0] != '/' ||
      page_origin[0] == '\0' || strlen(page_path) >= sizeof(base->path) ||
      strlen(page_origin) >= sizeof(base->origin))
    goto finished;
  strcpy(base->origin, page_origin);
  strcpy(base->page_origin, page_origin);
  if (!laghu_html_url_normalize_path(page_path, base->path, sizeof(base->path)))
    goto finished;
  base->enabled = true;
  for (index = 0U; index < token_count; ++index) {
    size_t count = 0U;
    char name[16U];
    bool closing;
    bool self_closing;
    const laghu_html_attribute *href;
    char resolved_origin[LAGHU_RUNTIME_PATH_SIZE];
    char resolved_path[LAGHU_RUNTIME_PATH_SIZE];
    if (tokens[index].kind != LAGHU_HEAD_TOKEN_TAG || tokens[index].closing ||
        tokens[index].protected_content ||
        strcmp(tokens[index].name, "base") != 0)
      continue;
    if (!laghu_html_parse_tag(html, tokens[index].start, tokens[index].end - 1U,
                              name, &closing, &self_closing, attributes,
                              &count)) {
      base->enabled = false;
      goto finished;
    }
    href = laghu_html_find_attribute(attributes, count, "href");
    if (href == NULL) continue;
    if (!href->has_value ||
        !laghu_html_url_resolve(base, html.data + href->value_start,
                                href->value_end - href->value_start,
                                resolved_origin, resolved_path)) {
      base->enabled = false;
      goto finished;
    }
    strcpy(base->origin, resolved_origin);
    strcpy(base->path, resolved_path);
    goto finished;
  }
finished:
  free(attributes);
  return true;
}

bool laghu_html_trim_resource_urls(laghu_buffer html,
                                   const laghu_head_token *tokens,
                                   size_t token_count, const char *page_path,
                                   const char *page_origin,
                                   laghu_runtime_head_result *result) {
  laghu_html_url_base base;
  laghu_head_builder builder = {0};
  size_t cursor = 0U;
  size_t index;
  bool changed = false;
  laghu_html_attribute *attributes =
      calloc(LAGHU_HTML_MAX_ATTRIBUTES, sizeof(*attributes));
  if (attributes == NULL) return false;
  if (!laghu_html_effective_base(html, tokens, token_count, page_path,
                                 page_origin, &base)) {
    free(attributes);
    return false;
  }
  if (!base.enabled) {
    free(attributes);
    return true;
  }
  for (index = 0U; index < token_count; ++index) {
    size_t count = 0U;
    size_t attribute_index;
    char name[16U];
    bool closing;
    bool self_closing;
    if (tokens[index].kind != LAGHU_HEAD_TOKEN_TAG || tokens[index].closing ||
        tokens[index].protected_content ||
        strcmp(tokens[index].name, "base") == 0)
      continue;
    if (!laghu_html_parse_tag(html, tokens[index].start, tokens[index].end - 1U,
                              name, &closing, &self_closing, attributes,
                              &count))
      goto failed;
    for (attribute_index = 0U; attribute_index < count; ++attribute_index) {
      laghu_html_attribute *attribute = &attributes[attribute_index];
      bool srcset;
      char trimmed[LAGHU_RUNTIME_PATH_SIZE];
      laghu_head_builder srcset_value = {0};
      const unsigned char *replacement = NULL;
      size_t replacement_length = 0U;
      if (!attribute->has_value ||
          !laghu_html_resource_attribute(html, name, attributes, count,
                                         attribute, &srcset))
        continue;
      if (srcset) {
        if (laghu_html_trim_srcset(
                &base, html.data + attribute->value_start,
                attribute->value_end - attribute->value_start, &srcset_value)) {
          replacement = srcset_value.data;
          replacement_length = srcset_value.length;
        }
      } else if (laghu_html_url_trim(
                     &base, html.data + attribute->value_start,
                     attribute->value_end - attribute->value_start, trimmed)) {
        replacement = (const unsigned char *)trimmed;
        replacement_length = strlen(trimmed);
      }
      if (replacement != NULL &&
          replacement_length < attribute->value_end - attribute->value_start) {
        if (!laghu_head_append(&builder, html.data + cursor,
                               attribute->value_start - cursor) ||
            !laghu_head_append(&builder, replacement, replacement_length)) {
          free(srcset_value.data);
          goto failed;
        }
        cursor = attribute->value_end;
        changed = true;
      }
      free(srcset_value.data);
    }
  }
  if (!changed) {
    free(builder.data);
    free(attributes);
    return true;
  }
  if (!laghu_head_append(&builder, html.data + cursor, html.length - cursor) ||
      builder.length >= html.length)
    goto failed;
  result->data = builder.data;
  result->length = builder.length;
  result->rewritten = true;
  result->lexical_changed = true;
  free(attributes);
  return true;
failed:
  free(builder.data);
  free(attributes);
  return false;
}

static bool laghu_html_transform_tag(laghu_buffer html, size_t start,
                                     size_t end, laghu_html_planner_mask plan,
                                     laghu_head_builder *builder,
                                     char tag_name[16U], bool *closing,
                                     bool *self_closing,
                                     bool *contenteditable) {
  laghu_html_attribute attributes[LAGHU_HTML_MAX_ATTRIBUTES];
  size_t count = 0U;
  size_t cursor = start;
  size_t index;
  const laghu_html_attribute *rel;
  if (!laghu_html_parse_tag(html, start, end, tag_name, closing, self_closing,
                            attributes, &count)) {
    return false;
  }
  *contenteditable = false;
  rel = laghu_html_find_attribute(attributes, count, "rel");
  for (index = 0U; index < count; ++index) {
    laghu_html_attribute *attribute = &attributes[index];
    if (strcmp(attribute->name, "contenteditable") == 0 &&
        (!attribute->has_value ||
         !laghu_html_attribute_equals(html, attribute, "false"))) {
      *contenteditable = true;
    }
    if ((plan & LAGHU_HTML_PLAN_ELIDE_ATTRIBUTES) != 0U &&
        strcmp(attribute->name, "type") == 0) {
      attribute->elide =
          (strcmp(tag_name, "script") == 0 &&
           laghu_html_attribute_equals(html, attribute, "text/javascript")) ||
          (strcmp(tag_name, "style") == 0 &&
           laghu_html_attribute_equals(html, attribute, "text/css")) ||
          (strcmp(tag_name, "link") == 0 &&
           laghu_html_attribute_equals(html, rel, "stylesheet") &&
           laghu_html_attribute_equals(html, attribute, "text/css"));
    }
  }
  for (index = 0U; index < count; ++index) {
    laghu_html_attribute *attribute = &attributes[index];
    if (attribute->elide) {
      if (!laghu_head_append(builder, html.data + cursor,
                             attribute->leading_start - cursor)) {
        return false;
      }
      cursor = attribute->end;
    } else if ((plan & LAGHU_HTML_PLAN_REMOVE_QUOTES) != 0U &&
               attribute->quote != 0U &&
               laghu_html_unquoted_safe(
                   html.data + attribute->value_start,
                   attribute->value_end - attribute->value_start)) {
      size_t quote_start = attribute->value_start - 1U;
      if (!laghu_head_append(builder, html.data + cursor,
                             quote_start - cursor) ||
          !laghu_head_append(builder, html.data + attribute->value_start,
                             attribute->value_end - attribute->value_start)) {
        return false;
      }
      cursor = attribute->value_end + 1U;
    }
  }
  return laghu_head_append(builder, html.data + cursor, end + 1U - cursor);
}

static bool laghu_html_append_text(laghu_head_builder *builder,
                                   const unsigned char *data, size_t length,
                                   bool collapse) {
  size_t cursor = 0U;
  if (!collapse) {
    return laghu_head_append(builder, data, length);
  }
  while (cursor < length) {
    size_t start = cursor;
    if (laghu_html_ascii_space(data[cursor])) {
      while (cursor < length && laghu_html_ascii_space(data[cursor])) {
        ++cursor;
      }
      if (!laghu_head_append(builder, " ", 1U)) {
        return false;
      }
    } else {
      while (cursor < length && !laghu_html_ascii_space(data[cursor])) {
        ++cursor;
      }
      if (!laghu_head_append(builder, data + start, cursor - start)) {
        return false;
      }
    }
  }
  return true;
}

static bool laghu_html_attributes_equivalent(laghu_buffer before,
                                             const laghu_head_token *left,
                                             laghu_buffer after,
                                             const laghu_head_token *right,
                                             laghu_html_planner_mask plan) {
  laghu_html_attribute left_attributes[LAGHU_HTML_MAX_ATTRIBUTES];
  laghu_html_attribute right_attributes[LAGHU_HTML_MAX_ATTRIBUTES];
  char left_name[16U];
  char right_name[16U];
  bool left_closing;
  bool right_closing;
  bool left_self_closing;
  bool right_self_closing;
  size_t left_count = 0U;
  size_t right_count = 0U;
  size_t left_index;
  size_t right_index = 0U;
  const laghu_html_attribute *rel;
  if (!laghu_html_parse_tag(before, left->start, left->end - 1U, left_name,
                            &left_closing, &left_self_closing, left_attributes,
                            &left_count) ||
      !laghu_html_parse_tag(after, right->start, right->end - 1U, right_name,
                            &right_closing, &right_self_closing,
                            right_attributes, &right_count) ||
      left_closing != right_closing ||
      left_self_closing != right_self_closing) {
    return false;
  }
  rel = laghu_html_find_attribute(left_attributes, left_count, "rel");
  for (left_index = 0U; left_index < left_count; ++left_index) {
    const laghu_html_attribute *attribute = &left_attributes[left_index];
    bool approved_elision =
        (plan & LAGHU_HTML_PLAN_ELIDE_ATTRIBUTES) != 0U &&
        strcmp(attribute->name, "type") == 0 &&
        ((strcmp(left_name, "script") == 0 &&
          laghu_html_attribute_equals(before, attribute, "text/javascript")) ||
         (strcmp(left_name, "style") == 0 &&
          laghu_html_attribute_equals(before, attribute, "text/css")) ||
         (strcmp(left_name, "link") == 0 &&
          laghu_html_attribute_equals(before, rel, "stylesheet") &&
          laghu_html_attribute_equals(before, attribute, "text/css")));
    if (approved_elision) {
      continue;
    }
    if (right_index == right_count ||
        strcmp(attribute->name, right_attributes[right_index].name) != 0 ||
        attribute->has_value != right_attributes[right_index].has_value ||
        (attribute->has_value &&
         (attribute->value_end - attribute->value_start !=
              right_attributes[right_index].value_end -
                  right_attributes[right_index].value_start ||
          memcmp(before.data + attribute->value_start,
                 after.data + right_attributes[right_index].value_start,
                 attribute->value_end - attribute->value_start) != 0))) {
      return false;
    }
    ++right_index;
  }
  return right_index == right_count;
}

static bool laghu_html_tag_sequence_equal(laghu_buffer before,
                                          laghu_buffer after,
                                          laghu_html_planner_mask plan) {
  laghu_head_token *left = calloc(LAGHU_HTML_MAX_TOKENS, sizeof(*left));
  laghu_head_token *right = calloc(LAGHU_HTML_MAX_TOKENS, sizeof(*right));
  size_t left_count = 0U;
  size_t right_count = 0U;
  size_t li = 0U;
  size_t ri = 0U;
  bool equal = false;
  if (left == NULL || right == NULL ||
      !laghu_head_tokenize(before, left, &left_count) ||
      !laghu_head_tokenize(after, right, &right_count) ||
      !laghu_head_pair_tokens(left, left_count) ||
      !laghu_head_pair_tokens(right, right_count)) {
    goto finished;
  }
  for (;;) {
    while (li < left_count && left[li].kind == LAGHU_HEAD_TOKEN_COMMENT) {
      ++li;
    }
    while (ri < right_count && right[ri].kind == LAGHU_HEAD_TOKEN_COMMENT) {
      ++ri;
    }
    if (li == left_count || ri == right_count) {
      equal = li == left_count && ri == right_count;
      break;
    }
    if (left[li].kind != right[ri].kind ||
        left[li].closing != right[ri].closing ||
        strcmp(left[li].name, right[ri].name) != 0 ||
        (left[li].kind == LAGHU_HEAD_TOKEN_TAG &&
         !laghu_html_attributes_equivalent(before, &left[li], after, &right[ri],
                                           plan))) {
      break;
    }
    ++li;
    ++ri;
  }
finished:
  free(left);
  free(right);
  return equal;
}

static size_t laghu_html_paired_end(laghu_buffer html,
                                    const laghu_head_token *tokens,
                                    size_t token_count, size_t start,
                                    const char *name) {
  size_t index;
  unsigned int depth = 0U;
  for (index = 0U; index < token_count; ++index) {
    if (tokens[index].kind != LAGHU_HEAD_TOKEN_TAG ||
        strcmp(tokens[index].name, name) != 0 || tokens[index].start < start) {
      continue;
    }
    if (!tokens[index].closing) {
      if (tokens[index].end < 2U || html.data[tokens[index].end - 2U] != '/') {
        ++depth;
      }
    } else if (depth != 0U && --depth == 0U) {
      return tokens[index].end;
    }
  }
  return SIZE_MAX;
}

bool laghu_html_lexical(laghu_buffer html, laghu_html_planner_mask plan,
                        laghu_runtime_head_result *result) {
  laghu_head_builder builder = {0};
  laghu_head_token *tokens = calloc(LAGHU_HTML_MAX_TOKENS, sizeof(*tokens));
  size_t document_token_count = 0U;
  size_t cursor = 0U;
  unsigned int token_count = 0U;
  if (tokens == NULL ||
      !laghu_head_tokenize(html, tokens, &document_token_count)) {
    goto failed;
  }
  while (cursor < html.length) {
    const unsigned char *found =
        memchr(html.data + cursor, '<', html.length - cursor);
    size_t start;
    size_t end;
    unsigned char quote = 0U;
    if (found == NULL) {
      if (!laghu_html_append_text(
              &builder, html.data + cursor, html.length - cursor,
              (plan & LAGHU_HTML_PLAN_COLLAPSE_WHITESPACE) != 0U)) {
        goto failed;
      }
      break;
    }
    start = (size_t)(found - html.data);
    if (!laghu_html_append_text(
            &builder, html.data + cursor, start - cursor,
            (plan & LAGHU_HTML_PLAN_COLLAPSE_WHITESPACE) != 0U)) {
      goto failed;
    }
    if (++token_count > LAGHU_HTML_MAX_TOKENS) {
      goto failed;
    }
    if (start + 4U <= html.length &&
        memcmp(html.data + start, "<!--", 4U) == 0) {
      const unsigned char *close = NULL;
      size_t index;
      for (index = start + 4U; index + 2U < html.length; ++index) {
        if (memcmp(html.data + index, "-->", 3U) == 0) {
          close = html.data + index;
          break;
        }
      }
      if (close == NULL) {
        goto failed;
      }
      end = (size_t)(close - html.data) + 3U;
      if ((plan & LAGHU_HTML_PLAN_REMOVE_COMMENTS) == 0U ||
          laghu_html_comment_preserved(html.data + start + 4U,
                                       end - start - 7U)) {
        if (!laghu_head_append(&builder, html.data + start, end - start)) {
          goto failed;
        }
      }
      cursor = end;
      continue;
    }
    end = start + 1U;
    while (end < html.length) {
      unsigned char value = html.data[end];
      if (quote != 0U) {
        if (value == quote) {
          quote = 0U;
        }
      } else if (value == '\'' || value == '"') {
        quote = value;
      } else if (value == '>') {
        break;
      }
      ++end;
    }
    if (end == html.length || quote != 0U) {
      goto failed;
    }
    if (start + 2U < html.length &&
        (html.data[start + 1U] == '!' || html.data[start + 1U] == '?')) {
      if (!laghu_head_append(&builder, html.data + start, end + 1U - start)) {
        goto failed;
      }
      cursor = end + 1U;
      continue;
    }
    {
      char name[16U];
      bool closing;
      bool self_closing;
      bool contenteditable;
      size_t protected_end;
      if (!laghu_html_transform_tag(html, start, end, plan, &builder, name,
                                    &closing, &self_closing,
                                    &contenteditable)) {
        goto failed;
      }
      cursor = end + 1U;
      if (!closing && !self_closing &&
          (laghu_html_protected_element(name) || contenteditable)) {
        if (strcmp(name, "plaintext") == 0) {
          if (!laghu_head_append(&builder, html.data + cursor,
                                 html.length - cursor)) {
            goto failed;
          }
          cursor = html.length;
          continue;
        }
        protected_end = laghu_html_paired_end(
            html, tokens, document_token_count, start, name);
        if (protected_end == SIZE_MAX || protected_end < cursor) {
          goto failed;
        }
        if (!laghu_head_append(&builder, html.data + cursor,
                               protected_end - cursor)) {
          goto failed;
        }
        cursor = protected_end;
      }
    }
  }
  if (!laghu_html_tag_sequence_equal(
          html, (laghu_buffer){builder.data, builder.length}, plan)) {
    goto failed;
  }
  if (builder.length != html.length ||
      memcmp(builder.data, html.data, html.length) != 0) {
    result->data = builder.data;
    result->length = builder.length;
    result->rewritten = true;
    result->lexical_changed = true;
    builder.data = NULL;
  }
  free(tokens);
  free(builder.data);
  return true;
failed:
  free(tokens);
  free(builder.data);
  return false;
}
