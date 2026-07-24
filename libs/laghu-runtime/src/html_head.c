// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/runtime.h"

#define LAGHU_HEAD_MAX_CSS_NODES 64U

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

bool laghu_runtime_normalize_head(laghu_buffer html, bool add_or_combine_head,
                                  bool move_css_to_head,
                                  bool move_css_above_scripts,
                                  laghu_runtime_head_result *result) {
  laghu_head_token *tokens = NULL;
  size_t token_count = 0U;
  laghu_runtime_head_result normalized = {0};
  laghu_runtime_head_result moved = {0};
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
  if (add_or_combine_head &&
      !laghu_head_merge(html, tokens, token_count, &normalized)) {
    goto finished;
  }
  if (normalized.rewritten) {
    working = (laghu_buffer){normalized.data, normalized.length};
  }
  if (move_css_to_head &&
      !laghu_head_move_css(working, move_css_above_scripts, &moved)) {
    goto finished;
  }
  if (moved.rewritten) {
    result->data = moved.data;
    result->length = moved.length;
    result->moved_css = true;
    moved.data = NULL;
  } else if (normalized.rewritten) {
    result->data = normalized.data;
    result->length = normalized.length;
    normalized.data = NULL;
  }
  result->added_head = normalized.added_head;
  result->combined_heads = normalized.combined_heads;
  result->rewritten = result->data != NULL;
  success = true;
finished:
  free(tokens);
  laghu_runtime_head_result_release(&normalized);
  laghu_runtime_head_result_release(&moved);
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
