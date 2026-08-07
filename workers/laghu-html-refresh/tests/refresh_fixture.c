// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "laghu/html_cache.h"
#include "laghu/queue.h"

static int laghu_html_refresh_publish_job(int argc, char **argv) {
  laghu_runtime_job job = {0};
  laghu_runtime_queue queue;
  char key[LAGHU_RUNTIME_KEY_SIZE];
  int path_length, validator_length;
  if (argc != 6 || !laghu_html_cache_key(argv[3], argv[4], key)) return 2;
  job.kind = LAGHU_RUNTIME_JOB_HTML_REFRESH;
  memcpy(job.index_key, key, sizeof(job.index_key));
  memcpy(job.policy_key, key, sizeof(job.policy_key));
  path_length =
      snprintf(job.request_path, sizeof(job.request_path), "%s", argv[4]);
  validator_length =
      snprintf(job.validator, sizeof(job.validator), "%s", argv[5]);
  if (path_length < 0 || (size_t)path_length >= sizeof(job.request_path) ||
      validator_length < 0 || (size_t)validator_length >= sizeof(job.validator))
    return 1;
  laghu_runtime_queue_init(&queue);
  if (!laghu_runtime_queue_open(&queue, argv[2]) ||
      !laghu_runtime_queue_try_publish(&queue, &job)) {
    laghu_runtime_queue_close(&queue);
    return 1;
  }
  laghu_runtime_queue_close(&queue);
  return 0;
}

static int laghu_html_refresh_assert_cached(int argc, char **argv) {
  laghu_runtime_cache_entry entry;
  char key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned char *body;
  size_t expected_length;
  int result = 1;
  if (argc != 6 || !laghu_html_cache_key(argv[3], argv[4], key)) return 2;
  expected_length = strlen(argv[5]);
  if (!laghu_runtime_cache_lookup_variant(argv[2], key, &entry) ||
      entry.length != expected_length ||
      (body = malloc(entry.length == 0U ? 1U : entry.length)) == NULL)
    return 1;
  if (laghu_runtime_cache_read(&entry, body, entry.length) &&
      memcmp(body, argv[5], expected_length) == 0)
    result = 0;
  free(body);
  return result;
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--job") == 0)
    return laghu_html_refresh_publish_job(argc, argv);
  if (argc > 1 && strcmp(argv[1], "--cached") == 0)
    return laghu_html_refresh_assert_cached(argc, argv);
  fputs(
      "Usage: refresh_fixture --job|--cached QUEUE_OR_CACHE ORIGIN PATH "
      "VALUE\n",
      stderr);
  return 2;
}
