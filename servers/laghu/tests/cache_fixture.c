// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdlib.h>
#include <string.h>

#include "laghu/runtime.h"

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
  if (argc != 3 || strlen(argv[2]) != LAGHU_RUNTIME_KEY_SIZE - 1U) return 2;
  return laghu_runtime_cache_publish(
             argv[1], argv[2], argv[2], argv[2], "image/png", "fixture",
             (laghu_buffer){body, sizeof(body) - 1U}, &entry)
             ? 0
             : 1;
}
