// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_HTTP_H
#define LAGHU_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/assets.h"
#include "laghu/budget.h"
#include "laghu/cache.h"
#include "laghu/fonts.h"
#include "laghu/html.h"
#include "laghu/javascript.h"
#include "laghu/lcp.h"
#include "laghu/layout.h"
#include "laghu/operational.h"
#include "laghu/queue.h"
#include "laghu/rum.h"
#include "laghu/types.h"
#include "laghu/trace.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_HTTP_ABI_VERSION 7U
#define LAGHU_HTTP_MAX_REQUEST_HEADERS 64U
#define LAGHU_HTTP_MAX_RESPONSE_HEADERS 64U
#define LAGHU_HTTP_MAX_HEADER_OPERATIONS 32U
#define LAGHU_HTTP_MAX_HEADER_NAME 64U
#define LAGHU_HTTP_MAX_HEADER_VALUE 8192U

/*
 * Administrative routes are classified before native authorization or cache
 * access. The plan is value-only: it neither opens a registry nor touches a
 * cache, so each delivery surface can apply the same route and method rules
 * without putting server I/O in shared request policy.
 */
typedef enum {
  LAGHU_HTTP_ADMINISTRATIVE_ROUTE_NONE = 0,
  LAGHU_HTTP_ADMINISTRATIVE_ROUTE_PURGE,
  LAGHU_HTTP_ADMINISTRATIVE_ROUTE_STATS,
  LAGHU_HTTP_ADMINISTRATIVE_ROUTE_METRICS,
  LAGHU_HTTP_ADMINISTRATIVE_ROUTE_READINESS,
  LAGHU_HTTP_ADMINISTRATIVE_ROUTE_CONSOLE,
  LAGHU_HTTP_ADMINISTRATIVE_ROUTE_HISTORY,
  LAGHU_HTTP_ADMINISTRATIVE_ROUTE_EXPLAIN
} laghu_http_administrative_route;

typedef enum {
  LAGHU_HTTP_ADMINISTRATIVE_ACTION_NONE = 0,
  LAGHU_HTTP_ADMINISTRATIVE_ACTION_PURGE,
  LAGHU_HTTP_ADMINISTRATIVE_ACTION_STATS,
  LAGHU_HTTP_ADMINISTRATIVE_ACTION_METRICS,
  LAGHU_HTTP_ADMINISTRATIVE_ACTION_READINESS,
  LAGHU_HTTP_ADMINISTRATIVE_ACTION_CONSOLE,
  LAGHU_HTTP_ADMINISTRATIVE_ACTION_HISTORY,
  LAGHU_HTTP_ADMINISTRATIVE_ACTION_EXPLAIN
} laghu_http_administrative_action;

typedef enum {
  LAGHU_HTTP_ADMINISTRATIVE_CONTENT_NONE = 0,
  LAGHU_HTTP_ADMINISTRATIVE_CONTENT_JSON,
  LAGHU_HTTP_ADMINISTRATIVE_CONTENT_PROMETHEUS,
  LAGHU_HTTP_ADMINISTRATIVE_CONTENT_HTML
} laghu_http_administrative_content;

typedef enum {
  LAGHU_HTTP_ADMINISTRATIVE_CONSOLE_VIEW_OVERVIEW = 0,
  LAGHU_HTTP_ADMINISTRATIVE_CONSOLE_VIEW_HISTORY,
  LAGHU_HTTP_ADMINISTRATIVE_CONSOLE_VIEW_EXPLAIN
} laghu_http_administrative_console_view;

typedef struct {
  bool metrics_enabled;
  bool readiness_enabled;
  bool statistics_enabled;
  bool purge_method_enabled;
  bool purge_query_enabled;
  /* Standalone currently limits query purge to GET; native adapters do not. */
  bool purge_query_get_only;
} laghu_http_administrative_options;

typedef struct {
  unsigned int limit;
} laghu_http_administrative_history_query;

typedef struct {
  char path[LAGHU_RUNTIME_PATH_SIZE];
  char before[LAGHU_RUNTIME_PATH_SIZE];
  char after[LAGHU_RUNTIME_PATH_SIZE];
  char filter[LAGHU_RUNTIME_PATH_SIZE];
} laghu_http_administrative_console_query;

typedef struct {
  char target[LAGHU_RUNTIME_PATH_SIZE];
  char before[LAGHU_RUNTIME_PATH_SIZE];
  char after[LAGHU_RUNTIME_PATH_SIZE];
  char filter[LAGHU_RUNTIME_PATH_SIZE];
} laghu_http_administrative_explain_query;

