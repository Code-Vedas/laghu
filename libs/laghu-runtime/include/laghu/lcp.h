// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_LCP_H
#define LAGHU_LCP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"
#include "laghu/csp.h"
#include "laghu/image.h"
#include "laghu/instrumentation.h"
#include "laghu/rum.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_LCP_MAX_CANDIDATES 32U
#define LAGHU_LCP_MAX_RESOURCES 4U
#define LAGHU_LCP_QUORUM 3U
#define LAGHU_LCP_DOMINANCE_PERCENT 80U
typedef enum {
  LAGHU_LCP_DECISION_NONE = 0,
  LAGHU_LCP_DECISION_LEARNED,
  LAGHU_LCP_DECISION_HEURISTIC,
  LAGHU_LCP_DECISION_UNRESOLVED,
  LAGHU_LCP_DECISION_STALE,
  LAGHU_LCP_DECISION_CONFLICT
} laghu_lcp_decision;

typedef struct {
  unsigned char *data;
  size_t length;
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];
  char *link_header;
  laghu_lcp_decision decision;
  unsigned int observations;
  unsigned int profile_observations[4];
  bool profile_ready[4];
  bool rewritten;
  bool applied;
} laghu_lcp_result;
bool laghu_lcp_inventory_record(laghu_buffer html, const char *page_path,
                                const char *page_origin,
                                laghu_rum_instrumentation_record *record,
                                char digest[LAGHU_RUNTIME_KEY_SIZE]);
bool laghu_runtime_prioritize_lcp(
    laghu_rum_engine *rum, laghu_buffer evidence_html, laghu_buffer html,
    const char *page_path, const char *page_origin, const char *template_key,
    uint64_t now, unsigned int ttl_seconds, unsigned int viewport_width,
    bool resource_hints, bool lazyload, const laghu_csp_policy *csp,
    laghu_lcp_result *result);
/* Applies no heuristic fallback: callers that have opted into template
 * profiles use this after profile evidence has passed its safety gate. */
bool laghu_runtime_prioritize_learned_lcp(
    laghu_rum_engine *rum, laghu_buffer evidence_html, laghu_buffer html,
    const char *page_path, const char *page_origin, const char *template_key,
    uint64_t now, unsigned int ttl_seconds, unsigned int viewport_width,
    bool resource_hints, bool lazyload, const laghu_csp_policy *csp,
    laghu_lcp_result *result);
void laghu_lcp_result_release(laghu_lcp_result *result);

#ifdef __cplusplus
}
#endif

#endif
