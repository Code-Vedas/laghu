// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/runtime.h"

#define LAGHU_HEAD_MAX_CSS_NODES 64U
#define LAGHU_HTML_MAX_ATTRIBUTES 128U
#define LAGHU_HTML_ATTRIBUTE_NAME_SIZE 64U

typedef enum {
  LAGHU_HEAD_TOKEN_TAG = 0,
  LAGHU_HEAD_TOKEN_COMMENT,
  LAGHU_HEAD_TOKEN_DOCTYPE
} laghu_head_token_kind;

typedef struct {
  size_t start;
  size_t end;
  size_t pair;
  laghu_head_token_kind kind;
  char name[16U];
  bool closing;
  bool protected_content;
} laghu_head_token;

typedef struct {
  unsigned char *data;
  size_t length;
  size_t capacity;
} laghu_head_builder;

typedef struct {
  size_t start;
  size_t end;
} laghu_head_range;

typedef struct {
  size_t leading_start;
  size_t name_start;
  size_t name_end;
  size_t value_start;
  size_t value_end;
  size_t end;
  unsigned char quote;
  char name[LAGHU_HTML_ATTRIBUTE_NAME_SIZE];
  bool has_value;
  bool elide;
} laghu_html_attribute;

static bool laghu_head_append(laghu_head_builder *builder, const void *data,
                              size_t length) {
  size_t needed;
  size_t capacity;
  unsigned char *grown;
  if (length > SIZE_MAX - builder->length - 1U) {
    return false;
  }
  needed = builder->length + length + 1U;
  if (needed > builder->capacity) {
    capacity = builder->capacity == 0U ? 1024U : builder->capacity;
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
  if (length != 0U) {
    memcpy(builder->data + builder->length, data, length);
  }
  builder->length += length;
  builder->data[builder->length] = '\0';
  return true;
}

static bool laghu_head_equal(const unsigned char *value, size_t length,
                             const char *expected) {
  size_t index;
  if (length != strlen(expected)) {
    return false;
  }
  for (index = 0U; index < length; ++index) {
    if (tolower(value[index]) != tolower((unsigned char)expected[index])) {
      return false;
    }
  }
  return true;
}

static bool laghu_html_ascii_space(unsigned char value) {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
         value == '\f';
}

static bool laghu_html_contains_ci(const unsigned char *data, size_t length,
                                   const char *needle) {
  size_t needle_length = strlen(needle);
  size_t offset;
  if (needle_length == 0U || needle_length > length) {
    return false;
  }
  for (offset = 0U; offset + needle_length <= length; ++offset) {
    size_t index;
    for (index = 0U; index < needle_length; ++index) {
      if (tolower(data[offset + index]) !=
          tolower((unsigned char)needle[index])) {
        break;
      }
    }
    if (index == needle_length) {
      return true;
    }
  }
  return false;
}

static bool laghu_html_protected_element(const char *name) {
  static const char *const names[] = {
      "pre",  "textarea", "script", "style",   "template", "noscript", "svg",
      "math", "xmp",      "iframe", "noembed", "noframes", "plaintext"};
  size_t index;
  for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
    if (strcmp(name, names[index]) == 0) {
      return true;
    }
  }
  return false;
}

static bool laghu_head_protected_name(const char *name) {
  return strcmp(name, "template") == 0 || strcmp(name, "noscript") == 0 ||
         strcmp(name, "svg") == 0 || strcmp(name, "math") == 0;
}