typedef struct {
  char runtime[16];
  char cache[16];
  char workers[16];
  uint64_t hits;
  uint64_t misses;
  uint64_t bytes;
  char explain_path[LAGHU_RUNTIME_PATH_SIZE];
  char compare_before[LAGHU_RUNTIME_PATH_SIZE];
  char compare_after[LAGHU_RUNTIME_PATH_SIZE];
  char filter[LAGHU_RUNTIME_PATH_SIZE];
} laghu_http_administrative_console_model;

typedef struct {
  unsigned int index;
  char surface[24];
  char process[24];
  bool active;
  bool healthy;
  bool required;
  uint64_t requests;
  uint64_t original_bytes;
  uint64_t selected_bytes;
} laghu_http_administrative_history_record;

typedef struct {
  unsigned int count;
  unsigned int limit;
  laghu_http_administrative_history_record records[LAGHU_OPERATIONAL_HISTORY_SIZE];
} laghu_http_administrative_history_model;

typedef struct {
  bool has_target;
  char target[LAGHU_RUNTIME_PATH_SIZE];
  bool has_before;
  bool has_after;
  bool has_filter;
  char before[LAGHU_RUNTIME_PATH_SIZE];
  char after[LAGHU_RUNTIME_PATH_SIZE];
  char source_hash[LAGHU_RUNTIME_KEY_SIZE];
  char before_hash[LAGHU_RUNTIME_KEY_SIZE];
  char after_hash[LAGHU_RUNTIME_KEY_SIZE];
  char filter[LAGHU_RUNTIME_PATH_SIZE];
  bool runtime_ready;
  bool cache_ready;
  bool workers_ready;
  bool json;
  char status[24];
  uint64_t hits_ratio_ppm;
  char recommendation[256];
} laghu_http_administrative_explain_model;

typedef struct {
  laghu_http_administrative_console_model console;
  laghu_http_administrative_history_model history;
  laghu_http_administrative_explain_model explain;
  bool has_history;
  bool has_explain;
  bool output_json;
  laghu_http_administrative_action action;
} laghu_http_administrative_console_page_model;

typedef struct {
  laghu_http_administrative_route route;
  laghu_http_administrative_action action;
  char normalized_path[LAGHU_RUNTIME_PATH_SIZE];
  char purge_target[LAGHU_RUNTIME_PATH_SIZE];
  bool recognized;
  bool requires_authorization;
  bool head;
  bool purge_by_method;
  bool purge_by_query;
  bool output_json;
  laghu_http_administrative_console_view console_view;
  laghu_http_administrative_console_query console_query;
  laghu_http_administrative_history_query history_query;
  laghu_http_administrative_explain_query explain_query;
  /* A non-zero status is returned after native authorization succeeds. */
  unsigned int status;
} laghu_http_administrative_plan;

typedef struct {
  unsigned int status;
  laghu_http_administrative_content content;
  size_t length;
} laghu_http_administrative_response;

typedef enum {
  LAGHU_HTTP_BEACON_ROUTE_NONE = 0,
  LAGHU_HTTP_BEACON_ROUTE_IMAGE_SCRIPT,
  LAGHU_HTTP_BEACON_ROUTE_IMAGE_REPORT,
  LAGHU_HTTP_BEACON_ROUTE_CRITICAL_CSS_SCRIPT,
  LAGHU_HTTP_BEACON_ROUTE_CRITICAL_CSS_REPORT,
  LAGHU_HTTP_BEACON_ROUTE_INSTRUMENTATION_SCRIPT,
  LAGHU_HTTP_BEACON_ROUTE_INSTRUMENTATION_REPORT
} laghu_http_beacon_route;

typedef enum {
  LAGHU_HTTP_BEACON_ACTION_NONE = 0,
  LAGHU_HTTP_BEACON_ACTION_SERVE_SCRIPT,
  LAGHU_HTTP_BEACON_ACTION_ACCEPT_REPORT
} laghu_http_beacon_action;

typedef struct {
  bool image_enabled;
  bool critical_css_enabled;
  bool instrumentation_enabled;
} laghu_http_beacon_options;

typedef struct {
  laghu_http_beacon_route route;
  laghu_http_beacon_action action;
  bool recognized;
  unsigned int status;
} laghu_http_beacon_plan;

/*
 * laghu-http owns classification, policy and backend resolution, warm lookup,
 * derivation, queue publication, dependency identity, body selection, and the
 * complete ordered header plan. Adapters own configuration syntax, normalized
 * metadata extraction, bounded server-pool capture, atomic result application,
 * and transport behavior. NGINX therefore retains chain and deferred-header
 * mechanics; Apache retains brigade metadata, FLUSH, and EOS mechanics. A
 * standalone proxy retains sockets, TLS, framing, cancellation, timeouts, and
 * backpressure.
 */

typedef struct {
  laghu_buffer name;
  laghu_buffer value;
} laghu_http_header;

