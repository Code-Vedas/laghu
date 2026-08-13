// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "laghu/cache.h"
#include "laghu/fonts.h"
#include "laghu/queue.h"
#include "laghu/types.h"

static unsigned char *read_file(const char *path, size_t *length) {
  FILE *file = fopen(path, "rb");
  long size;
  unsigned char *data;
  if (file == NULL || fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0) {
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
  if (argc == 5 && strcmp(argv[1], "--font-job") == 0) {
    laghu_font_provider_set providers;
    const laghu_font_provider *provider;
    laghu_runtime_queue queue;
    laghu_runtime_job job = {0};
    char error[128];
    if (!laghu_font_providers_load(argv[3], &providers, error, sizeof(error)) ||
        (provider = laghu_font_provider_match(&providers, argv[4])) == NULL || !laghu_font_stylesheet_key(argv[4], provider->digest, job.index_key))
      return 1;
    job.kind = LAGHU_RUNTIME_JOB_FONT_CSS;
    memcpy(job.policy_key, job.index_key, sizeof(job.policy_key));
    (void)snprintf(job.request_path, sizeof(job.request_path), "%s", argv[4]);
    (void)snprintf(job.content_type, sizeof(job.content_type), "text/css");
    (void)snprintf(job.provider_id, sizeof(job.provider_id), "%s", provider->id);
    memcpy(job.provider_digest, provider->digest, sizeof(job.provider_digest));
    laghu_runtime_queue_init(&queue);
    if (!laghu_runtime_queue_open(&queue, argv[2]) || !laghu_runtime_queue_try_publish(&queue, &job)) {
      laghu_runtime_queue_close(&queue);
      return 1;
    }
    laghu_runtime_queue_close(&queue);
    return 0;
  }
  if (argc == 5 && strcmp(argv[1], "--font-ready") == 0) {
    laghu_font_provider_set providers;
    const laghu_font_provider *provider;
    laghu_font_stylesheet_record record;
    char error[128];
    if (!laghu_font_providers_load(argv[3], &providers, error, sizeof(error)) ||
        (provider = laghu_font_provider_match(&providers, argv[4])) == NULL ||
        !laghu_font_stylesheet_lookup(argv[2], argv[4], provider, (uint64_t)time(NULL), &record) || !record.ready)
      return 1;
    return 0;
  }
  if (argc == 4 && strcmp(argv[1], "--queue") == 0) {
    laghu_runtime_queue queue;
    uint64_t now = (uint64_t)strtoull(argv[3], NULL, 10);
    bool ready;
    laghu_runtime_queue_init(&queue);
    ready = now != 0U && laghu_runtime_queue_create(&queue, argv[2], 4U, 4096U) && laghu_runtime_queue_set_backend(&queue, 1U, "fixture") &&
            laghu_runtime_queue_heartbeat(&queue, now);
    laghu_runtime_queue_close(&queue);
    return ready ? 0 : 1;
  }
  if (argc == 6 && strcmp(argv[1], "--file") == 0 && strlen(argv[3]) == LAGHU_RUNTIME_KEY_SIZE - 1U) {
    size_t length = 0U;
    unsigned char *data = read_file(argv[5], &length);
    bool published =
        data != NULL && laghu_runtime_cache_publish(argv[2], argv[3], argv[3], argv[3], argv[4], "fixture", (laghu_buffer){data, length}, &entry);
    free(data);
    return published ? 0 : 1;
  }
  if (argc != 3 || strlen(argv[2]) != LAGHU_RUNTIME_KEY_SIZE - 1U) return 2;
  return laghu_runtime_cache_publish(argv[1], argv[2], argv[2], argv[2], "image/png", "fixture", (laghu_buffer){body, sizeof(body) - 1U}, &entry) ? 0
                                                                                                                                                  : 1;
}
