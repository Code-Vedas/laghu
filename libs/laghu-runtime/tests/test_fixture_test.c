// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "test_fixture.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static bool fixture_path_exists(const char *path) {
  struct stat status;
  return stat(path, &status) == 0;
}

int main(void) {
  static const unsigned char data[] = {0U, 1U, 2U, 3U};
  laghu_test_workspace first, second;
  laghu_test_queue_pair pair;
  char path[LAGHU_RUNTIME_PATH_SIZE], root[LAGHU_RUNTIME_PATH_SIZE];
  char uri[LAGHU_RUNTIME_PATH_SIZE + 16U];
  unsigned char readback[sizeof(data)];
  FILE *file;
  assert(laghu_test_workspace_create(&first));
  assert(laghu_test_workspace_create(&second));
  assert(strcmp(first.path, second.path) != 0);
  assert(laghu_test_workspace_path(&first, "nested/data.bin", path, sizeof(path)));
  assert(laghu_test_workspace_write(&first, "nested/data.bin", data, sizeof(data)));
  assert(fixture_path_exists(path));
  file = fopen(path, "rb");
  assert(file != NULL && fread(readback, 1U, sizeof(readback), file) == sizeof(readback) && fclose(file) == 0);
  assert(memcmp(readback, data, sizeof(data)) == 0);
  assert(laghu_test_workspace_write(&first, "empty", NULL, 0U));
  assert(!laghu_test_workspace_write(&first, "short", NULL, 1U));
  assert(!laghu_test_workspace_path(&first, "", path, sizeof(path)));
  assert(!laghu_test_workspace_path(&first, "/absolute", path, sizeof(path)));
  assert(!laghu_test_workspace_path(&first, "../escape", path, sizeof(path)));
  assert(!laghu_test_workspace_path(&first, "nested/../escape", path, sizeof(path)));
  assert(!laghu_test_workspace_path(&first, "nested\\escape", path, sizeof(path)));
  assert(laghu_test_cache_uri(first.path, uri, sizeof(uri)));
  assert(strncmp(uri, "file:///", sizeof("file:///") - 1U) == 0);
  assert(!laghu_test_cache_uri("relative", uri, sizeof(uri)));
  assert(laghu_test_cache_uri("/tmp/cache?x=1", uri, sizeof(uri)));
  assert(strcmp(uri, "file:///tmp/cache%3Fx=1") == 0);
  assert(strstr(uri, "%3F") != NULL);
  assert(laghu_test_queue_pair_open(&pair, &first, "queue/jobs.queue", 2U, 64U));
  laghu_test_queue_pair_close(&pair);
  assert(!laghu_test_queue_pair_open(&pair, &first, "queue/invalid.queue", 0U, 64U));
  assert(!pair.producer_open && !pair.consumer_open);
  assert(snprintf(root, sizeof(root), "%s", first.path) > 0);
  assert(laghu_test_workspace_remove(&first));
  assert(!fixture_path_exists(root));
  assert(laghu_test_workspace_remove(&first));
  assert(laghu_test_workspace_remove(&second));
  puts("laghu_runtime_fixture_test: all tests passed");
  return 0;
}