typedef struct {
  uint32_t version;
  size_t struct_size;
  laghu_buffer method;
  laghu_buffer scheme;
  laghu_buffer authority;
  laghu_buffer normalized_path;
  const laghu_http_header *headers;
  size_t header_count;
} laghu_http_request;

typedef struct {
  uint32_t version;
  size_t struct_size;
  unsigned int status;
  const laghu_http_header *headers;
  size_t header_count;
  size_t declared_length;
  bool has_declared_length;
  bool complete;
  bool partial;
  laghu_buffer source_validator;
} laghu_http_response;

typedef struct {
  uint32_t version;
  size_t struct_size;
  laghu_config config;
  const char *cache_path;
  laghu_rum_engine *rum;
  laghu_runtime_queue *queue;
  /* Worker capability state refreshed by the adapter lifecycle, never by a request. */
  uint32_t queue_capabilities;
  laghu_runtime_queue *font_fetch_queue;
  laghu_runtime_queue *javascript_queue;
  /* Already-attached optional queue; HTTP finalization only try-publishes. */
  laghu_runtime_queue *chrome_analysis_queue;
  laghu_runtime_queue *otel_trace_queue;
  unsigned int otel_sampling_rate;
  unsigned int chrome_analysis_timeout_ms;
  const char *javascript_target;
  const laghu_font_provider_set *font_providers;
  const laghu_javascript_observation_set *javascript_observations;
  const laghu_javascript_defer_set *javascript_defer;
  const laghu_layout_reservation_set *layout_reservations;
  const laghu_asset_config *asset_offload;
  uint64_t now;
} laghu_http_environment;

typedef enum { LAGHU_HTTP_HEADER_SET = 0, LAGHU_HTTP_HEADER_APPEND, LAGHU_HTTP_HEADER_REMOVE } laghu_http_header_operation_kind;

typedef struct {
  laghu_http_header_operation_kind kind;
  char name[LAGHU_HTTP_MAX_HEADER_NAME + 1U];
  char *value;
  /* Send this generated Link field in an informational 103 when transport can.
   */
  bool early_hint;
} laghu_http_header_operation;

typedef enum {
  LAGHU_HTTP_ACTION_BYPASS = 0,
  LAGHU_HTTP_ACTION_CAPTURE_HTML,
  LAGHU_HTTP_ACTION_CAPTURE_CSS,
  LAGHU_HTTP_ACTION_CAPTURE_JAVASCRIPT,
  LAGHU_HTTP_ACTION_CAPTURE_IMAGE,
  LAGHU_HTTP_ACTION_CAPTURE_RESOURCE,
  LAGHU_HTTP_ACTION_SERVE_CACHED
} laghu_http_action;

typedef struct {
  uint32_t version;
  size_t struct_size;
  laghu_http_action action;
  laghu_decision decision;
  laghu_buffer original;
  laghu_buffer selected;
  unsigned char *owned_body;
  /* Immutable cache artifact adapters may send without copying payload bytes. */
  laghu_runtime_cache_entry cached_entry;
  bool cached_file;
  /* Identity representation preserved when selected is content-encoded. */
  laghu_buffer cache_selected;
  unsigned char *cache_owned_body;
  size_t capture_limit;
  laghu_http_header_operation header_operations[LAGHU_HTTP_MAX_HEADER_OPERATIONS];
  size_t header_operation_count;
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];
  char cache_key[LAGHU_RUNTIME_KEY_SIZE];
  bool dependencies_pending;
  bool job_published;
  bool not_modified;
  bool javascript_defer_recommended;
  bool javascript_defer_rollback_recommended;
  char javascript_defer_path[LAGHU_RUNTIME_PATH_SIZE];
  char javascript_defer_template[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int javascript_defer_bucket;
  uint64_t javascript_defer_observations;
  laghu_lcp_decision lcp_decision;
  unsigned int lcp_profile_observations[4];
  bool lcp_profile_ready[4];
  bool lcp_applied;
} laghu_http_transaction_result;

