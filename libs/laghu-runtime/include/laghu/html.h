// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_HTML_H
#define LAGHU_HTML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"
#include "laghu/csp.h"
#include "laghu/image.h"
#include "laghu/rum.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_HTML_PLANNER_VERSION 4U
#define LAGHU_HTML_MAX_TOKENS 4096U
#define LAGHU_HTML_MAX_HEADS 16U
#define LAGHU_HTML_MAX_PRELOADS 4U
#define LAGHU_HTML_MAX_PRECONNECT 4U
#define LAGHU_HTML_MAX_DNS_PREFETCH 8U
#define LAGHU_HTML_MAX_LINK_HEADERS 16U
#define LAGHU_HTML_HEADER_VALUE_SIZE 1152U
#define LAGHU_HTML_LANGUAGE_SIZE 128U
typedef uint32_t laghu_html_planner_mask;

#define LAGHU_HTML_PLAN_ADD_COMBINE_HEAD (UINT32_C(1) << 0)
#define LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD (UINT32_C(1) << 1)
#define LAGHU_HTML_PLAN_MOVE_CSS_ABOVE_SCRIPTS (UINT32_C(1) << 2)
#define LAGHU_HTML_PLAN_COLLAPSE_WHITESPACE (UINT32_C(1) << 3)
#define LAGHU_HTML_PLAN_REMOVE_COMMENTS (UINT32_C(1) << 4)
#define LAGHU_HTML_PLAN_REMOVE_QUOTES (UINT32_C(1) << 5)
#define LAGHU_HTML_PLAN_ELIDE_ATTRIBUTES (UINT32_C(1) << 6)
#define LAGHU_HTML_PLAN_CONVERT_META_TAGS (UINT32_C(1) << 7)
#define LAGHU_HTML_PLAN_RESOURCE_HINTS (UINT32_C(1) << 8)
#define LAGHU_HTML_PLAN_TRIM_URLS (UINT32_C(1) << 9)
#define LAGHU_HTML_PLAN_LEXICAL                                                                                                               \
  (LAGHU_HTML_PLAN_COLLAPSE_WHITESPACE | LAGHU_HTML_PLAN_REMOVE_COMMENTS | LAGHU_HTML_PLAN_REMOVE_QUOTES | LAGHU_HTML_PLAN_ELIDE_ATTRIBUTES | \
   LAGHU_HTML_PLAN_TRIM_URLS)

typedef struct {
  unsigned char *data;
  size_t length;
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];
  char content_language[LAGHU_HTML_LANGUAGE_SIZE];
  char *link_headers[LAGHU_HTML_MAX_LINK_HEADERS];
  unsigned int link_header_count;
  bool set_content_language;
  bool invalid;
  bool rewritten;
  bool dependencies_pending;
  bool javascript_defer_recommended;
  char javascript_defer_path[LAGHU_RUNTIME_PATH_SIZE];
  char javascript_defer_template[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int javascript_defer_bucket;
  uint64_t javascript_defer_observations;
} laghu_runtime_html_result;

typedef struct {
  unsigned char *data;
  size_t length;
  bool rewritten;
  bool added_head;
  bool combined_heads;
  bool moved_css;
  bool structural_changed;
  bool lexical_changed;
} laghu_runtime_head_result;
typedef struct {
  uint32_t version;
  char template_key[LAGHU_RUNTIME_KEY_SIZE];
  char stylesheet_url[LAGHU_RUNTIME_PATH_SIZE];
  char stylesheet_key[LAGHU_RUNTIME_KEY_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  uint64_t updated_at;
  uint32_t generation;
  uint16_t observation_count[4];
  unsigned char critical_rules[4][LAGHU_CRITICAL_CSS_MAX_RULES / 8U];
} laghu_critical_css_record;

typedef struct {
  char template_key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int viewport_bucket;
  unsigned int color_scheme_bucket;
  unsigned int rule_count;
  uint16_t rules[LAGHU_CRITICAL_CSS_MAX_RULES];
} laghu_critical_css_beacon;
bool laghu_runtime_rewrite_html(laghu_rum_engine *rum, const char *cache_path, laghu_buffer html, const char *page_path, const char *page_origin,
                                const char *policy_key, uint32_t capability_mask, uint64_t now, unsigned int ttl_seconds,
                                laghu_image_filter_mask filters, bool allow_inline, bool allow_css_inline, bool allow_css_outline,
                                bool allow_css_combine, laghu_html_planner_mask html_plan, const laghu_csp_policy *csp, bool beacon_enabled,
                                size_t inline_limit, unsigned int css_inline_limit, unsigned int css_outline_threshold, unsigned int viewport_width,
                                unsigned int dpr_hundredths, laghu_runtime_html_result *result);
void laghu_runtime_html_result_release(laghu_runtime_html_result *result);
bool laghu_runtime_finalize_html_headers(const char *cache_path, laghu_buffer html, const char *page_path, const char *page_origin,
                                         const char *policy_key, uint32_t capability_mask, uint64_t now, unsigned int ttl_seconds,
                                         laghu_html_planner_mask plan, const char *existing_content_language, const char *existing_link_headers,
                                         unsigned int css_inline_limit, unsigned int css_outline_threshold, bool already_warm,
                                         laghu_runtime_html_result *result);
bool laghu_runtime_plan_html_document(laghu_buffer html, laghu_html_planner_mask plan, laghu_runtime_head_result *result);
bool laghu_runtime_plan_html_document_at(laghu_buffer html, const char *page_path, const char *page_origin, laghu_html_planner_mask plan,
                                         laghu_runtime_head_result *result);
void laghu_runtime_head_result_release(laghu_runtime_head_result *result);
bool laghu_runtime_prioritize_critical_css(laghu_rum_engine *rum, const char *cache_path, laghu_buffer html, const char *page_path,
                                           const char *page_origin, const char *policy_key, uint32_t capability_mask, uint64_t now,
                                           unsigned int ttl_seconds, unsigned int inline_limit, unsigned int outline_threshold,
                                           unsigned int viewport_width, bool beacon_enabled, const laghu_csp_policy *csp,
                                           laghu_runtime_html_result *result);
bool laghu_runtime_parse_critical_css_beacon(laghu_buffer json, laghu_critical_css_beacon *record);
bool laghu_critical_css_apply_beacon(laghu_rum_engine *rum, const char *cache_path, const char *policy_key, uint64_t now, unsigned int ttl_seconds,
                                     const laghu_critical_css_beacon *beacon);
const char *laghu_runtime_critical_css_beacon_script(void);

#ifdef __cplusplus
}
#endif

#endif
