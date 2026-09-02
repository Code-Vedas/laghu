// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/precompressed.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "test_fixture.h"

int main(void) {
  char root[LAGHU_RUNTIME_PATH_SIZE];
  char failed_root[LAGHU_RUNTIME_PATH_SIZE];
  char failure_path[LAGHU_RUNTIME_PATH_SIZE];
  unsigned char body[2048U];
  unsigned char output[sizeof(body)];
  laghu_runtime_cache_entry entry;
  laghu_cache_stats stats;
  laghu_precompressed_coding coding;
  unsigned int index;
  memset(body, 'a', sizeof(body));
  assert(laghu_test_directory(root, sizeof(root)));
  assert(laghu_cache_backend_register_path(root, NULL));
  assert(laghu_precompressed_text_type("text/html; charset=utf-8"));
  assert(!laghu_precompressed_text_type("image/png"));
  assert(laghu_precompressed_publish(root, (laghu_buffer){body, sizeof(body)}, "text/html", "etag"));
  assert(laghu_cache_backend_health_path(root, &stats));
  assert(stats.publications == 2U);
  /* A matching warm response does no second gzip/Brotli publication. */
  assert(laghu_precompressed_publish(root, (laghu_buffer){body, sizeof(body)}, "text/html", "etag"));
  assert(laghu_cache_backend_health_path(root, &stats));
  assert(stats.publications == 2U);
  /* Validator and representation metadata are part of the process-local key. */
  assert(laghu_precompressed_publish(root, (laghu_buffer){body, sizeof(body)}, "text/html", "etag-next"));
  assert(laghu_cache_backend_health_path(root, &stats));
  assert(stats.publications == 4U);
  assert(laghu_precompressed_publish(root, (laghu_buffer){body, sizeof(body)}, "application/json", "etag-next"));
  assert(laghu_cache_backend_health_path(root, &stats));
  assert(stats.publications == 6U);
  assert(laghu_precompressed_select(root, (laghu_buffer){body, sizeof(body)}, "gzip", &entry, &coding));
  assert(coding == LAGHU_PRECOMPRESSED_GZIP);
  assert(laghu_runtime_cache_read(&entry, output, sizeof(output)));
  assert(entry.length < sizeof(body));
  assert(laghu_precompressed_select(root, (laghu_buffer){body, sizeof(body)}, "br, gzip", &entry, &coding));
  assert(coding == LAGHU_PRECOMPRESSED_BROTLI);
  assert(laghu_precompressed_select(root, (laghu_buffer){body, sizeof(body)}, "deflate, gzip, br, zstd", &entry, &coding));
  assert(coding == LAGHU_PRECOMPRESSED_BROTLI);
  assert(!laghu_precompressed_select(root, (laghu_buffer){body, sizeof(body)}, "identity", &entry, &coding));
  assert(laghu_precompressed_select(root, (laghu_buffer){body, sizeof(body)}, "gzip;q=0, br;q=0.5", &entry, &coding));
  assert(coding == LAGHU_PRECOMPRESSED_BROTLI);

  /* A cache invalidation observed by select clears matching warm-index entries. */
  assert(laghu_precompressed_select(root, (laghu_buffer){body, sizeof(body)}, "gzip", &entry, &coding));
  assert(remove(entry.variant_path) == 0);
  assert(!laghu_precompressed_select(root, (laghu_buffer){body, sizeof(body)}, "gzip", &entry, &coding));
  assert(laghu_cache_backend_health_path(root, &stats));
  {
    uint64_t publications = stats.publications;
    assert(laghu_precompressed_publish(root, (laghu_buffer){body, sizeof(body)}, "text/html", "etag"));
    assert(laghu_cache_backend_health_path(root, &stats));
    assert(stats.publications == publications + 1U);
  }

  /* A failed publish never enters the index and can succeed on a retry. */
  assert(laghu_test_directory(failed_root, sizeof(failed_root)));
  assert(snprintf(failure_path, sizeof(failure_path), "%s/not-a-directory", failed_root) > 0);
  {
    FILE *file = fopen(failure_path, "wb");
    assert(file != NULL);
    assert(fclose(file) == 0);
  }
  assert(laghu_precompressed_publish(failure_path, (laghu_buffer){body, sizeof(body)}, "text/html", "retry"));
  assert(!laghu_precompressed_select(failure_path, (laghu_buffer){body, sizeof(body)}, "gzip", &entry, &coding));
  assert(remove(failure_path) == 0);
  assert(mkdir(failure_path, 0750) == 0);
  assert(laghu_precompressed_publish(failure_path, (laghu_buffer){body, sizeof(body)}, "text/html", "retry"));
  assert(laghu_precompressed_select(failure_path, (laghu_buffer){body, sizeof(body)}, "gzip", &entry, &coding));

  /* 64 individual variants are retained; deterministic LRU eviction retries the oldest pair. */
  for (index = 0U; index < 32U; ++index) {
    body[0] = (unsigned char)index;
    assert(laghu_precompressed_publish(root, (laghu_buffer){body, sizeof(body)}, "text/html", "bounded"));
  }
  memset(body, 'a', sizeof(body));
  assert(laghu_cache_backend_health_path(root, &stats));
  {
    uint64_t publications = stats.publications;
    assert(laghu_precompressed_publish(root, (laghu_buffer){body, sizeof(body)}, "text/html", "etag"));
    assert(laghu_cache_backend_health_path(root, &stats));
    assert(stats.publications == publications + 2U);
  }
  return 0;
}
