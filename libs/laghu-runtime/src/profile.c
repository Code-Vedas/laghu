// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/profile.h"

#include <string.h>

static bool laghu_template_profile_unhealthy(const laghu_rum_instrumentation_record *record, unsigned int bucket) {
  uint64_t observations;
  uint64_t allowed_failures;
  if (record == NULL || bucket > 1U) return true;
  observations = record->observations[bucket];
  /* A profile must stay below one failure per twenty observations.  Checking
   * each counter separately avoids overflow in a hostile aggregate. */
  allowed_failures = observations / 20U;
  return record->errors[bucket] > allowed_failures || record->rejections[bucket] > allowed_failures;
}

static unsigned int laghu_template_profile_p75(const uint64_t histogram[LAGHU_RUM_BUCKETS], uint64_t observations) {
  uint64_t cumulative = 0U;
  uint64_t threshold = observations - observations / 4U;
  unsigned int index;
  for (index = 0U; index < LAGHU_RUM_BUCKETS; ++index) {
    if (UINT64_MAX - cumulative < histogram[index]) return LAGHU_RUM_BUCKETS - 1U;
    cumulative += histogram[index];
    if (cumulative >= threshold) return index;
  }
  return LAGHU_RUM_BUCKETS - 1U;
}

bool laghu_template_profile_decide(laghu_rum_engine *rum, const char *template_key, uint64_t now, unsigned int ttl_seconds,
                                   unsigned int viewport_bucket, laghu_template_profile *profile) {
  laghu_rum_instrumentation_record record;
  laghu_rum_value value;
  if (profile == NULL) return false;
  memset(profile, 0, sizeof(*profile));
  if (rum == NULL || template_key == NULL || template_key[0] == '\0' || ttl_seconds == 0U || viewport_bucket > 1U ||
      !laghu_rum_engine_read(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, template_key, now, &record, sizeof(record), &value) ||
      value.length != sizeof(record) || record.version != LAGHU_INSTRUMENTATION_VERSION || strcmp(record.template_key, template_key) != 0)
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
  profile->lcp_over_budget =
      laghu_template_profile_p75(record.histograms[viewport_bucket][0], profile->observations) > LAGHU_TEMPLATE_PROFILE_LCP_BUDGET_BUCKET;
  profile->inp_over_budget =
      laghu_template_profile_p75(record.histograms[viewport_bucket][1], profile->observations) > LAGHU_TEMPLATE_PROFILE_INP_BUDGET_BUCKET;
  profile->cls_over_budget =
      laghu_template_profile_p75(record.histograms[viewport_bucket][2], profile->observations) > LAGHU_TEMPLATE_PROFILE_CLS_BUDGET_BUCKET;
  profile->decision = LAGHU_TEMPLATE_PROFILE_LEARNED;
  profile->apply = profile->lcp_over_budget;
  return true;
}
