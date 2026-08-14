// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>
#include <string.h>

#include "laghu/queue.h"
#include "laghu/trace.h"

int main(int argc, char **argv) {
  laghu_runtime_queue queue;
  laghu_runtime_job job;
  laghu_trace_context trace;
  char payload[1024U];
  size_t length;
  assert(argc == 2 || (argc == 3 && strcmp(argv[1], "--publish") == 0));
  laghu_runtime_queue_init(&queue);
  assert(argc == 2 ? laghu_runtime_queue_create(&queue, argv[1], 8U, 4096U) : laghu_runtime_queue_open(&queue, argv[2]));
  assert(laghu_trace_context_root(100U, &trace));
  assert(laghu_trace_otlp_json(&trace, "laghu.request", 1U, 2U, "ok", payload, sizeof(payload), &length));
  memset(&job, 0, sizeof(job));
  job.kind = LAGHU_RUNTIME_JOB_TRACE_EXPORT;
  job.trace = trace;
  job.payload = (laghu_buffer){(const unsigned char *)payload, length};
  assert(laghu_runtime_queue_try_publish(&queue, &job));
  laghu_runtime_queue_close(&queue);
  return 0;
}