static size_t laghu_head_raw_close(laghu_buffer html, size_t cursor,
                                   const char *name) {
  size_t name_length = strlen(name);
  while (cursor + name_length + 3U <= html.length) {
    const unsigned char *open =
        memchr(html.data + cursor, '<', html.length - cursor);
    size_t start;
    size_t end;
    if (open == NULL) {
      return SIZE_MAX;
    }
    start = (size_t)(open - html.data);
    if (start + name_length + 2U < html.length &&
        html.data[start + 1U] == '/' &&
        laghu_head_equal(html.data + start + 2U, name_length, name) &&
        (isspace(html.data[start + 2U + name_length]) ||
         html.data[start + 2U + name_length] == '>')) {
      end = start + 2U + name_length;
      while (end < html.length && html.data[end] != '>') {
        ++end;
      }
      return end < html.length ? start : SIZE_MAX;
    }
    cursor = start + 1U;
  }
  return SIZE_MAX;
}

static bool laghu_head_tokenize(laghu_buffer html, laghu_head_token *tokens,
                                size_t *token_count) {
  size_t cursor = 0U;
  size_t count = 0U;
  unsigned int protected_depth = 0U;
  while (cursor < html.length) {
    size_t start;
    size_t end;
    size_t name_start;
    size_t name_end;
    unsigned char quote = 0U;
    laghu_head_token *token;
    const unsigned char *open =
        memchr(html.data + cursor, '<', html.length - cursor);
    if (open == NULL) {
      break;
    }
    start = (size_t)(open - html.data);
    if (count == LAGHU_HTML_MAX_TOKENS) {
      return false;
    }
    token = &tokens[count];
    memset(token, 0, sizeof(*token));
    token->start = start;
    token->pair = SIZE_MAX;
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
        return false;
      }
      token->kind = LAGHU_HEAD_TOKEN_COMMENT;
      token->end = (size_t)(close - html.data) + 3U;
      token->protected_content = protected_depth != 0U;
      cursor = token->end;
      ++count;
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
      return false;
    }
    token->end = end + 1U;
    if (start + sizeof("<!doctype") - 1U <= end &&
        laghu_head_equal(html.data + start + 1U, sizeof("!doctype") - 1U,
                         "!doctype")) {
      token->kind = LAGHU_HEAD_TOKEN_DOCTYPE;
      cursor = token->end;
      ++count;
      continue;
    }
    name_start = start + 1U;
    while (name_start < end && isspace(html.data[name_start])) {
      ++name_start;
    }
    if (name_start < end && html.data[name_start] == '/') {
      token->closing = true;
      ++name_start;
      while (name_start < end && isspace(html.data[name_start])) {
        ++name_start;
      }
    }
    name_end = name_start;
    while (name_end < end &&
           (isalnum(html.data[name_end]) || html.data[name_end] == '-' ||
            html.data[name_end] == '_')) {
      ++name_end;
    }
    if (name_end == name_start) {
      cursor = token->end;
      continue;
    }
    if (name_end - name_start >= sizeof(token->name)) {
      return false;
    }
    {
      size_t index;
      for (index = 0U; index < name_end - name_start; ++index) {
        token->name[index] = (char)tolower(html.data[name_start + index]);
      }
    }
    token->kind = LAGHU_HEAD_TOKEN_TAG;
    token->protected_content = protected_depth != 0U;
    if (token->kind == LAGHU_HEAD_TOKEN_TAG &&
        laghu_head_protected_name(token->name)) {
      if (token->closing) {
        if (protected_depth == 0U) {
          return false;
        }
        --protected_depth;
        token->protected_content = protected_depth != 0U;
      } else if (html.data[end - 1U] != '/') {
        ++protected_depth;
        token->protected_content = true;
      }
    }
    cursor = token->end;
    ++count;
    if (!token->closing && html.data[end - 1U] != '/' &&
        (strcmp(token->name, "script") == 0 ||
         strcmp(token->name, "style") == 0)) {
      cursor = laghu_head_raw_close(html, cursor, token->name);
      if (cursor == SIZE_MAX) {
        return false;
      }
    }
  }
  if (protected_depth != 0U) {
    return false;
  }
  *token_count = count;
  return true;
}

