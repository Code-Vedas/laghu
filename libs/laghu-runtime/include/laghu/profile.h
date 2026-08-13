// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_PROFILE_H
#define LAGHU_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#include "laghu/instrumentation.h"
#include "laghu/rum.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES 10U

typedef enum {
  LAGHU_TEMPLATE_PROFILE_NONE = 0,
  LAGHU_TEMPLATE_PROFILE_LEARNED,
  LAGHU_TEMPLATE_PROFILE_STALE,
  LAGHU_TEMPLATE_PROFILE_REGRESSION
} laghu_template_profile_decision;

typedef struct {
  laghu_template_profile_decision decision;
  uint64_t observations;
  bool apply;
} laghu_template_profile;

/* Derives a bounded profile from the existing instrumentation record.  It
 * never writes RUM state: stale, undersampled, malformed, or unhealthy
 * evidence simply leaves the original response unchanged. */
bool laghu_template_profile_decide(laghu_rum_engine *rum, const char *template_key, uint64_t now, unsigned int ttl_seconds,
                                   unsigned int viewport_bucket, laghu_template_profile *profile);

#ifdef __cplusplus
}
#endif

#endif
