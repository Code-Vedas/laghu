// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/instrumentation.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LAGHU_CHROME_ANALYSIS_MAX_JSON 16384U

typedef struct {
  char template_key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int ordinal;
  unsigned int bucket;
  uint64_t now;
  unsigned int ttl_seconds;
} laghu_chrome_analysis_context;

static const char *laghu_chrome_field(const char *json, const char *name) {
  char needle[48U];
  const char *value;
  if (snprintf(needle, sizeof(needle), "\"%s\"", name) < 0 || (value = strstr(json, needle)) == NULL) return NULL;
  value += strlen(needle);
  while (isspace((unsigned char)*value)) ++value;
  if (*value++ != ':') return NULL;
  while (isspace((unsigned char)*value)) ++value;
  return value;
}

static bool laghu_chrome_uint(const char *json, const char *name, unsigned int maximum, unsigned int *output) {
  const char *value = laghu_chrome_field(json, name);
  char *end;
  unsigned long number;
  if (value == NULL || !isdigit((unsigned char)*value)) return false;
  number = strtoul(value, &end, 10);
  if (end == value || number > maximum) return false;
  *output = (unsigned int)number;
  return true;
}

static bool laghu_chrome_key(const char *json, char output[LAGHU_RUNTIME_KEY_SIZE]) {
  const char *value = laghu_chrome_field(json, "template");
  const char *end;
  if (value == NULL || *value++ != '\"' || (end = strchr(value, '\"')) == NULL || (size_t)(end - value) != LAGHU_SHA256_HEX_LENGTH)
    return false;
  memcpy(output, value, LAGHU_SHA256_HEX_LENGTH);
  output[LAGHU_SHA256_HEX_LENGTH] = '\0';
  for (size_t index = 0U; index < LAGHU_SHA256_HEX_LENGTH; ++index)
    if (!isxdigit((unsigned char)output[index])) return false;
  return true;
}

static bool laghu_chrome_merge(void *data, size_t length, void *opaque) {
  laghu_rum_instrumentation_record *record = data;
  const laghu_chrome_analysis_context *context = opaque;
  unsigned int lcp_bucket;
  if (length != sizeof(*record) || record->version != LAGHU_INSTRUMENTATION_VERSION || strcmp(record->template_key, context->template_key) != 0 ||
      context->now < record->updated_at || context->now - record->updated_at > context->ttl_seconds || context->ordinal >= record->media_count)
    return false;
  lcp_bucket = context->bucket * 2U;
  if (record->lcp_observations[lcp_bucket] != UINT16_MAX) ++record->lcp_observations[lcp_bucket];
  if (record->lcp_candidates[lcp_bucket][context->ordinal] != UINT16_MAX) ++record->lcp_candidates[lcp_bucket][context->ordinal];
  record->updated_at = context->now;
  return true;
}

bool laghu_runtime_apply_chrome_analysis(laghu_rum_engine *rum, laghu_buffer json, uint64_t now, unsigned int ttl_seconds) {
  laghu_chrome_analysis_context context;
  char *text;
  unsigned int viewport = 0U;
  bool result;
  if (rum == NULL || json.data == NULL || json.length == 0U || json.length > LAGHU_CHROME_ANALYSIS_MAX_JSON || ttl_seconds == 0U ||
      memchr(json.data, '\0', json.length) != NULL)
    return false;
  text = malloc(json.length + 1U);
  if (text == NULL) return false;
  memcpy(text, json.data, json.length);
  text[json.length] = '\0';
  memset(&context, 0, sizeof(context));
  result = laghu_chrome_key(text, context.template_key) &&
           laghu_chrome_uint(text, "lcp_ordinal", LAGHU_LCP_MAX_CANDIDATES - 1U, &context.ordinal) &&
           laghu_chrome_uint(text, "width", 100000U, &viewport);
  if (result) {
    context.bucket = viewport < 768U ? 0U : 1U;
    context.now = now;
    context.ttl_seconds = ttl_seconds;
    result = laghu_rum_engine_update(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, context.template_key, now, laghu_chrome_merge, &context, NULL);
  }
  free(text);
  return result;
}

unsigned int laghu_runtime_import_chrome_analysis(laghu_rum_engine *rum, const char *directory, uint64_t now, unsigned int ttl_seconds) {
  DIR *stream;
  struct dirent *entry;
  unsigned int imported = 0U, attempted = 0U;
  if (rum == NULL || directory == NULL || directory[0] == '\0' || (stream = opendir(directory)) == NULL) return 0U;
  while (attempted < 8U && (entry = readdir(stream)) != NULL) {
    char path[LAGHU_RUNTIME_PATH_SIZE * 2U];
    struct stat status;
    unsigned char payload[LAGHU_CHROME_ANALYSIS_MAX_JSON];
    ssize_t length;
    int descriptor;
    size_t name_length = strlen(entry->d_name);
    if (name_length <= 5U || strcmp(entry->d_name + name_length - 5U, ".json") != 0 ||
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >= (int)sizeof(path))
      continue;
    ++attempted;
    descriptor = open(path, O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0 || fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0 ||
        status.st_size > (off_t)sizeof(payload)) {
      if (descriptor >= 0) (void)close(descriptor);
      continue;
    }
    length = read(descriptor, payload, (size_t)status.st_size);
    (void)close(descriptor);
    if (length != status.st_size || !laghu_runtime_apply_chrome_analysis(rum, (laghu_buffer){payload, (size_t)length}, now, ttl_seconds)) continue;
    if (unlink(path) == 0) ++imported;
  }
  (void)closedir(stream);
  return imported;
}
