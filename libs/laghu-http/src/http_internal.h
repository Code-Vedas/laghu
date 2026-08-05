// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_HTTP_INTERNAL_H
#define LAGHU_HTTP_INTERNAL_H

#include "laghu/http.h"

bool laghu_http_view_valid(laghu_buffer value);
bool laghu_http_header_name_equal(laghu_buffer name, const char *expected);
void laghu_http_result_init(laghu_http_transaction_result *result);
bool laghu_http_add_header_operation(laghu_http_transaction_result *result,
                                     laghu_http_header_operation_kind kind,
                                     const char *name, const char *value);
bool laghu_http_add_status(laghu_http_transaction_result *result,
                           laghu_decision decision);
bool laghu_http_add_length(laghu_http_transaction_result *result,
                           size_t length);
bool laghu_http_join_response_headers(const laghu_http_response *response,
                                      const char *name, char *output,
                                      size_t capacity);

#endif
