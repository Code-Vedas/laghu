// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_TRACE_H
#define LAGHU_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_TRACE_ID_SIZE 33U
#define LAGHU_TRACE_SPAN_ID_SIZE 17U
#define LAGHU_TRACE_FLAGS_SIZE 3U
#define LAGHU_TRACE_STATE_SIZE 257U

typedef struct {
  char trace_id[LAGHU_TRACE_ID_SIZE];
  char parent_span_id[LAGHU_TRACE_SPAN_ID_SIZE];
  char span_id[LAGHU_TRACE_SPAN_ID_SIZE];
  char flags[LAGHU_TRACE_FLAGS_SIZE];
  char tracestate[LAGHU_TRACE_STATE_SIZE];
  bool sampled;
} laghu_trace_context;

/* Parses W3C trace context. Invalid input deliberately produces no context. */
bool laghu_trace_context_parse(const char *traceparent, const char *tracestate, laghu_trace_context *context);
/* Starts a root span using the platform CSPRNG; never falls back to predictable IDs. */
bool laghu_trace_context_root(unsigned int sampling_rate, laghu_trace_context *context);
bool laghu_trace_context_child(const laghu_trace_context *parent, laghu_trace_context *child);
bool laghu_trace_context_traceparent(const laghu_trace_context *context, char output[56U]);
/* Encodes one bounded OTLP/HTTP JSON span. Names and attributes are fixed,
 * deliberately excluding headers, authorities, queries, bodies and keys. */
bool laghu_trace_otlp_json(const laghu_trace_context *context, const char *name, uint64_t start_unix_nano, uint64_t end_unix_nano,
                           const char *outcome, char *output, size_t capacity, size_t *length);

#ifdef __cplusplus
}
#endif

#endif
