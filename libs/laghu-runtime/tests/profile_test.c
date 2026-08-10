// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/profile.h"

#include <assert.h>
#include <string.h>

int main(void) {
  static const char key[] =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
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
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key,
                                  100U, &record, sizeof(record), NULL));
  assert(laghu_template_profile_decide(rum, key, 101U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_NONE && !profile.apply);

  record.observations[1] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES;
  record.updated_at = 102U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key,
                                  102U, &record, sizeof(record), NULL));
  assert(laghu_template_profile_decide(rum, key, 103U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_LEARNED && profile.apply &&
         profile.observations == LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES);

  record.errors[1] = 1U;
  record.updated_at = 104U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key,
                                  104U, &record, sizeof(record), NULL));
  assert(laghu_template_profile_decide(rum, key, 105U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_REGRESSION &&
         !profile.apply);

  record.errors[1] = 0U;
  record.updated_at = 106U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key,
                                  106U, &record, sizeof(record), NULL));
  assert(laghu_template_profile_decide(rum, key, 167U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_STALE && !profile.apply);
  laghu_rum_engine_destroy(rum);
  return 0;
}
