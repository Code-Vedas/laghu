// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/precompressed.h"

#include <assert.h>
#include <string.h>

#include "test_fixture.h"

int main(void) {
  char root[LAGHU_RUNTIME_PATH_SIZE];
  unsigned char body[2048U];
  unsigned char output[sizeof(body)];
  laghu_runtime_cache_entry entry;
  laghu_precompressed_coding coding;
  memset(body, 'a', sizeof(body));
  assert(laghu_test_directory(root, sizeof(root)));
  assert(laghu_precompressed_text_type("text/html; charset=utf-8"));
  assert(!laghu_precompressed_text_type("image/png"));
  assert(laghu_precompressed_publish(root, (laghu_buffer){body, sizeof(body)},
                                     "text/html", "etag"));
  assert(laghu_precompressed_select(root, (laghu_buffer){body, sizeof(body)},
                                    "gzip", &entry, &coding));
  assert(coding == LAGHU_PRECOMPRESSED_GZIP);
  assert(laghu_runtime_cache_read(&entry, output, sizeof(output)));
  assert(entry.length < sizeof(body));
  assert(laghu_precompressed_select(root, (laghu_buffer){body, sizeof(body)},
                                    "br, gzip", &entry, &coding));
  assert(coding == LAGHU_PRECOMPRESSED_BROTLI);
  assert(laghu_precompressed_select(root, (laghu_buffer){body, sizeof(body)},
                                    "deflate, gzip, br, zstd", &entry,
                                    &coding));
  assert(coding == LAGHU_PRECOMPRESSED_BROTLI);
  assert(!laghu_precompressed_select(root, (laghu_buffer){body, sizeof(body)},
                                     "identity", &entry, &coding));
  assert(laghu_precompressed_select(root, (laghu_buffer){body, sizeof(body)},
                                    "gzip;q=0, br;q=0.5", &entry, &coding));
  assert(coding == LAGHU_PRECOMPRESSED_BROTLI);
  return 0;
}
