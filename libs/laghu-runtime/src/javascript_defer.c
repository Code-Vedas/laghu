// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "laghu/instrumentation.h"
#include "laghu/javascript.h"
#include "laghu/rum.h"
#include "laghu/types.h"

static bool laghu_defer_path(const char *value) {
  size_t i;
  if (value == NULL || value[0] != '/' || value[1] == '/' ||
      strpbrk(value, "?#\\*[]") != NULL || strstr(value, "..") != NULL)
    return false;
  for (i = 0U; value[i] != '\0'; ++i)
    if ((unsigned char)value[i] <= 32U || (unsigned char)value[i] >= 127U)
      return false;
  return i > 1U && i < LAGHU_RUNTIME_PATH_SIZE;
}

bool laghu_javascript_defer_load(const char *path,
                                 laghu_javascript_defer_set *set, char *error,
                                 size_t error_size) {
  FILE *file;
  char line[2200U], material[65536U];
  size_t used = 0U;
  unsigned int line_no = 0U;
  if (path == NULL || set == NULL || (file = fopen(path, "rb")) == NULL) {
    if (error != NULL) snprintf(error, error_size, "cannot open defer config");
    return false;
  }
  memset(set, 0, sizeof(*set));
  while (fgets(line, sizeof(line), file) != NULL) {
    char script[LAGHU_RUNTIME_PATH_SIZE], scope[LAGHU_RUNTIME_PATH_SIZE] = "";
    char second[LAGHU_RUNTIME_PATH_SIZE + 16U] = "", extra[2U];
    unsigned int fields, i;
    int length;
    ++line_no;
    if (line[0] == '#' || strspn(line, " \t\r\n") == strlen(line)) continue;
    fields = (unsigned int)sscanf(line, " defer %1023s %1039s %1s", script,
                                  second, extra);
    if ((fields != 1U && fields != 2U) || !laghu_defer_path(script) ||
        set->count == LAGHU_JAVASCRIPT_DEFER_MAX_RULES)
      goto invalid;
    if (fields == 2U) {
      if (strncmp(second, "template=", 9U) != 0 ||
          !laghu_defer_path(second + 9U))
        goto invalid;
      strcpy(scope, second + 9U);
    }
    for (i = 0U; i < set->count; ++i)
      if (strcmp(set->rules[i].script_path, script) == 0 &&
          strcmp(set->rules[i].template_path, scope) == 0)
        goto invalid;
    strcpy(set->rules[set->count].script_path, script);
    strcpy(set->rules[set->count].template_path, scope);
    length = snprintf(material + used, sizeof(material) - used,
                      "defer %s template=%s\n", script, scope);
    if (length <= 0 || (size_t)length >= sizeof(material) - used) goto invalid;
    used += (size_t)length;
    ++set->count;
  }
  if (ferror(file) || fclose(file) != 0 ||
      !laghu_sha256_hex((laghu_buffer){(const unsigned char *)material, used},
                        set->digest)) {
    memset(set, 0, sizeof(*set));
    return false;
  }
  return true;
invalid:
  fclose(file);
  if (error != NULL)
    snprintf(error, error_size, "line %u: invalid defer directive", line_no);
  memset(set, 0, sizeof(*set));
  return false;
}

bool laghu_javascript_defer_approved(const laghu_javascript_defer_set *set,
                                     const char *script_path,
                                     const char *template_path) {
  unsigned int i;
  if (set == NULL || script_path == NULL || template_path == NULL) return false;
  for (i = 0U; i < set->count; ++i)
    if (strcmp(set->rules[i].script_path, script_path) == 0 &&
        (set->rules[i].template_path[0] == '\0' ||
         strcmp(set->rules[i].template_path, template_path) == 0))
      return true;
  return false;
}

bool laghu_javascript_defer_recommended(
    const laghu_rum_instrumentation_record *record, unsigned int bucket,
    unsigned int script_index) {
  uint64_t observations, candidate;
  if (record == NULL || record->version != LAGHU_INSTRUMENTATION_VERSION ||
      bucket > 1U || script_index >= record->script_count)
    return false;
  observations = record->observations[bucket];
  candidate = record->script_observations[bucket][script_index];
  return observations >= 100U && candidate <= UINT64_MAX / 10U &&
         observations <= UINT64_MAX / 9U &&
         candidate * 10U >= observations * 9U;
}

static uint64_t laghu_defer_rate(uint64_t count, uint64_t observations) {
  if (observations == 0U) return 0U;
  if (count >= observations || count > UINT64_MAX / 10000U) return 10000U;
  return count * 10000U / observations;
}

static uint64_t laghu_defer_add(uint64_t left, uint64_t right) {
  return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static unsigned int laghu_defer_p75(const uint64_t histogram[8],
                                    uint64_t observations) {
  uint64_t cumulative = 0U;
  uint64_t threshold = observations - observations / 4U;
  unsigned int index;
  for (index = 0U; index < 8U; ++index) {
    cumulative = UINT64_MAX - cumulative < histogram[index]
                     ? UINT64_MAX
                     : cumulative + histogram[index];
    if (cumulative >= threshold) return index;
  }
  return 7U;
}

bool laghu_javascript_defer_rollback_recommended(
    const laghu_rum_instrumentation_record *baseline,
    const laghu_rum_instrumentation_record *current, unsigned int bucket) {
  uint64_t baseline_observations, current_observations;
  uint64_t baseline_error_rate, current_error_rate;
  unsigned int metric;
  if (baseline == NULL || current == NULL || bucket > 1U ||
      baseline->version != LAGHU_INSTRUMENTATION_VERSION ||
      current->version != LAGHU_INSTRUMENTATION_VERSION)
    return false;
  baseline_observations = baseline->observations[bucket];
  current_observations = current->observations[bucket];
  if (baseline_observations < 100U || current_observations < 50U) return false;
  baseline_error_rate = laghu_defer_rate(
      laghu_defer_add(baseline->errors[bucket], baseline->rejections[bucket]),
      baseline_observations);
  current_error_rate = laghu_defer_rate(
      laghu_defer_add(current->errors[bucket], current->rejections[bucket]),
      current_observations);
  if (current_error_rate >= baseline_error_rate * 2U &&
      current_error_rate >= baseline_error_rate + 100U)
    return true;
  for (metric = 0U; metric < LAGHU_RUM_HISTOGRAMS; ++metric)
    if (laghu_defer_p75(current->histograms[bucket][metric],
                        current_observations) >
        laghu_defer_p75(baseline->histograms[bucket][metric],
                        baseline_observations))
      return true;
  for (metric = 3U; metric < 5U; ++metric) {
    uint64_t before =
        baseline->metric_sums[bucket][metric] / baseline_observations;
    uint64_t after =
        current->metric_sums[bucket][metric] / current_observations;
    if (after >= before + 100U && after >= before + before / 10U) return true;
  }
  return false;
}
