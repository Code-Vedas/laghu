// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "laghu/profile.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  static const char key[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  laghu_rum_options options;
  laghu_rum_engine *rum;
  laghu_rum_instrumentation_record record = {0};
  laghu_template_profile profile;

  laghu_rum_options_init(&options);
  options.store_uri = "memory:";
  options.ttl_seconds = 600U;
  rum = laghu_rum_engine_create(&options, NULL, 0U);
  assert(rum != NULL);
  assert(laghu_template_profile_decide(rum, key, 100U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_NONE && !profile.apply);

  record.version = LAGHU_INSTRUMENTATION_VERSION;
  strcpy(record.template_key, key);
  record.updated_at = 100U;
  record.observations[1] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES - 1U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 100U, &record, sizeof(record), NULL));
  assert(laghu_template_profile_decide(rum, key, 101U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_NONE && !profile.apply);

  record.observations[1] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES;
  record.histograms[1][0][2] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES;
  record.histograms[1][1][2] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES;
  record.histograms[1][2][2] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES;
  record.updated_at = 102U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 102U, &record, sizeof(record), NULL));
  assert(laghu_template_profile_decide(rum, key, 103U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_LEARNED && profile.apply && profile.lcp_over_budget && profile.inp_over_budget &&
         profile.cls_over_budget && profile.observations == LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES);

  memset(record.histograms, 0, sizeof(record.histograms));
  record.histograms[1][0][1] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES;
  record.histograms[1][1][1] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES;
  record.histograms[1][2][1] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES;
  record.updated_at = 104U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 104U, &record, sizeof(record), NULL));
  assert(laghu_template_profile_decide(rum, key, 105U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_LEARNED && !profile.apply && !profile.lcp_over_budget && !profile.inp_over_budget &&
         !profile.cls_over_budget);

  record.errors[1] = 1U;
  record.updated_at = 106U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 106U, &record, sizeof(record), NULL));
  assert(laghu_template_profile_decide(rum, key, 107U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_REGRESSION && !profile.apply);

  record.errors[1] = 0U;
  record.updated_at = 108U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 108U, &record, sizeof(record), NULL));
  assert(laghu_template_profile_decide(rum, key, 169U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_STALE && !profile.apply);

  /* Chrome contributes only a validated LCP candidate; it must not fabricate
   * an observation or any CWV timing sample. */
  memset(&record, 0, sizeof(record));
  record.version = LAGHU_INSTRUMENTATION_VERSION;
  strcpy(record.template_key, key);
  record.updated_at = 200U;
  record.media_count = 1U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 200U, &record, sizeof(record), NULL));
  assert(laghu_runtime_apply_chrome_analysis(
      rum,
      (laghu_buffer){(const unsigned char *)"{\"version\":1,\"viewport\":{\"width\":1440},\"lcp_ordinal\":0,\"template\":\""
                                            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}",
                     sizeof("{\"version\":1,\"viewport\":{\"width\":1440},\"lcp_ordinal\":0,\"template\":\""
                            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}") -
                         1U},
      201U, 60U));
  assert(laghu_rum_engine_read(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 201U, &record, sizeof(record), NULL));
  assert(record.lcp_observations[2] == 1U && record.lcp_candidates[2][0] == 1U && record.observations[1] == 0U);

  {
    char directory[] = "/tmp/laghu-chrome-import.XXXXXX";
    char path[sizeof(directory) + 16U];
    FILE *report;
    record.updated_at = 202U;
    assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 202U, &record, sizeof(record), NULL));
    assert(mkdtemp(directory) != NULL);
    assert(snprintf(path, sizeof(path), "%s/report.json", directory) > 0);
    report = fopen(path, "wb");
    assert(report != NULL);
    assert(fputs("{\"viewport\":{\"width\":1440},\"lcp_ordinal\":0,\"template\":\""
                 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}",
                 report) >= 0);
    assert(fclose(report) == 0);
    assert(laghu_runtime_import_chrome_analysis(rum, directory, 203U, 60U) == 1U);
    assert(access(path, F_OK) != 0);
    assert(rmdir(directory) == 0);
  }
  laghu_rum_engine_destroy(rum);
  return 0;
}
