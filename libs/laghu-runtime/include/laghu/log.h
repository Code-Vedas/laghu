// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_LOG_H
#define LAGHU_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_LOG_SCHEMA "laghu-log-v1"
#define LAGHU_LOG_LINE_SIZE 4096U
#define LAGHU_LOG_PATH_SIZE 1024U

typedef struct {
  time_t timestamp;
  const char *surface;
  const char *component;
} laghu_log_common;

typedef struct {
  laghu_log_common common;
  const char *method;
  const char *path;
  unsigned int status;
  const char *decision;
  uint64_t request_bytes;
  uint64_t original_bytes;
  uint64_t output_bytes;
  uint64_t duration_ms;
  const char *cache;
  bool job_published;
  const char *failure;
} laghu_log_transaction;

typedef struct {
  laghu_log_common common;
  const char *state;
  const char *failure;
} laghu_log_lifecycle;

typedef struct {
  laghu_log_common common;
  const char *job_kind;
  const char *outcome;
  uint64_t input_bytes;
  uint64_t output_bytes;
  uint64_t duration_ms;
  const char *failure;
} laghu_log_job;

bool laghu_log_render_transaction(const laghu_log_transaction *record,
                                  char *output, size_t capacity);
bool laghu_log_render_lifecycle(const laghu_log_lifecycle *record, char *output,
                                size_t capacity);
bool laghu_log_render_job(const laghu_log_job *record, char *output,
                          size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
