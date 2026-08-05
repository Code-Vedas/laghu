// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_MARKUP_H
#define LAGHU_MARKUP_H

#include "laghu/base.h"

typedef struct {
  laghu_buffer source;
  laghu_buffer name;
  size_t start;
  size_t end;
  bool closing;
  bool self_closing;
} laghu_html_tag;

typedef struct {
  laghu_buffer name;
  laghu_buffer value;
  size_t start;
  size_t end;
  bool has_value;
} laghu_html_attribute;

typedef struct {
  laghu_buffer url;
  laghu_buffer descriptor;
} laghu_srcset_candidate;

bool laghu_html_next_tag(laghu_buffer html, size_t *cursor,
                         laghu_html_tag *tag);
bool laghu_html_tag_attribute(const laghu_html_tag *tag, const char *name,
                              laghu_html_attribute *attribute);
bool laghu_html_tag_has_attribute(const laghu_html_tag *tag, const char *name);
bool laghu_srcset_next(laghu_buffer srcset, size_t *cursor,
                       laghu_srcset_candidate *candidate);

#endif
