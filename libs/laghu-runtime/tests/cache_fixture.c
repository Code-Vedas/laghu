// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/runtime.h"

static unsigned char *read_file(const char *path, size_t *length) {
  FILE *file = fopen(path, "rb");
  long size;
  unsigned char *data;
  if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
      (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0) {
    if (file != NULL) fclose(file);
    return NULL;
  }
  data = malloc((size_t)size);
  if (data == NULL || fread(data, 1U, (size_t)size, file) != (size_t)size) {
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *length = (size_t)size;
  return data;
}

int main(int argc, char **argv) {
  static const unsigned char body[] = "immutable-fixture";
  laghu_runtime_cache_entry entry;
  if (argc == 4 && strcmp(argv[1], "--queue") == 0) {
    laghu_runtime_queue queue;
    uint64_t now = (uint64_t)strtoull(argv[3], NULL, 10);
    bool ready;
    laghu_runtime_queue_init(&queue);
    ready = now != 0U &&
            laghu_runtime_queue_create(&queue, argv[2], 4U, 4096U) &&
            laghu_runtime_queue_set_backend(&queue, 1U, "fixture") &&
            laghu_runtime_queue_heartbeat(&queue, now);
    laghu_runtime_queue_close(&queue);
    return ready ? 0 : 1;
  }
  if (argc == 6 && strcmp(argv[1], "--file") == 0 &&
      strlen(argv[3]) == LAGHU_RUNTIME_KEY_SIZE - 1U) {
    size_t length = 0U;
    unsigned char *data = read_file(argv[5], &length);
    bool published =
        data != NULL && laghu_runtime_cache_publish(
                            argv[2], argv[3], argv[3], argv[3], argv[4],
                            "fixture", (laghu_buffer){data, length}, &entry);
    free(data);
    return published ? 0 : 1;
  }
  if (argc != 3 || strlen(argv[2]) != LAGHU_RUNTIME_KEY_SIZE - 1U) return 2;
  return laghu_runtime_cache_publish(
             argv[1], argv[2], argv[2], argv[2], "image/png", "fixture",
             (laghu_buffer){body, sizeof(body) - 1U}, &entry)
             ? 0
             : 1;
}
