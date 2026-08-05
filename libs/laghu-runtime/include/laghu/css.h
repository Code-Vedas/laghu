// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_CSS_H
#define LAGHU_CSS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"
#include "laghu/csp.h"
#include "laghu/fonts.h"
#include "laghu/html.h"
#include "laghu/image.h"
#include "laghu/queue.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_CSS_DERIVATION_VERSION 2U
#define LAGHU_STYLESHEET_CATALOG_VERSION 2U
#define LAGHU_CSS_IMPORT_MAX_DEPTH 8U
#define LAGHU_HTML_PLANNER_VERSION 4U
#define LAGHU_CRITICAL_CSS_VERSION 2U
#define LAGHU_CRITICAL_CSS_MAX_RULES 512U
#define LAGHU_CRITICAL_CSS_QUORUM 3U
typedef struct {
  unsigned char *data;
  size_t length;
  size_t original_external_bytes;
  size_t combined_external_bytes;
  bool rewritten;
  bool dependencies_pending;
} laghu_runtime_css_combine_result;

typedef struct {
  unsigned char *data;
  size_t length;
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];
  bool rewritten;
  bool dependencies_pending;
  bool published;
  bool used_fallback;
} laghu_runtime_css_result;

typedef struct {
  unsigned char *data;
  size_t length;
  size_t original_external_bytes;
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];
  bool flattened;
  bool dependencies_pending;
  bool invalid;
} laghu_runtime_css_import_result;

typedef struct {
  uint32_t version;
  char normalized_url[LAGHU_RUNTIME_PATH_SIZE];
  char source_hash[LAGHU_RUNTIME_KEY_SIZE];
  char source_key[LAGHU_RUNTIME_KEY_SIZE];
  char derived_key[LAGHU_RUNTIME_KEY_SIZE];
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  uint32_t capability_mask;
  uint32_t parser_version;
  unsigned int inline_limit;
  unsigned int outline_threshold;
  size_t source_length;
  size_t derived_length;
  uint64_t updated_at;
  bool ready;
  bool terminally_excluded;
} laghu_stylesheet_record;
bool laghu_runtime_rewrite_css_markup(
    const char *cache_path, laghu_buffer html, const char *page_path,
    const char *page_origin, const char *policy_key, uint32_t capability_mask,
    uint64_t now, unsigned int ttl_seconds, bool allow_inline,
    bool allow_outline, bool allow_combine, laghu_html_planner_mask html_plan,
    bool csp_allows_inline_styles, bool csp_allows_self_styles,
    unsigned int inline_limit, unsigned int outline_threshold,
    laghu_runtime_html_result *result);
bool laghu_runtime_rewrite_css_markup_csp(
    const char *cache_path, laghu_buffer html, const char *page_path,
    const char *page_origin, const char *policy_key, uint32_t capability_mask,
    uint64_t now, unsigned int ttl_seconds, bool allow_inline,
    bool allow_outline, bool allow_combine, laghu_html_planner_mask html_plan,
    const laghu_csp_policy *csp, unsigned int inline_limit,
    unsigned int outline_threshold, laghu_runtime_html_result *result);
bool laghu_runtime_rewrite_font_css(
    laghu_runtime_queue *fetch_queue, const char *cache_path,
    const laghu_font_provider_set *providers, laghu_buffer html, uint64_t now,
    bool allow_inline, const laghu_csp_policy *csp, unsigned int inline_limit,
    laghu_runtime_html_result *result);
bool laghu_runtime_combine_css_markup(
    const char *cache_path, laghu_buffer html, const char *page_path,
    const char *page_origin, const char *policy_key, uint32_t capability_mask,
    uint64_t now, unsigned int ttl_seconds, unsigned int inline_limit,
    unsigned int outline_threshold, laghu_runtime_css_combine_result *result);
void laghu_runtime_css_combine_result_release(
    laghu_runtime_css_combine_result *result);
bool laghu_runtime_rewrite_css(laghu_runtime_queue *queue,
                               const char *cache_path, laghu_buffer css,
                               const char *stylesheet_path,
                               const char *page_origin, const char *policy_key,
                               uint32_t capability_mask, uint64_t now,
                               unsigned int ttl_seconds, bool minify,
                               bool allow_sprites, unsigned int inline_limit,
                               unsigned int outline_threshold,
                               laghu_runtime_css_result *result);
void laghu_runtime_css_result_release(laghu_runtime_css_result *result);
bool laghu_runtime_flatten_css_imports(
    const char *cache_path, laghu_buffer css, const char *stylesheet_path,
    const char *page_origin, const char *policy_key, uint32_t capability_mask,
    uint64_t now, unsigned int ttl_seconds, unsigned int inline_limit,
    unsigned int outline_threshold, laghu_runtime_css_import_result *result);
void laghu_runtime_css_import_result_release(
    laghu_runtime_css_import_result *result);
bool laghu_stylesheet_publish(const char *cache_path,
                              const laghu_stylesheet_record *record);
bool laghu_stylesheet_lookup(const char *cache_path, const char *normalized_url,
                             const char *policy_key, uint32_t capability_mask,
                             unsigned int inline_limit,
                             unsigned int outline_threshold, uint64_t now,
                             unsigned int ttl_seconds,
                             laghu_stylesheet_record *record);

#ifdef __cplusplus
}
#endif

#endif
