// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_LAYOUT_H
#define LAGHU_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>

#include "laghu/csp.h"
#include "laghu/html.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_LAYOUT_RESERVATION_MAX_RULES 32U
#define LAGHU_LAYOUT_RESERVATION_ID_SIZE 128U

typedef enum { LAGHU_LAYOUT_RESERVATION_BOX = 1, LAGHU_LAYOUT_RESERVATION_FONT = 2 } laghu_layout_reservation_kind;

typedef struct {
  laghu_layout_reservation_kind kind;
  char id[LAGHU_LAYOUT_RESERVATION_ID_SIZE];
  unsigned int width;
  unsigned int height;
  unsigned int font_size_adjust_milli;
} laghu_layout_reservation_rule;

typedef struct {
  laghu_layout_reservation_rule rules[LAGHU_LAYOUT_RESERVATION_MAX_RULES];
  unsigned int count;
} laghu_layout_reservation_set;

bool laghu_layout_reservations_load(const char *path, laghu_layout_reservation_set *set, char *error, size_t error_size);
bool laghu_layout_reservations_apply(laghu_buffer html, const laghu_layout_reservation_set *set, const laghu_csp_policy *csp,
                                     laghu_runtime_html_result *result);

#ifdef __cplusplus
}
#endif

#endif
