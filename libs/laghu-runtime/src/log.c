// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#define _POSIX_C_SOURCE 200809L

#include "laghu/log.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  char *output;
  size_t capacity;
  size_t used;
} laghu_log_builder;

static bool laghu_log_append(laghu_log_builder *builder, const char *value) {
  size_t length = strlen(value);
  if (builder->used + length >= builder->capacity) return false;
  memcpy(builder->output + builder->used, value, length);
  builder->used += length;
  builder->output[builder->used] = '\0';
  return true;
}

static bool laghu_log_number(laghu_log_builder *builder, uint64_t value) {
  char text[32U];
  int length = snprintf(text, sizeof(text), "%" PRIu64, value);
  return length > 0 && (size_t)length < sizeof(text) &&
         laghu_log_append(builder, text);
}

static bool laghu_log_string(laghu_log_builder *builder, const char *value,
                             size_t limit, bool *truncated) {
  static const char hexadecimal[] = "0123456789abcdef";
  size_t index = 0U;
  if (!laghu_log_append(builder, "\"")) return false;
  if (value == NULL) value = "";
  while (value[index] != '\0' && index < limit) {
    unsigned char byte = (unsigned char)value[index++];
    char escaped[7U] = {0};
    if (byte == '"')
      strcpy(escaped, "\\\"");
    else if (byte == '\\')
      strcpy(escaped, "\\\\");
    else if (byte == '\b')
      strcpy(escaped, "\\b");
    else if (byte == '\f')
      strcpy(escaped, "\\f");
    else if (byte == '\n')
      strcpy(escaped, "\\n");
    else if (byte == '\r')
      strcpy(escaped, "\\r");
    else if (byte == '\t')
      strcpy(escaped, "\\t");
    else if (byte < 32U || byte > 126U) {
      escaped[0] = '\\';
      escaped[1] = 'u';
      escaped[2] = '0';
      escaped[3] = '0';
      escaped[4] = hexadecimal[byte >> 4U];
      escaped[5] = hexadecimal[byte & 15U];
    } else {
      escaped[0] = (char)byte;
    }
    if (!laghu_log_append(builder, escaped)) return false;
  }
  if (truncated != NULL && value[index] != '\0') *truncated = true;
  return laghu_log_append(builder, "\"");
}

static bool laghu_log_timestamp(time_t now, char output[32U]) {
  struct tm value;
#if defined(_WIN32)
  if (gmtime_s(&value, &now) != 0) return false;
#else
  if (gmtime_r(&now, &value) == NULL) return false;
#endif
  return strftime(output, 32U, "%Y-%m-%dT%H:%M:%SZ", &value) != 0U;
}

static bool laghu_log_common_render(laghu_log_builder *builder,
                                    const laghu_log_common *common,
                                    const char *event) {
  char timestamp[32U];
  if (common == NULL || !laghu_log_timestamp(common->timestamp, timestamp) ||
      !laghu_log_append(
          builder, "{\"schema\":\"" LAGHU_LOG_SCHEMA "\",\"timestamp\":\"") ||
      !laghu_log_append(builder, timestamp) ||
      !laghu_log_append(builder, "\",\"surface\":"))
    return false;
  if (!laghu_log_string(builder, common->surface, 32U, NULL) ||
      !laghu_log_append(builder, ",\"component\":") ||
      !laghu_log_string(builder, common->component, 32U, NULL) ||
      !laghu_log_append(builder, ",\"event\":") ||
      !laghu_log_string(builder, event, 16U, NULL)) return false;
  if (common->trace_id != NULL && common->trace_id[0] != '\0') {
    if (!laghu_log_append(builder, ",\"trace_id\":") ||
        !laghu_log_string(builder, common->trace_id, 32U, NULL))
      return false;
  }
  if (common->span_id != NULL && common->span_id[0] != '\0') {
    if (!laghu_log_append(builder, ",\"span_id\":") ||
        !laghu_log_string(builder, common->span_id, 16U, NULL))
      return false;
  }
  return true;
}

