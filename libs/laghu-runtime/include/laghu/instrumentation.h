// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_INSTRUMENTATION_H
#define LAGHU_INSTRUMENTATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"
#include "laghu/csp.h"
#include "laghu/html.h"
#include "laghu/image.h"
#include "laghu/javascript.h"
#include "laghu/rum.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_INSTRUMENTATION_VERSION 2U
#define LAGHU_INSTRUMENTATION_MAX_PROVIDERS 32U
#define LAGHU_INSTRUMENTATION_MAX_SCRIPTS 64U
#define LAGHU_SHA256_DIGEST_SIZE 32U
typedef struct laghu_rum_instrumentation_record {
  uint32_t version;
  char template_key[LAGHU_RUNTIME_KEY_SIZE];
  char provider_digest[LAGHU_RUNTIME_KEY_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  uint64_t updated_at;
  unsigned int script_count;
  char script_keys[LAGHU_INSTRUMENTATION_MAX_SCRIPTS][LAGHU_RUNTIME_KEY_SIZE];
  uint32_t media_count;
  unsigned char media_kind[LAGHU_LCP_MAX_CANDIDATES];
  unsigned char media_keys[LAGHU_LCP_MAX_CANDIDATES][LAGHU_SHA256_DIGEST_SIZE];
  unsigned char media_resource_count[LAGHU_LCP_MAX_CANDIDATES];
  unsigned char media_resource_keys[LAGHU_LCP_MAX_CANDIDATES][LAGHU_LCP_MAX_RESOURCES][LAGHU_SHA256_DIGEST_SIZE];
  uint64_t observations[2];
  uint64_t metric_sums[2][5];
  unsigned int metric_maxima[2][5];
  uint64_t histograms[2][LAGHU_RUM_HISTOGRAMS][LAGHU_RUM_BUCKETS];
  uint64_t errors[2];
  uint64_t rejections[2];
  uint64_t script_observations[2][LAGHU_INSTRUMENTATION_MAX_SCRIPTS];
  uint64_t script_before_dcl[2][LAGHU_INSTRUMENTATION_MAX_SCRIPTS];
  uint64_t script_long_tasks[2][LAGHU_INSTRUMENTATION_MAX_SCRIPTS];
  uint16_t lcp_observations[4];
  uint16_t lcp_unresolved[4];
  uint16_t lcp_candidates[4][LAGHU_LCP_MAX_CANDIDATES];
  uint16_t lcp_resources[4][LAGHU_LCP_MAX_CANDIDATES][LAGHU_LCP_MAX_RESOURCES];
} laghu_rum_instrumentation_record;
typedef struct {
  char key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int before_dcl;
  unsigned int long_tasks;
} laghu_instrumentation_candidate;

typedef struct {
  char template_key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int bucket;
  unsigned int lcp_ms;
  unsigned int inp_ms;
  unsigned int cls_milli;
  unsigned int dcl_ms;
  unsigned int load_ms;
  unsigned int errors;
  unsigned int rejections;
  laghu_instrumentation_candidate candidates[LAGHU_INSTRUMENTATION_MAX_SCRIPTS];
  unsigned int candidate_count;
  unsigned int version;
  unsigned int color_scheme_bucket;
  unsigned int lcp_kind;
  unsigned int lcp_ordinal;
  char lcp_resource_key[LAGHU_RUNTIME_KEY_SIZE];
} laghu_instrumentation_beacon;
bool laghu_runtime_add_instrumentation(laghu_rum_engine *rum, const char *cache_path, const laghu_javascript_observation_set *providers,
                                       laghu_buffer html, const char *page_path, const char *page_origin, const char *policy_key, uint64_t now,
                                       unsigned int ttl_seconds, unsigned int sample_rate, const laghu_csp_policy *csp,
                                       laghu_runtime_html_result *result);
/* Inserts the supplied, already-created template id into the served document.
 * This keeps post-rewrite markup associated with the pre-rewrite RUM record. */
bool laghu_runtime_insert_instrumentation_template(laghu_buffer html, const char *template_key, unsigned int sample_rate,
                                                   const laghu_csp_policy *csp, laghu_runtime_html_result *result);
bool laghu_runtime_instrumentation_template_key(laghu_rum_engine *rum, const char *cache_path, const laghu_javascript_observation_set *providers,
                                                laghu_buffer html, const char *page_path, const char *page_origin, const char *policy_key,
                                                uint64_t now, unsigned int ttl_seconds, unsigned int sample_rate,
                                                char output[LAGHU_RUNTIME_KEY_SIZE]);
const char *laghu_runtime_instrumentation_script(void);
bool laghu_runtime_parse_instrumentation_beacon(laghu_buffer json, laghu_instrumentation_beacon *record);
bool laghu_instrumentation_apply_beacon(laghu_rum_engine *rum, const char *cache_path, uint64_t now, unsigned int ttl_seconds,
                                        const laghu_instrumentation_beacon *beacon);
/* Applies one Chrome-analysis result only to the existing LCP candidate
 * counters.  It never creates a template, adds synthetic timing samples, or
 * changes a response when the report is malformed/stale. */
bool laghu_runtime_apply_chrome_analysis(laghu_rum_engine *rum, laghu_buffer json, uint64_t now, unsigned int ttl_seconds);
/* Lifecycle-only bounded importer.  At most eight regular report files are
 * considered per call; successfully applied reports are unlinked. */
unsigned int laghu_runtime_import_chrome_analysis(laghu_rum_engine *rum, const char *directory, uint64_t now, unsigned int ttl_seconds);

#ifdef __cplusplus
}
#endif

#endif