static bool laghu_head_attribute(const unsigned char *tag, size_t length,
                                 const char *name, const unsigned char **value,
                                 size_t *value_length, bool *present) {
  size_t cursor = 1U;
  *present = false;
  while (cursor < length && !isspace(tag[cursor]) && tag[cursor] != '>') {
    ++cursor;
  }
  while (cursor < length) {
    size_t start;
    size_t end;
    size_t value_start;
    unsigned char quote = 0U;
    while (cursor < length && (isspace(tag[cursor]) || tag[cursor] == '/')) {
      ++cursor;
    }
    start = cursor;
    while (cursor < length &&
           (isalnum(tag[cursor]) || tag[cursor] == '-' || tag[cursor] == '_')) {
      ++cursor;
    }
    end = cursor;
    if (end == start) {
      break;
    }
    while (cursor < length && isspace(tag[cursor])) {
      ++cursor;
    }
    if (cursor >= length || tag[cursor] != '=') {
      if (laghu_head_equal(tag + start, end - start, name)) {
        *present = true;
        *value = NULL;
        *value_length = 0U;
        return true;
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
    value_start = cursor;
    while (cursor < length &&
           ((quote != 0U && tag[cursor] != quote) ||
            (quote == 0U && !isspace(tag[cursor]) && tag[cursor] != '>'))) {
      ++cursor;
    }
    if (laghu_head_equal(tag + start, end - start, name)) {
      *present = true;
      *value = tag + value_start;
      *value_length = cursor - value_start;
      return true;
    }
    if (quote != 0U && cursor < length) {
      ++cursor;
    }
  }
  return true;
}

static bool laghu_head_has_event_handler(const unsigned char *tag,
                                         size_t length) {
  size_t cursor = 1U;
  while (cursor < length && !isspace(tag[cursor]) && tag[cursor] != '>') {
    ++cursor;
  }
  while (cursor < length) {
    size_t start;
    size_t end;
    unsigned char quote = 0U;
    while (cursor < length && (isspace(tag[cursor]) || tag[cursor] == '/')) {
      ++cursor;
    }
    start = cursor;
    while (cursor < length &&
           (isalnum(tag[cursor]) || tag[cursor] == '-' || tag[cursor] == '_')) {
      ++cursor;
    }
    end = cursor;
    if (end == start) {
      break;
    }
    if (end - start > 2U && tolower(tag[start]) == 'o' &&
        tolower(tag[start + 1U]) == 'n') {
      return true;
    }
    while (cursor < length && isspace(tag[cursor])) {
      ++cursor;
    }
    if (cursor < length && tag[cursor] == '=') {
      ++cursor;
      while (cursor < length && isspace(tag[cursor])) {
        ++cursor;
      }
      if (cursor < length && (tag[cursor] == '\'' || tag[cursor] == '"')) {
        quote = tag[cursor++];
      }
      while (cursor < length &&
             ((quote != 0U && tag[cursor] != quote) ||
              (quote == 0U && !isspace(tag[cursor]) && tag[cursor] != '>'))) {
        ++cursor;
      }
      if (quote != 0U && cursor < length) {
        ++cursor;
      }
    }
  }
  return false;
}

static bool laghu_head_trivia(const unsigned char *data, size_t length) {
  size_t cursor = 0U;
  while (cursor < length) {
    if (isspace(data[cursor])) {
      ++cursor;
    } else if (cursor + 4U <= length &&
               memcmp(data + cursor, "<!--", 4U) == 0) {
      const unsigned char *close = NULL;
      size_t index;
      for (index = cursor + 4U; index + 2U < length; ++index) {
        if (memcmp(data + index, "-->", 3U) == 0) {
          close = data + index;
          break;
        }
      }
      if (close == NULL) {
        return false;
      }
      cursor = (size_t)(close - data) + 3U;
    } else {
      return false;
    }
  }
  return true;
}

static bool laghu_head_pair_tokens(laghu_head_token *tokens, size_t count) {
  size_t stack[LAGHU_HTML_MAX_TOKENS];
  size_t stack_count = 0U;
  size_t index;
  for (index = 0U; index < count; ++index) {
    if (tokens[index].kind != LAGHU_HEAD_TOKEN_TAG ||
        (strcmp(tokens[index].name, "head") != 0 &&
         strcmp(tokens[index].name, "style") != 0)) {
      continue;
    }
    if (!tokens[index].closing) {
      size_t nested;
      for (nested = 0U; nested < stack_count; ++nested) {
        if (strcmp(tokens[stack[nested]].name, tokens[index].name) == 0) {
          return false;
        }
      }
      if (stack_count == LAGHU_HTML_MAX_TOKENS) {
        return false;
      }
      stack[stack_count++] = index;
    } else {
      size_t open;
      if (stack_count == 0U) {
        return false;
      }
      open = stack[--stack_count];
      if (strcmp(tokens[open].name, tokens[index].name) != 0) {
        return false;
      }
      tokens[open].pair = index;
      tokens[index].pair = open;
    }
  }
  return stack_count == 0U;
}

static bool laghu_head_ambiguous(laghu_buffer html, laghu_head_token *tokens,
                                 size_t count) {
  size_t previous = SIZE_MAX;
  size_t index;
  for (index = 0U; index < count; ++index) {
    if (tokens[index].kind != LAGHU_HEAD_TOKEN_TAG || tokens[index].closing ||
        tokens[index].protected_content ||
        strcmp(tokens[index].name, "head") != 0) {
      continue;
    }
    if (previous != SIZE_MAX) {
      size_t close = tokens[previous].pair;
      if (close == SIZE_MAX ||
          !laghu_head_trivia(html.data + tokens[close].end,
                             tokens[index].start - tokens[close].end)) {
        return true;
      }
    }
    previous = index;
  }
  return false;
}

static bool laghu_head_merge(laghu_buffer html, laghu_head_token *tokens,
                             size_t count, laghu_runtime_head_result *result) {
  size_t heads[LAGHU_HTML_MAX_HEADS];
  size_t head_count = 0U;
  size_t index;
  size_t cursor = 0U;
  laghu_head_builder builder = {0};
  for (index = 0U; index < count; ++index) {
    if (tokens[index].kind == LAGHU_HEAD_TOKEN_TAG && !tokens[index].closing &&
        strcmp(tokens[index].name, "head") == 0 &&
        !tokens[index].protected_content) {
      if (head_count == LAGHU_HTML_MAX_HEADS ||
          tokens[index].pair == SIZE_MAX) {
        return false;
      }
      heads[head_count++] = index;
    }
  }
  if (head_count == 0U) {
    size_t insertion = SIZE_MAX;
    for (index = 0U; index < count; ++index) {
      if (tokens[index].kind == LAGHU_HEAD_TOKEN_TAG &&
          !tokens[index].closing && strcmp(tokens[index].name, "html") == 0) {
        insertion = tokens[index].end;
        break;
      }
    }
    if (insertion == SIZE_MAX) {
      for (index = 0U; index < count; ++index) {
        if (tokens[index].kind == LAGHU_HEAD_TOKEN_DOCTYPE) {
          insertion = tokens[index].end;
          break;
        }
      }
    }
    if (insertion == SIZE_MAX) {
      for (index = 0U; index < count; ++index) {
        if (tokens[index].kind == LAGHU_HEAD_TOKEN_TAG &&
            !tokens[index].closing && strcmp(tokens[index].name, "body") == 0) {
          insertion = tokens[index].start;
          break;
        }
      }
    }
    if (insertion == SIZE_MAX ||
        !laghu_head_append(&builder, html.data, insertion) ||
        !laghu_head_append(&builder, "<head></head>", 13U) ||
        !laghu_head_append(&builder, html.data + insertion,
                           html.length - insertion)) {
      free(builder.data);
      return insertion == SIZE_MAX;
    }
    result->data = builder.data;
    result->length = builder.length;
    result->rewritten = true;
    result->added_head = true;
    return true;
  }
  for (index = 0U; index < head_count;) {
    size_t group_end = index;
    size_t first = heads[index];
    size_t first_close = tokens[first].pair;
    while (group_end + 1U < head_count) {
      size_t current_close = tokens[heads[group_end]].pair;
      size_t next = heads[group_end + 1U];
      if (!laghu_head_trivia(html.data + tokens[current_close].end,
                             tokens[next].start - tokens[current_close].end)) {
        break;
      }
      ++group_end;
    }
    if (group_end == index) {
      ++index;
      continue;
    }
    if (!laghu_head_append(&builder, html.data + cursor,
                           tokens[first].end - cursor)) {
      free(builder.data);
      return false;
    }
    {
      size_t member;
      for (member = index; member <= group_end; ++member) {
        size_t open = heads[member];
        size_t close = tokens[open].pair;
        if (member != index) {
          size_t previous_close = tokens[heads[member - 1U]].pair;
          if (!laghu_head_append(
                  &builder, html.data + tokens[previous_close].end,
                  tokens[open].start - tokens[previous_close].end)) {
            free(builder.data);
            return false;
          }
        }
        if (!laghu_head_append(&builder, html.data + tokens[open].end,
                               tokens[close].start - tokens[open].end)) {
          free(builder.data);
          return false;
        }
      }
    }
    if (!laghu_head_append(
            &builder, html.data + tokens[first_close].start,
            tokens[first_close].end - tokens[first_close].start)) {
      free(builder.data);
      return false;
    }
    cursor = tokens[tokens[heads[group_end]].pair].end;
    result->combined_heads = true;
    index = group_end + 1U;
  }
  if (!result->combined_heads) {
    free(builder.data);
    return true;
  }
  if (!laghu_head_append(&builder, html.data + cursor, html.length - cursor)) {
    free(builder.data);
    return false;
  }
  result->data = builder.data;
  result->length = builder.length;
  result->rewritten = true;
  return true;
}

static bool laghu_head_script_executable(laghu_buffer html,
                                         const laghu_head_token *token) {
  const unsigned char *type = NULL;
  size_t type_length = 0U;
  bool present;
  if (!laghu_head_attribute(html.data + token->start, token->end - token->start,
                            "type", &type, &type_length, &present)) {
    return true;
  }
  if (!present || type_length == 0U ||
      laghu_head_equal(type, type_length, "text/javascript") ||
      laghu_head_equal(type, type_length, "application/javascript") ||
      laghu_head_equal(type, type_length, "module")) {
    return true;
  }
  return !(laghu_head_equal(type, type_length, "application/ld+json") ||
           laghu_head_equal(type, type_length, "application/json") ||
           laghu_head_equal(type, type_length, "importmap") ||
           laghu_head_equal(type, type_length, "speculationrules"));
}

static bool laghu_head_css_eligible(laghu_buffer html,
                                    const laghu_head_token *token) {
  const unsigned char *value = NULL;
  size_t value_length = 0U;
  bool present;
  if (token->protected_content || token->closing ||
      laghu_head_has_event_handler(html.data + token->start,
                                   token->end - token->start)) {
    return false;
  }
  if (strcmp(token->name, "style") == 0) {
    if (!laghu_head_attribute(html.data + token->start,
                              token->end - token->start, "scoped", &value,
                              &value_length, &present) ||
        present || token->pair == SIZE_MAX) {
      return false;
    }
    if (!laghu_head_attribute(html.data + token->start,
                              token->end - token->start, "type", &value,
                              &value_length, &present)) {
      return false;
    }
    return !present || laghu_head_equal(value, value_length, "text/css");
  }
  if (strcmp(token->name, "link") != 0 ||
      !laghu_head_attribute(html.data + token->start, token->end - token->start,
                            "rel", &value, &value_length, &present) ||
      !present || !laghu_head_equal(value, value_length, "stylesheet")) {
    return false;
  }
  if (!laghu_head_attribute(html.data + token->start, token->end - token->start,
                            "disabled", &value, &value_length, &present) ||
      present ||
      !laghu_head_attribute(html.data + token->start, token->end - token->start,
                            "alternate", &value, &value_length, &present) ||
      present) {
    return false;
  }
  if (!laghu_head_attribute(html.data + token->start, token->end - token->start,
                            "type", &value, &value_length, &present)) {
    return false;
  }
  return !present || laghu_head_equal(value, value_length, "text/css");
}

static bool laghu_head_move_css(laghu_buffer html, bool cross_scripts,
                                laghu_runtime_head_result *result) {
  laghu_head_token *tokens = calloc(LAGHU_HTML_MAX_TOKENS, sizeof(*tokens));
  laghu_head_range ranges[LAGHU_HEAD_MAX_CSS_NODES];
  size_t token_count = 0U;
  size_t range_count = 0U;
  size_t first_script = SIZE_MAX;
  size_t head_open = SIZE_MAX;
  size_t head_close = SIZE_MAX;
  size_t insertion;
  size_t cursor = 0U;
  size_t index;
  laghu_head_builder moved = {0};
  laghu_head_builder output = {0};
  bool success = false;
  if (tokens == NULL || !laghu_head_tokenize(html, tokens, &token_count) ||
      !laghu_head_pair_tokens(tokens, token_count)) {
    goto finished;
  }
  if (laghu_head_ambiguous(html, tokens, token_count)) {
    success = true;
    goto finished;
  }
  for (index = 0U; index < token_count; ++index) {
    if (tokens[index].kind != LAGHU_HEAD_TOKEN_TAG) {
      continue;
    }
    if (!tokens[index].closing && strcmp(tokens[index].name, "head") == 0 &&
        !tokens[index].protected_content && head_open == SIZE_MAX) {
      head_open = index;
      head_close = tokens[index].pair;
    }
    if (!tokens[index].closing && strcmp(tokens[index].name, "script") == 0 &&
        !tokens[index].protected_content &&
        laghu_head_script_executable(html, &tokens[index]) &&
        first_script == SIZE_MAX) {
      first_script = tokens[index].start;
    }
  }
  if (head_open == SIZE_MAX || head_close == SIZE_MAX) {
    success = true;
    goto finished;
  }
  insertion = tokens[head_close].start;
  if (cross_scripts && first_script != SIZE_MAX &&
      first_script >= tokens[head_open].end &&
      first_script < tokens[head_close].start) {
    insertion = first_script;
  }
  for (index = 0U; index < token_count; ++index) {
    size_t end;
    if (tokens[index].kind != LAGHU_HEAD_TOKEN_TAG ||
        !laghu_head_css_eligible(html, &tokens[index]) ||
        (!cross_scripts && first_script != SIZE_MAX &&
         tokens[index].start > first_script)) {
      continue;
    }
    end = strcmp(tokens[index].name, "style") == 0
              ? tokens[tokens[index].pair].end
              : tokens[index].end;
    if (range_count == LAGHU_HEAD_MAX_CSS_NODES ||
        !laghu_head_append(&moved, html.data + tokens[index].start,
                           end - tokens[index].start)) {
      goto finished;
    }
    ranges[range_count++] = (laghu_head_range){tokens[index].start, end};
  }
  if (range_count == 0U) {
    success = true;
    goto finished;
  }
  for (index = 0U; index <= range_count; ++index) {
    size_t next = index < range_count ? ranges[index].start : html.length;
    if (cursor <= insertion && insertion <= next) {
      if (!laghu_head_append(&output, html.data + cursor, insertion - cursor) ||
          !laghu_head_append(&output, moved.data, moved.length)) {
        goto finished;
      }
      cursor = insertion;
      insertion = SIZE_MAX;
    }
    if (index < range_count) {
      if (!laghu_head_append(&output, html.data + cursor,
                             ranges[index].start - cursor)) {
        goto finished;
      }
      cursor = ranges[index].end;
    }
  }
  if (insertion != SIZE_MAX) {
    if (!laghu_head_append(&output, html.data + cursor, insertion - cursor) ||
        !laghu_head_append(&output, moved.data, moved.length)) {
      goto finished;
    }
    cursor = insertion;
    insertion = SIZE_MAX;
  }
  if (!laghu_head_append(&output, html.data + cursor, html.length - cursor)) {
    goto finished;
  }
  if (insertion != SIZE_MAX || output.length != html.length ||
      memcmp(output.data, html.data, html.length) != 0) {
    result->data = output.data;
    result->length = output.length;
    result->rewritten = true;
    result->moved_css = true;
    output.data = NULL;
  }
  success = true;
finished:
  free(tokens);
  free(moved.data);
  free(output.data);
  return success;
}

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

static bool laghu_html_lexical(laghu_buffer html, laghu_html_planner_mask plan,
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

bool laghu_runtime_plan_html_document(laghu_buffer html,
                                      laghu_html_planner_mask plan,
                                      laghu_runtime_head_result *result) {
  laghu_head_token *tokens = NULL;
  size_t token_count = 0U;
  laghu_runtime_head_result normalized = {0};
  laghu_runtime_head_result moved = {0};
  laghu_runtime_head_result lexical = {0};
  laghu_buffer working = html;
  bool success = false;
  if (result == NULL || html.length > LAGHU_IMAGE_MAX_INPUT_BYTES ||
      (html.data == NULL && html.length != 0U)) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  tokens = calloc(LAGHU_HTML_MAX_TOKENS, sizeof(*tokens));
  if (tokens == NULL || !laghu_head_tokenize(html, tokens, &token_count) ||
      !laghu_head_pair_tokens(tokens, token_count)) {
    goto finished;
  }
  if ((plan & LAGHU_HTML_PLAN_ADD_COMBINE_HEAD) != 0U &&
      !laghu_head_merge(html, tokens, token_count, &normalized)) {
    goto finished;
  }
  if (normalized.rewritten) {
    working = (laghu_buffer){normalized.data, normalized.length};
  }
  if ((plan & LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD) != 0U &&
      !laghu_head_move_css(
          working, (plan & LAGHU_HTML_PLAN_MOVE_CSS_ABOVE_SCRIPTS) != 0U,
          &moved)) {
    goto finished;
  }
  if (moved.rewritten) {
    working = (laghu_buffer){moved.data, moved.length};
  }
  if ((plan & LAGHU_HTML_PLAN_LEXICAL) != 0U &&
      !laghu_html_lexical(working, plan, &lexical)) {
    goto finished;
  }
  if (lexical.rewritten) {
    result->data = lexical.data;
    result->length = lexical.length;
    result->lexical_changed = true;
    lexical.data = NULL;
  } else if (moved.rewritten) {
    result->data = moved.data;
    result->length = moved.length;
    moved.data = NULL;
  } else if (normalized.rewritten) {
    result->data = normalized.data;
    result->length = normalized.length;
    normalized.data = NULL;
  }
  result->added_head = normalized.added_head;
  result->combined_heads = normalized.combined_heads;
  result->moved_css = moved.moved_css;
  result->structural_changed = normalized.rewritten || moved.rewritten;
  result->rewritten = result->data != NULL;
  success = true;
finished:
  free(tokens);
  laghu_runtime_head_result_release(&normalized);
  laghu_runtime_head_result_release(&moved);
  laghu_runtime_head_result_release(&lexical);
  if (!success) {
    laghu_runtime_head_result_release(result);
  }
  return success;
}

void laghu_runtime_head_result_release(laghu_runtime_head_result *result) {
  if (result != NULL) {
    free(result->data);
    memset(result, 0, sizeof(*result));
  }
}
