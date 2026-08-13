// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_HTML_INTERNAL_H
#define LAGHU_HTML_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "laghu/html.h"
#include "laghu/types.h"

#define LAGHU_HEAD_MAX_CSS_NODES 64U
#define LAGHU_HTML_MAX_ATTRIBUTES 128U
#define LAGHU_HTML_ATTRIBUTE_NAME_SIZE 64U

typedef enum { LAGHU_HEAD_TOKEN_TAG = 0, LAGHU_HEAD_TOKEN_COMMENT, LAGHU_HEAD_TOKEN_DOCTYPE } laghu_head_token_kind;

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

typedef struct {
  char origin[LAGHU_RUNTIME_PATH_SIZE];
  char path[LAGHU_RUNTIME_PATH_SIZE];
  char page_origin[LAGHU_RUNTIME_PATH_SIZE];
  bool enabled;
} laghu_html_url_base;

bool laghu_head_append(laghu_head_builder *builder, const void *data, size_t length);
bool laghu_head_equal(const unsigned char *value, size_t length, const char *expected);
bool laghu_html_ascii_space(unsigned char value);
bool laghu_html_contains_ci(const unsigned char *data, size_t length, const char *needle);
bool laghu_html_protected_element(const char *name);
bool laghu_head_tokenize(laghu_buffer html, laghu_head_token *tokens, size_t *token_count);
bool laghu_head_pair_tokens(laghu_head_token *tokens, size_t count);
bool laghu_html_trim_resource_urls(laghu_buffer html, const laghu_head_token *tokens, size_t token_count, const char *page_path,
                                   const char *page_origin, laghu_runtime_head_result *result);
bool laghu_html_lexical(laghu_buffer html, laghu_html_planner_mask plan, laghu_runtime_head_result *result);

#endif
