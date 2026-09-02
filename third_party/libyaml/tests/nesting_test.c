// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yaml.h>

#define LAGHU_LIBYAML_NESTING_LIMIT 1000U

static bool scan_input(const char *input, size_t length, bool expected, bool expect_depth_error) {
  yaml_parser_t parser;
  yaml_token_t token;
  if (!yaml_parser_initialize(&parser)) return false;
  yaml_parser_set_input_string(&parser, (const unsigned char *)input, length);
  for (;;) {
    if (!yaml_parser_scan(&parser, &token)) {
      bool result = !expected && (!expect_depth_error ||
                                  (parser.error == YAML_SCANNER_ERROR && parser.problem != NULL && !strcmp(parser.problem, "exceeded maximum nesting depth")));
      yaml_parser_delete(&parser);
      return result;
    }
    if (token.type == YAML_STREAM_END_TOKEN) {
      yaml_token_delete(&token);
      yaml_parser_delete(&parser);
      return expected;
    }
    yaml_token_delete(&token);
  }
}

static bool flow_nesting_test(size_t depth, bool expected) {
  char *input = malloc(depth * 2U + 1U);
  bool result;
  if (input == NULL) return false;
  memset(input, '[', depth);
  memset(input + depth, ']', depth);
  input[depth * 2U] = '\0';
  result = scan_input(input, depth * 2U, expected, !expected);
  free(input);
  return result;
}

static bool block_nesting_test(size_t depth, bool expected) {
  char *input;
  size_t capacity = depth * (depth + 5U) / 2U + 1U;
  size_t index, used = 0U;
  bool result;
  input = malloc(capacity);
  if (input == NULL) return false;
  for (index = 0U; index < depth; ++index) {
    memset(input + used, ' ', index);
    used += index;
    input[used++] = 'x';
    input[used++] = ':';
    input[used++] = '\n';
  }
  result = scan_input(input, used, expected, !expected);
  free(input);
  return result;
}

int main(void) {
  static const char aliases[] = "base: &base [one, two]\ncopy: *base\n";
  if (!flow_nesting_test(LAGHU_LIBYAML_NESTING_LIMIT, true)) {
    fputs("flow nesting at limit failed\n", stderr);
    return 1;
  }
  if (!flow_nesting_test(LAGHU_LIBYAML_NESTING_LIMIT + 1U, false)) {
    fputs("flow nesting over limit failed\n", stderr);
    return 1;
  }
  /* Block parsing retains the document root in the indentation stack. */
  if (!block_nesting_test(LAGHU_LIBYAML_NESTING_LIMIT - 1U, true)) {
    fputs("block nesting near limit failed\n", stderr);
    return 1;
  }
  if (!block_nesting_test(LAGHU_LIBYAML_NESTING_LIMIT + 1U, false)) {
    fputs("block nesting over limit failed\n", stderr);
    return 1;
  }
  if (!scan_input(aliases, sizeof(aliases) - 1U, true, false)) {
    fputs("aliases failed\n", stderr);
    return 1;
  }
  return 0;
}
