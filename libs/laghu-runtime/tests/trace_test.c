// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/trace.h"

#include <assert.h>
#include <string.h>

int main(void) {
  laghu_trace_context parent, child;
  char traceparent[56U], json[1024U];
  size_t length;
  assert(!laghu_trace_context_parse("00-00000000000000000000000000000000-0123456789abcdef-01", NULL, &parent));
  assert(laghu_trace_context_parse("00-0123456789abcdef0123456789abcdef-0123456789abcdef-01", "vendor=value", &parent));
  assert(parent.sampled && laghu_trace_context_traceparent(&parent, traceparent));
  assert(strncmp(traceparent, "00-0123456789abcdef0123456789abcdef-", 36U) == 0);
  assert(laghu_trace_context_child(&parent, &child) && strcmp(child.parent_span_id, parent.span_id) == 0);
  assert(laghu_trace_otlp_json(&child, "laghu.worker.image", 100U, 101U, "ok", json, sizeof(json), &length));
  assert(length != 0U && strstr(json, "laghu.worker.image") != NULL && strstr(json, "vendor=value") == NULL);
  assert(laghu_trace_context_root(0U, &child) && !child.sampled);
  assert(!laghu_trace_otlp_json(&child, "laghu.request", 1U, 2U, "ok", json, sizeof(json), &length));
  return 0;
}
