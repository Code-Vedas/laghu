// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/profile.h"

#include <string.h>

static bool laghu_template_profile_unhealthy(
    const laghu_rum_instrumentation_record *record, unsigned int bucket) {
  uint64_t observations;
  uint64_t allowed_failures;
  if (record == NULL || bucket > 1U) return true;
  observations = record->observations[bucket];
  /* A profile must stay below one failure per twenty observations.  Checking
   * each counter separately avoids overflow in a hostile aggregate. */
  allowed_failures = observations / 20U;
  return record->errors[bucket] > allowed_failures ||
         record->rejections[bucket] > allowed_failures;
}

bool laghu_template_profile_decide(laghu_rum_engine *rum,
                                   const char *template_key, uint64_t now,
                                   unsigned int ttl_seconds,
                                   unsigned int viewport_bucket,
                                   laghu_template_profile *profile) {
  laghu_rum_instrumentation_record record;
  laghu_rum_value value;
  if (profile == NULL) return false;
  memset(profile, 0, sizeof(*profile));
  if (rum == NULL || template_key == NULL || template_key[0] == '\0' ||
      ttl_seconds == 0U || viewport_bucket > 1U ||
      !laghu_rum_engine_read(rum, LAGHU_RUM_RECORD_INSTRUMENTATION,
                             template_key, now, &record, sizeof(record),
                             &value) ||
      value.length != sizeof(record) ||
      record.version != LAGHU_INSTRUMENTATION_VERSION ||
      strcmp(record.template_key, template_key) != 0)
    return true;
  profile->observations = record.observations[viewport_bucket];
  if (record.updated_at > now || now - record.updated_at > ttl_seconds) {
    profile->decision = LAGHU_TEMPLATE_PROFILE_STALE;
    return true;
  }
  if (profile->observations < LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES) return true;
  if (laghu_template_profile_unhealthy(&record, viewport_bucket)) {
    profile->decision = LAGHU_TEMPLATE_PROFILE_REGRESSION;
    return true;
  }
  profile->decision = LAGHU_TEMPLATE_PROFILE_LEARNED;
  profile->apply = true;
  return true;
}
