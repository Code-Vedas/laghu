// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/queue.h"

#include <assert.h>
#include <stdio.h>

#include "test_fixture.h"

int main(void) {
  char root[LAGHU_RUNTIME_PATH_SIZE], path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_runtime_queue queue;
  uint64_t capacity = 0U, occupied = 0U;
  assert(laghu_test_directory(root, sizeof(root)));
  assert(snprintf(path, sizeof(path), "%s/jobs.queue", root) > 0);
  laghu_runtime_queue_init(&queue);
  assert(laghu_runtime_queue_create(&queue, path, 4U, 64U));
  assert(laghu_runtime_queue_status(&queue, &capacity, &occupied));
  assert(capacity == 4U && occupied == 0U);
  laghu_runtime_queue_close(&queue);
  return 0;
}
