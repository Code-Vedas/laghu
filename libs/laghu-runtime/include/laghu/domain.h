// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_DOMAIN_H
#define LAGHU_DOMAIN_H

#include <stdbool.h>
#include <stddef.h>

#include "laghu/core.h"
#include "laghu/types.h"

typedef struct {
  unsigned char *data;
  size_t length;
  bool rewritten;
} laghu_domain_rewrite_result;

/* Rewrites URL-bearing HTML attributes (including srcset and style) and CSS
 * url() values.
 * The result owns data only when rewritten is true. */
bool laghu_domain_rewrite_html(laghu_buffer input,
                               const laghu_domain_policy *policy,
                               laghu_domain_rewrite_result *result);
bool laghu_domain_rewrite_css(laghu_buffer input,
                              const laghu_domain_policy *policy,
                              laghu_domain_rewrite_result *result);
void laghu_domain_rewrite_result_release(laghu_domain_rewrite_result *result);

#endif