typedef struct {
  uint32_t version;
  size_t struct_size;
  const laghu_http_request *request;
  const laghu_http_response *response;
  laghu_http_environment environment;
  laghu_policy policy;
  laghu_image_filter_mask image_filters;
  laghu_html_planner_mask html_plan;
  laghu_http_action action;
  laghu_decision decision;
  char path[LAGHU_RUNTIME_PATH_SIZE];
  char origin[LAGHU_RUNTIME_PATH_SIZE];
  char content_type[LAGHU_RUNTIME_TYPE_SIZE];
  char cache_control[LAGHU_HTTP_MAX_HEADER_VALUE + 1U];
  char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  char cache_key[LAGHU_RUNTIME_KEY_SIZE];
  uint32_t capability_mask;
  unsigned int target_count;
  unsigned int target_width[LAGHU_RUNTIME_MAX_TARGETS];
  unsigned int target_height[LAGHU_RUNTIME_MAX_TARGETS];
  uint64_t resize_filter[LAGHU_RUNTIME_MAX_TARGETS];
  unsigned int viewport_width;
  unsigned int dpr_hundredths;
  bool sec_ch_viewport_width;
  bool sec_ch_dpr;
  bool legacy_viewport_width;
  bool legacy_dpr;
  bool client_hint_variant;
  bool accept_webp;
  bool accept_avif;
  bool accept_jxl;
  bool save_data;
  bool index_key_content_classified;
  bool prepared;
  bool asset_allowed;
  bool cache_publishable;
  bool query_preview;
  laghu_trace_context trace;
  laghu_transform_budget budget;
} laghu_http_transaction;

void laghu_http_administrative_options_init(laghu_http_administrative_options *options);
bool laghu_http_administrative_plan_build(laghu_http_administrative_plan *plan, laghu_buffer method, laghu_buffer target,
                                          const laghu_http_administrative_options *options);
bool laghu_http_administrative_render_operational(const laghu_http_administrative_plan *plan, const laghu_operational_snapshot *snapshot,
                                                  uint64_t now, bool runtime_ready, bool cache_ready, bool strict_workers, char *output,
                                                  size_t capacity, laghu_http_administrative_response *response);
bool laghu_http_administrative_render_console_model(const laghu_http_administrative_console_model *model, char *output, size_t capacity,
                                                    laghu_http_administrative_response *response);
bool laghu_http_administrative_render_console(const laghu_cache_stats *stats, const laghu_operational_readiness *ready, bool as_json, char *output,
                                              size_t capacity, const laghu_http_administrative_console_query *query,
                                              laghu_http_administrative_response *response);
bool laghu_http_administrative_build_console_page_model(const laghu_http_administrative_plan *plan, const laghu_cache_stats *stats,
                                                        const laghu_operational_readiness *ready, const laghu_operational_snapshot *snapshot,
                                                        laghu_http_administrative_console_page_model *model);
bool laghu_http_administrative_render_console_page(const laghu_http_administrative_plan *plan,
                                                   const laghu_http_administrative_console_page_model *model, char *output, size_t capacity,
                                                   laghu_http_administrative_response *response);
bool laghu_http_administrative_build_console_model(const laghu_cache_stats *stats, const laghu_operational_readiness *ready,
                                                   const laghu_http_administrative_console_query *query,
                                                   laghu_http_administrative_console_model *model);
bool laghu_http_administrative_render_purge(laghu_cache_purge_result purge_result, uint64_t matched_artifacts, char *output, size_t capacity,
                                            laghu_http_administrative_response *response);
bool laghu_http_administrative_render_stats(const laghu_cache_limits *limits, const laghu_cache_stats *stats, char *output, size_t capacity,
                                            laghu_http_administrative_response *response);
bool laghu_http_administrative_render_history(const laghu_http_administrative_history_model *model, bool as_json, char *output, size_t capacity,
                                              laghu_http_administrative_response *response);
bool laghu_http_administrative_render_explain(const laghu_http_administrative_explain_model *model, char *output, size_t capacity,
                                              laghu_http_administrative_response *response);
bool laghu_http_administrative_build_history_model(const laghu_operational_snapshot *snapshot, const laghu_http_administrative_history_query *query,
                                                   laghu_http_administrative_history_model *model);
bool laghu_http_administrative_build_explain_model(const laghu_http_administrative_explain_query *query, const laghu_operational_readiness *ready,
                                                   const laghu_cache_stats *stats, bool as_json, laghu_http_administrative_explain_model *model);
void laghu_http_beacon_options_init(laghu_http_beacon_options *options);
bool laghu_http_beacon_plan_build(laghu_http_beacon_plan *plan, laghu_buffer method, laghu_buffer target, const laghu_http_beacon_options *options);

void laghu_http_transaction_init(laghu_http_transaction *transaction);
bool laghu_http_transaction_prepare(laghu_http_transaction *transaction, const laghu_http_request *request, const laghu_http_response *response,
                                    const laghu_http_environment *environment, laghu_http_transaction_result *result);
bool laghu_http_transaction_finalize(laghu_http_transaction *transaction, laghu_buffer captured_body, laghu_http_transaction_result *result);
/* Implements HTTP weak comparison for If-None-Match against a generated ETag.
 * Call after finalize, because transformed representations own their validator.
 */
bool laghu_http_request_matches_result_etag(const laghu_http_request *request, const laghu_http_transaction_result *result);
void laghu_http_transaction_result_release(laghu_http_transaction_result *result);

#ifdef __cplusplus
}
#endif

#endif
