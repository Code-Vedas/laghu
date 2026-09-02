// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "server_internal.h"

enum {
  CHUNK_FUZZ_MAX_INPUT = 2048U,
  CHUNK_FUZZ_MAX_WIRE = CHUNK_FUZZ_MAX_INPUT * 2U
};

static bool decode(const uint8_t *data, size_t size, bool split, unsigned char **body, size_t *length) {
  int sockets[2] = {LAGHU_INVALID_SOCKET, LAGHU_INVALID_SOCKET};
  size_t initial_length = split ? size / 2U : size;
  bool decoded;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return false;
  if (split && initial_length < size && send(sockets[1], data + initial_length, size - initial_length, 0) != (ssize_t)(size - initial_length)) {
    laghu_close(sockets[0]);
    laghu_close(sockets[1]);
    return false;
  }
  (void)shutdown(sockets[1], LAGHU_SHUT_WRITE);
  decoded = proxy_read_chunked_body(sockets[0], NULL, data, initial_length, body, length);
  laghu_close(sockets[0]);
  laghu_close(sockets[1]);
  return decoded;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  unsigned char wire[CHUNK_FUZZ_MAX_WIRE];
  unsigned char *complete_body = NULL;
  unsigned char *split_body = NULL;
  size_t complete_length = 0U;
  size_t split_length = 0U;
  size_t wire_length = 0U;
  size_t index;
  bool complete;
  bool split;
  if (size > CHUNK_FUZZ_MAX_INPUT) return 0;
  /* Corpus files are text-safe LF form. Mutations still retain arbitrary
   * non-LF bytes, while this makes valid CRLF framing reproducible in Git. */
  for (index = 0U; index < size; ++index) {
    if (data[index] == '\n') wire[wire_length++] = '\r';
    wire[wire_length++] = data[index];
  }
  complete = decode(wire, wire_length, false, &complete_body, &complete_length);
  split = decode(wire, wire_length, true, &split_body, &split_length);
  /* Invalid, truncated, and post-message bytes may be rejected or left for
   * the next stream consumer depending on receive boundaries.  A successful
   * decode must still own a bounded body, and equivalent successful decodes
   * must produce identical bytes. */
  if ((complete && (complete_body == NULL || complete_length > LAGHU_PROXY_MAX_BODY)) ||
      (split && (split_body == NULL || split_length > LAGHU_PROXY_MAX_BODY)) ||
      (complete && split && (complete_length != split_length || memcmp(complete_body, split_body, complete_length) != 0)))
    abort();
  free(complete_body);
  free(split_body);
  return 0;
}
