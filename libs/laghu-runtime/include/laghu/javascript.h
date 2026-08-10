// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_JAVASCRIPT_H
#define LAGHU_JAVASCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"
#include "laghu/csp.h"
#include "laghu/fonts.h"
#include "laghu/html.h"
#include "laghu/image.h"
#include "laghu/queue.h"
#include "laghu/rum.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct laghu_rum_instrumentation_record
    laghu_rum_instrumentation_record;

#define LAGHU_JAVASCRIPT_MAX_BYTES 2097152U
#define LAGHU_JAVASCRIPT_TARGET_SIZE 512U
#define LAGHU_JAVASCRIPT_MAX_SCRIPTS 64U
#define LAGHU_JAVASCRIPT_DERIVATION_VERSION 2U
#define LAGHU_JAVASCRIPT_DEFER_MAX_RULES 64U
typedef struct {
  unsigned char *data;
  size_t length;
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];
  bool rewritten;
  bool published;
} laghu_runtime_javascript_result;
typedef struct {
  char host[LAGHU_FONT_PROVIDER_HOST_SIZE];
  char path_prefix[LAGHU_FONT_PROVIDER_PREFIX_SIZE];
} laghu_javascript_observation_rule;

typedef struct {
  laghu_javascript_observation_rule rules[LAGHU_INSTRUMENTATION_MAX_PROVIDERS];
  unsigned int count;
  char digest[LAGHU_RUNTIME_KEY_SIZE];
} laghu_javascript_observation_set;

typedef struct {
  enum {
    LAGHU_JAVASCRIPT_DELAY_DEFER = 0,
    LAGHU_JAVASCRIPT_DELAY_INTERACTION
  } mode;
  char script_path[LAGHU_RUNTIME_PATH_SIZE];
  char template_path[LAGHU_RUNTIME_PATH_SIZE];
} laghu_javascript_defer_rule;

typedef struct {
  laghu_javascript_defer_rule rules[LAGHU_JAVASCRIPT_DEFER_MAX_RULES];
  unsigned int count;
  char digest[LAGHU_RUNTIME_KEY_SIZE];
} laghu_javascript_defer_set;
bool laghu_javascript_target_normalize(
    const char *target, char output[LAGHU_JAVASCRIPT_TARGET_SIZE]);
bool laghu_runtime_rewrite_javascript(
    laghu_runtime_queue *queue, const char *cache_path, laghu_buffer source,
    const char *normalized_path, const char *policy_key, const char *target,
    bool module, bool include_source_map,
    laghu_runtime_javascript_result *result);
bool laghu_runtime_rewrite_javascript_html(
    laghu_runtime_queue *queue, const char *cache_path, laghu_buffer html,
    const char *page_path, const char *policy_key, const char *target,
    const laghu_csp_policy *csp, uint64_t now, unsigned int ttl_seconds,
    laghu_rum_engine *rum, const char *template_key, const char *page_origin,
    unsigned int viewport_bucket, const laghu_javascript_defer_set *defer_set,
    bool allow_defer, bool allow_defer_suggestions, bool allow_combine,
    bool allow_inline, bool allow_outline, bool include_source_maps,
    unsigned int inline_limit, unsigned int outline_threshold,
    laghu_runtime_html_result *result);
/* Adds an explicit cooperative-yield helper before an application-owned
 * script marked data-laghu-yield="cooperative".  The marker nonce is both
 * validated and copied, so this is fail-open when CSP cannot authorize it. */
bool laghu_runtime_add_javascript_yield(laghu_buffer html,
                                        const laghu_csp_policy *csp, bool allow,
                                        laghu_runtime_html_result *result);
void laghu_runtime_javascript_result_release(
    laghu_runtime_javascript_result *result);
bool laghu_javascript_observations_load(const char *path,
                                        laghu_javascript_observation_set *set,
                                        char *error, size_t error_size);
bool laghu_javascript_defer_load(const char *path,
                                 laghu_javascript_defer_set *set, char *error,
                                 size_t error_size);
bool laghu_javascript_defer_approved(const laghu_javascript_defer_set *set,
                                     const char *script_path,
                                     const char *template_path);
bool laghu_javascript_interaction_approved(
    const laghu_javascript_defer_set *set, const char *script_url,
    const char *template_path);
bool laghu_javascript_defer_recommended(
    const laghu_rum_instrumentation_record *record, unsigned int bucket,
    unsigned int script_index);
bool laghu_javascript_defer_rollback_recommended(
    const laghu_rum_instrumentation_record *baseline,
    const laghu_rum_instrumentation_record *current, unsigned int bucket);

#ifdef __cplusplus
}
#endif

#endif