bool laghu_log_render_transaction(const laghu_log_transaction *record,
                                  char *output, size_t capacity) {
  laghu_log_builder builder = {output, capacity, 0U};
  const char *query;
  size_t path_length;
  bool path_truncated = false;
  char path[LAGHU_LOG_PATH_SIZE + 1U];
  if (record == NULL || output == NULL || capacity == 0U) return false;
  output[0] = '\0';
  query = record->path == NULL ? NULL : strchr(record->path, '?');
  path_length = query == NULL
                    ? (record->path == NULL ? 0U : strlen(record->path))
                    : (size_t)(query - record->path);
  if (path_length == 0U) {
    strcpy(path, "/");
  } else {
    if (path_length > LAGHU_LOG_PATH_SIZE) {
      path_length = LAGHU_LOG_PATH_SIZE;
      path_truncated = true;
    }
    memcpy(path, record->path, path_length);
    path[path_length] = '\0';
  }
  return laghu_log_common_render(&builder, &record->common, "transaction") &&
         laghu_log_append(&builder, ",\"method\":") &&
         laghu_log_string(&builder, record->method, 16U, NULL) &&
         laghu_log_append(&builder, ",\"path\":") &&
         laghu_log_string(&builder, path, LAGHU_LOG_PATH_SIZE,
                          &path_truncated) &&
         laghu_log_append(&builder, ",\"path_truncated\":") &&
         laghu_log_append(&builder, path_truncated ? "true" : "false") &&
         laghu_log_append(&builder, ",\"status\":") &&
         laghu_log_number(&builder, record->status) &&
         laghu_log_append(&builder, ",\"decision\":") &&
         laghu_log_string(&builder, record->decision, 64U, NULL) &&
         laghu_log_append(&builder, ",\"request_bytes\":") &&
         laghu_log_number(&builder, record->request_bytes) &&
         laghu_log_append(&builder, ",\"original_bytes\":") &&
         laghu_log_number(&builder, record->original_bytes) &&
         laghu_log_append(&builder, ",\"output_bytes\":") &&
         laghu_log_number(&builder, record->output_bytes) &&
         laghu_log_append(&builder, ",\"duration_ms\":") &&
         laghu_log_number(&builder, record->duration_ms) &&
         laghu_log_append(&builder, ",\"cache\":") &&
         laghu_log_string(&builder, record->cache, 16U, NULL) &&
         laghu_log_append(&builder, ",\"job_published\":") &&
         laghu_log_append(&builder, record->job_published ? "true" : "false") &&
         laghu_log_append(&builder, ",\"failure\":") &&
         laghu_log_string(&builder, record->failure, 64U, NULL) &&
         laghu_log_append(&builder, "}");
}

bool laghu_log_render_lifecycle(const laghu_log_lifecycle *record, char *output,
                                size_t capacity) {
  laghu_log_builder builder = {output, capacity, 0U};
  if (record == NULL || output == NULL || capacity == 0U) return false;
  output[0] = '\0';
  return laghu_log_common_render(&builder, &record->common, "lifecycle") &&
         laghu_log_append(&builder, ",\"state\":") &&
         laghu_log_string(&builder, record->state, 16U, NULL) &&
         laghu_log_append(&builder, ",\"failure\":") &&
         laghu_log_string(&builder, record->failure, 64U, NULL) &&
         laghu_log_append(&builder, "}");
}

bool laghu_log_render_job(const laghu_log_job *record, char *output,
                          size_t capacity) {
  laghu_log_builder builder = {output, capacity, 0U};
  if (record == NULL || output == NULL || capacity == 0U) return false;
  output[0] = '\0';
  return laghu_log_common_render(&builder, &record->common, "job") &&
         laghu_log_append(&builder, ",\"job_kind\":") &&
         laghu_log_string(&builder, record->job_kind, 32U, NULL) &&
         laghu_log_append(&builder, ",\"outcome\":") &&
         laghu_log_string(&builder, record->outcome, 16U, NULL) &&
         laghu_log_append(&builder, ",\"input_bytes\":") &&
         laghu_log_number(&builder, record->input_bytes) &&
         laghu_log_append(&builder, ",\"output_bytes\":") &&
         laghu_log_number(&builder, record->output_bytes) &&
         laghu_log_append(&builder, ",\"duration_ms\":") &&
         laghu_log_number(&builder, record->duration_ms) &&
         laghu_log_append(&builder, ",\"failure\":") &&
         laghu_log_string(&builder, record->failure, 64U, NULL) &&
         laghu_log_append(&builder, "}");
}
