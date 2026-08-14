// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/trace.h"

#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
#include <stdlib.h>
#else
#include <sys/random.h>
#endif

static bool laghu_trace_hex(const char *value, size_t length, bool nonzero) {
  size_t index;
  unsigned char any = 0U;
  if (value == NULL) return false;
  for (index = 0U; index < length; ++index) {
    unsigned char c = (unsigned char)value[index];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    any |= c != '0';
  }
  return !nonzero || any != 0U;
}

static void laghu_trace_encode(const unsigned char *input, size_t length, char *output) {
  static const char digits[] = "0123456789abcdef";
  size_t index;
  for (index = 0U; index < length; ++index) {
    output[index * 2U] = digits[input[index] >> 4U];
    output[index * 2U + 1U] = digits[input[index] & 15U];
  }
  output[length * 2U] = '\0';
}

static bool laghu_trace_random(unsigned char *output, size_t length) {
#if defined(__APPLE__)
  arc4random_buf(output, length);
  return true;
#else
  return getrandom(output, length, 0) == (ssize_t)length;
#endif
}

static bool laghu_trace_new_span_id(char output[LAGHU_TRACE_SPAN_ID_SIZE]) {
  unsigned char bytes[8U];
  if (!laghu_trace_random(bytes, sizeof(bytes)) || memcmp(bytes, "\0\0\0\0\0\0\0\0", sizeof(bytes)) == 0) return false;
  laghu_trace_encode(bytes, sizeof(bytes), output);
  return true;
}

bool laghu_trace_context_parse(const char *traceparent, const char *tracestate, laghu_trace_context *context) {
  if (context == NULL || traceparent == NULL || strlen(traceparent) != 55U || memcmp(traceparent, "00-", 3U) != 0 || traceparent[35] != '-' ||
      traceparent[52] != '-' || !laghu_trace_hex(traceparent + 3U, 32U, true) || !laghu_trace_hex(traceparent + 36U, 16U, true) ||
      !laghu_trace_hex(traceparent + 53U, 2U, false) || (tracestate != NULL && strlen(tracestate) >= LAGHU_TRACE_STATE_SIZE))
    return false;
  memset(context, 0, sizeof(*context));
  memcpy(context->trace_id, traceparent + 3U, 32U);
  memcpy(context->parent_span_id, traceparent + 36U, 16U);
  memcpy(context->flags, traceparent + 53U, 2U);
  if (tracestate != NULL) memcpy(context->tracestate, tracestate, strlen(tracestate) + 1U);
  context->sampled = (traceparent[54] & 1) != 0;
  return laghu_trace_new_span_id(context->span_id);
}

bool laghu_trace_context_root(unsigned int sampling_rate, laghu_trace_context *context) {
  unsigned char trace[16U];
  if (context == NULL || sampling_rate > 100U || !laghu_trace_random(trace, sizeof(trace)) ||
      memcmp(trace, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", sizeof(trace)) == 0)
    return false;
  memset(context, 0, sizeof(*context));
  laghu_trace_encode(trace, sizeof(trace), context->trace_id);
  context->sampled = sampling_rate != 0U && (unsigned int)trace[0] % 100U < sampling_rate;
  memcpy(context->flags, context->sampled ? "01" : "00", 3U);
  return laghu_trace_new_span_id(context->span_id);
}

bool laghu_trace_context_child(const laghu_trace_context *parent, laghu_trace_context *child) {
  if (parent == NULL || child == NULL || !laghu_trace_hex(parent->trace_id, 32U, true) || !laghu_trace_hex(parent->span_id, 16U, true)) return false;
  memset(child, 0, sizeof(*child));
  memcpy(child->trace_id, parent->trace_id, sizeof(child->trace_id));
  memcpy(child->parent_span_id, parent->span_id, sizeof(child->parent_span_id));
  memcpy(child->flags, parent->flags, sizeof(child->flags));
  memcpy(child->tracestate, parent->tracestate, sizeof(child->tracestate));
  child->sampled = parent->sampled;
  return laghu_trace_new_span_id(child->span_id);
}

bool laghu_trace_context_traceparent(const laghu_trace_context *context, char output[56U]) {
  if (context == NULL || output == NULL || !laghu_trace_hex(context->trace_id, 32U, true) || !laghu_trace_hex(context->span_id, 16U, true) ||
      !laghu_trace_hex(context->flags, 2U, false))
    return false;
  (void)snprintf(output, 56U, "00-%s-%s-%s", context->trace_id, context->span_id, context->flags);
  return true;
}

bool laghu_trace_otlp_json(const laghu_trace_context *context, const char *name, uint64_t start_unix_nano, uint64_t end_unix_nano,
                           const char *outcome, char *output, size_t capacity, size_t *length) {
  int written;
  if (context == NULL || name == NULL || outcome == NULL || output == NULL || length == NULL || !context->sampled ||
      end_unix_nano < start_unix_nano ||
      strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != strlen(name) ||
      strspn(outcome, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != strlen(outcome))
    return false;
  written = snprintf(output, capacity,
                     "{\"resourceSpans\":[{\"scopeSpans\":[{\"spans\":[{\"traceId\":\"%s\",\"spanId\":\"%s\",\"parentSpanId\":\"%s\",\"name\":\"%s\",\"kind\":1,"
                     "\"startTimeUnixNano\":\"%llu\",\"endTimeUnixNano\":\"%llu\",\"attributes\":[{\"key\":\"laghu.outcome\",\"value\":{\"stringValue\":\"%s\"}}]}]}]}]}",
                     context->trace_id, context->span_id, context->parent_span_id, name, (unsigned long long)start_unix_nano,
                     (unsigned long long)end_unix_nano, outcome);
  if (written < 0 || (size_t)written >= capacity) return false;
  *length = (size_t)written;
  return true;
}
