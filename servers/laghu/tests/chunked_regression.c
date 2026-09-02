// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server_internal.h"

typedef struct {
  const char *name;
  const unsigned char *body;
  size_t body_length;
  bool valid;
} chunked_fixture;

static bool read_fixture(const char *directory, const char *name, unsigned char **data, size_t *length) {
  char path[1024];
  FILE *file;
  long size;
  unsigned char *input;
  if (snprintf(path, sizeof(path), "%s/%s", directory, name) < 0 || strlen(path) >= sizeof(path)) return false;
  file = fopen(path, "rb");
  if (file == NULL || fseek(file, 0L, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0L, SEEK_SET) != 0) {
    if (file != NULL) fclose(file);
    return false;
  }
  input = malloc((size_t)size == 0U ? 1U : (size_t)size);
  if (input == NULL || fread(input, 1U, (size_t)size, file) != (size_t)size || fclose(file) != 0) {
    free(input);
    return false;
  }
  *data = input;
  *length = (size_t)size;
  return true;
}

static bool crlf_wire(const unsigned char *input, size_t input_length, unsigned char **wire, size_t *wire_length) {
  unsigned char *output;
  size_t index, output_length = 0U;
  if (input_length > SIZE_MAX / 2U) return false;
  output = malloc(input_length == 0U ? 1U : input_length * 2U);
  if (output == NULL) return false;
  for (index = 0U; index < input_length; ++index) {
    if (input[index] == '\n') output[output_length++] = '\r';
    output[output_length++] = input[index];
  }
  *wire = output;
  *wire_length = output_length;
  return true;
}

static bool send_all(laghu_socket socket, const unsigned char *data, size_t length) {
  while (length != 0U) {
    ssize_t sent = send(socket, data, length, 0);
    if (sent > 0) {
      data += (size_t)sent;
      length -= (size_t)sent;
    } else if (sent < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

static bool decode_split(const unsigned char *wire, size_t wire_length, size_t split, unsigned char **body, size_t *body_length) {
  laghu_socket sockets[2] = {LAGHU_INVALID_SOCKET, LAGHU_INVALID_SOCKET};
  bool decoded = false;
  if (split > wire_length || socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return false;
  if (!send_all(sockets[1], wire + split, wire_length - split) || shutdown(sockets[1], LAGHU_SHUT_WRITE) != 0) goto done;
  decoded = proxy_read_chunked_body(sockets[0], NULL, wire, split, body, body_length);
done:
  laghu_close(sockets[0]);
  laghu_close(sockets[1]);
  return decoded;
}

static bool run_fixture(const char *directory, const chunked_fixture *fixture) {
  unsigned char *input = NULL;
  unsigned char *wire = NULL;
  size_t input_length = 0U, wire_length = 0U, split;
  bool passed = false;
  if (!read_fixture(directory, fixture->name, &input, &input_length) || !crlf_wire(input, input_length, &wire, &wire_length)) goto done;
  if (!fixture->valid) {
    unsigned char *body = NULL;
    size_t body_length = 0U;
    passed = !decode_split(wire, wire_length, wire_length, &body, &body_length);
    free(body);
    goto done;
  }
  for (split = 0U; split <= wire_length; ++split) {
    unsigned char *body = NULL;
    size_t body_length = 0U;
    if (!decode_split(wire, wire_length, split, &body, &body_length) || body_length != fixture->body_length ||
        memcmp(body, fixture->body, body_length) != 0) {
      fprintf(stderr, "%s: split %zu failed\n", fixture->name, split);
      free(body);
      goto done;
    }
    free(body);
  }
  passed = true;
done:
  free(wire);
  free(input);
  return passed;
}

int main(int argc, char **argv) {
  static const unsigned char wikipedia[] = "Wikipedia";
  static const unsigned char hello[] = "hello";
  static const chunked_fixture fixtures[] = {
      {"valid.txt", wikipedia, sizeof(wikipedia) - 1U, true},
      {"extension-trailer.txt", hello, sizeof(hello) - 1U, true},
      {"malformed-size.txt", NULL, 0U, false},
      {"trailing-bytes.txt", NULL, 0U, false},
      {"truncated.txt", NULL, 0U, false},
  };
  size_t index;
  if (argc != 2) return 2;
  for (index = 0U; index < sizeof(fixtures) / sizeof(fixtures[0]); ++index) {
    if (!run_fixture(argv[1], &fixtures[index])) {
      fprintf(stderr, "chunked regression failed: %s\n", fixtures[index].name);
      return 1;
    }
  }
  puts("laghu chunked parser regression passed");
  return 0;
}
