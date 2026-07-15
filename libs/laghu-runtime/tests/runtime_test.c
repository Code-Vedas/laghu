// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef _WIN32
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "laghu/runtime.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

int main(void) {
  static const unsigned char payload[] = "runtime payload";
  char temporary[LAGHU_RUNTIME_PATH_SIZE];
  char queue_path[LAGHU_RUNTIME_PATH_SIZE];
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char no_webp_index_key[LAGHU_RUNTIME_KEY_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned char received[64];
  unsigned char cached[64];
  laghu_runtime_queue producer = {0};
  laghu_runtime_queue consumer = {0};
  laghu_runtime_job submitted;
  laghu_runtime_job taken;
  laghu_runtime_cache_entry entry;

  laghu_runtime_queue_init(&producer);
  laghu_runtime_queue_init(&consumer);
#ifdef _WIN32
  {
    char base[LAGHU_RUNTIME_PATH_SIZE];
    assert(GetTempPathA(sizeof(base), base) > 0U);
    assert(snprintf(temporary, sizeof(temporary), "%slaghu-runtime-%lu", base,
                    (unsigned long)GetCurrentProcessId()) > 0);
    assert(_mkdir(temporary) == 0 || errno == EEXIST);
  }
#else
  strcpy(temporary, "/tmp/laghu-runtime-XXXXXX");
  assert(mkdtemp(temporary) != NULL);
#endif
  assert(snprintf(queue_path, sizeof(queue_path), "%s/jobs.queue", temporary) >
         0);
  assert(laghu_sha256_hex((laghu_buffer){payload, sizeof(payload) - 1U},
                          policy_key));
  assert(laghu_runtime_index_key("/image.png", "etag", policy_key, true,
                                 index_key));
  assert(laghu_runtime_index_key("/image.png", "etag", policy_key, false,
                                 no_webp_index_key));
  assert(strcmp(index_key, no_webp_index_key) != 0);
  assert(laghu_runtime_queue_create(&producer, queue_path, 1U, 64U));
  assert(laghu_runtime_queue_set_backend(&producer, 0x55aaU, "test-vips"));
  assert(laghu_runtime_queue_heartbeat(&producer, 123456U));
  assert(laghu_runtime_queue_open(&consumer, queue_path));
  assert(consumer.capabilities == 0x55aaU);
  assert(consumer.worker_heartbeat == 123456U);
  assert(strcmp(consumer.backend_id, "test-vips") == 0);
  assert(laghu_runtime_queue_heartbeat(&producer, 123457U));
  assert(laghu_runtime_queue_refresh(&consumer));
  assert(consumer.worker_heartbeat == 123457U);
  memset(&submitted, 0, sizeof(submitted));
  strcpy(submitted.index_key, index_key);
  strcpy(submitted.request_path, "/image.png");
  strcpy(submitted.validator, "etag");
  strcpy(submitted.content_type, "image/png");
  strcpy(submitted.policy_key, policy_key);
  submitted.quality = 82U;
  submitted.payload = (laghu_buffer){payload, sizeof(payload) - 1U};
  assert(laghu_runtime_queue_try_publish(&producer, &submitted));
  assert(!laghu_runtime_queue_try_publish(&producer, &submitted));
  assert(laghu_runtime_queue_try_take(&consumer, &taken, received,
                                      sizeof(received)));
  assert(taken.payload.length == sizeof(payload) - 1U);
  assert(memcmp(taken.payload.data, payload, taken.payload.length) == 0);
  assert(laghu_runtime_cache_publish(temporary, index_key, policy_key, "etag",
                                     "image/png", "test-backend", taken.payload,
                                     &entry));
  assert(!laghu_runtime_cache_publish(NULL, index_key, policy_key, "etag",
                                      "image/png", "test-backend",
                                      taken.payload, &entry));
  assert(!laghu_runtime_cache_lookup(temporary, index_key, NULL, &entry));
  assert(laghu_runtime_cache_lookup(temporary, index_key, "etag", &entry));
  assert(laghu_runtime_cache_read(&entry, cached, sizeof(cached)));
  assert(memcmp(cached, payload, sizeof(payload) - 1U) == 0);
  assert(!laghu_runtime_cache_lookup(temporary, index_key, "changed", &entry));
  {
    unsigned char corrupted = (unsigned char)(payload[0] ^ 0xffU);
    FILE *file = fopen(entry.variant_path, "r+b");
    assert(file != NULL);
    assert(fwrite(&corrupted, 1U, 1U, file) == 1U);
    assert(fclose(file) == 0);
  }
  assert(laghu_runtime_cache_lookup(temporary, index_key, "etag", &entry));
  assert(!laghu_runtime_cache_read(&entry, cached, sizeof(cached)));
  {
    FILE *file = fopen(entry.variant_path, "wb");
    assert(file != NULL);
    assert(fwrite(payload, 1U, 1U, file) == 1U);
    assert(fclose(file) == 0);
  }
  assert(!laghu_runtime_cache_lookup(temporary, index_key, "etag", &entry));
  laghu_runtime_queue_close(&consumer);
  laghu_runtime_queue_close(&producer);
  puts("laghu_runtime_test: all tests passed");
  return 0;
}
