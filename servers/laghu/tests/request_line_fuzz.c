// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "server_internal.h"

static bool parse_request(const uint8_t *data, size_t size) {
  proxy_request request;
  memset(&request, 0, sizeof(request));
  memcpy(request.storage, data, size);
  request.storage[size] = '\0';
  return proxy_parse_request(&request, size);
}

static bool parse_request_line(const uint8_t *data, size_t size) {
  static const char headers[] = "\r\nHost: fuzz.test\r\n\r\n";
  proxy_request request;
  if (size > LAGHU_PROXY_LINE_BYTES) return false;
  if (size != 0U && data[size - 1U] == '\n') --size;
  memset(&request, 0, sizeof(request));
  memcpy(request.storage, data, size);
  memcpy(request.storage + size, headers, sizeof(headers));
  return proxy_parse_request(&request, size + sizeof(headers) - 1U);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > LAGHU_PROXY_HEADER_BYTES) return 0;
  if (parse_request(data, size) != parse_request(data, size)) abort();
  if (size <= LAGHU_PROXY_LINE_BYTES && parse_request_line(data, size) != parse_request_line(data, size)) abort();
  return 0;
}
