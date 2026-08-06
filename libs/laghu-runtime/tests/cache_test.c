// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/cache.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "test_fixture.h"

static void laghu_test_hash(char output[LAGHU_RUNTIME_KEY_SIZE], char value) {
  memset(output, value, LAGHU_RUNTIME_KEY_SIZE - 1U);
  output[LAGHU_RUNTIME_KEY_SIZE - 1U] = '\0';
}

int main(void) {
  uint64_t bytes = 0U;
  unsigned int seconds = 0U;
  char root[LAGHU_RUNTIME_PATH_SIZE], index[LAGHU_RUNTIME_KEY_SIZE];
  char variant[LAGHU_RUNTIME_KEY_SIZE], metadata[LAGHU_RUNTIME_PATH_SIZE];
  unsigned char payload[] = "cache artifact";
  unsigned char output[sizeof(payload)];
  laghu_runtime_cache_entry entry;
  FILE *file;
  assert(laghu_cache_size_parse("32m", 4U, UINT64_MAX, &bytes));
  assert(bytes == 32U * 1024U * 1024U);
  assert(!laghu_cache_size_parse("32mbogus", 4U, UINT64_MAX, &bytes));
  assert(laghu_cache_duration_parse("45s", 1U, 60U, &seconds));
  assert(seconds == 45U);
  assert(laghu_test_directory(root, sizeof(root)));
  laghu_test_hash(index, 'a');
  laghu_test_hash(variant, 'b');
  assert(laghu_runtime_file_cache_publish(
      root, index, variant, "etag", "text/plain", "test",
      (laghu_buffer){payload, sizeof(payload) - 1U}, &entry));
  assert(laghu_runtime_file_cache_lookup(root, index, "etag", &entry));
  assert(laghu_runtime_file_cache_read(&entry, output, sizeof(output)));
  assert(memcmp(output, payload, sizeof(payload) - 1U) == 0);
  assert(snprintf(metadata, sizeof(metadata), "%s/index-%s.meta", root, index) >
         0);
  file = fopen(metadata, "r+b");
  assert(file != NULL && fputc(0, file) != EOF && fclose(file) == 0);
  assert(!laghu_runtime_file_cache_lookup(root, index, "etag", &entry));
  return 0;
}
