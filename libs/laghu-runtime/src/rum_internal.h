// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_RUM_INTERNAL_H
#define LAGHU_RUM_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "laghu/rum.h"

bool laghu_rum_encode(laghu_rum_record_type type, const void *record,
                      size_t length, unsigned char *data, size_t capacity,
                      size_t *encoded_length);
bool laghu_rum_decode(laghu_rum_record_type type, const unsigned char *data,
                      size_t length, void *record, size_t record_size);
bool laghu_rum_same_identity(laghu_rum_record_type type, const void *left,
                             const void *right, size_t length);
void laghu_rum_zero_observations(laghu_rum_record_type type, void *data,
                                 size_t length);

#endif
