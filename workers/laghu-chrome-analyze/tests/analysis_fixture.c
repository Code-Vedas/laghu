// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/queue.h"

int main(int argc, char **argv) {
  laghu_runtime_queue queue;
  laghu_runtime_job job = {0};
  FILE *input;
  long length;
  unsigned char *source;
  int status = 1;
  if ((argc != 4 && argc != 5) || strcmp(argv[1], "--publish") != 0) return 2;
  input = fopen(argv[3], "rb");
  if (input == NULL || fseek(input, 0L, SEEK_END) != 0 || (length = ftell(input)) <= 0 || (size_t)length > 1024U * 1024U ||
      fseek(input, 0L, SEEK_SET) != 0 || (source = malloc((size_t)length)) == NULL)
    return 1;
  if (fread(source, 1U, (size_t)length, input) != (size_t)length) goto done;
  job.kind = LAGHU_RUNTIME_JOB_BROWSER_ANALYSIS;
  memset(job.index_key, 'a', sizeof(job.index_key) - 1U);
  memset(job.policy_key, 'b', sizeof(job.policy_key) - 1U);
  memset(job.validator, 'c', LAGHU_SHA256_HEX_LENGTH);
  memset(job.provider_digest, 'd', LAGHU_SHA256_HEX_LENGTH);
  strcpy(job.request_path, "/fixture.html");
  strcpy(job.content_type, "text/html");
  job.analysis_timeout_ms = argc == 5 ? (unsigned int)strtoul(argv[4], NULL, 10) : 1500U;
  job.payload = (laghu_buffer){source, (size_t)length};
  laghu_runtime_queue_init(&queue);
  if (laghu_runtime_queue_open(&queue, argv[2]) && laghu_runtime_queue_try_publish(&queue, &job)) status = 0;
  laghu_runtime_queue_close(&queue);
done:
  fclose(input);
  free(source);
  return status;
}
