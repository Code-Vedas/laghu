// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_NGINX_INTERNAL_H
#define LAGHU_NGINX_INTERNAL_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "laghu/assets.h"
#include "laghu/cache.h"
#include "laghu/fonts.h"
#include "laghu/http.h"
#include "laghu/javascript.h"
#include "laghu/operational.h"
#include "laghu/precompressed.h"
#include "laghu/queue.h"
#include "laghu/service_config.h"
#include "laghu/source.h"

#define LAGHU_NGINX_DEFAULT_QUEUE "/run/laghu/jobs.queue"
#define LAGHU_NGINX_DEFAULT_CACHE "/var/cache/laghu/images"
#define LAGHU_NGINX_DEFAULT_FONT_QUEUE "/run/laghu/fonts.queue"
#define LAGHU_NGINX_DEFAULT_JAVASCRIPT_QUEUE "/run/laghu/javascript.queue"

#define LAGHU_NGINX_QUEUE_CONFIG_LIMIT 128U
#define LAGHU_NGINX_HTML_REFRESH_DEDUP 16U

typedef struct {
  laghu_config core;
  laghu_service_config service;
  laghu_runtime_queue runtime_queue;
  laghu_runtime_queue font_fetch_runtime_queue;
  laghu_runtime_queue javascript_runtime_queue;
  laghu_runtime_queue html_refresh_runtime_queue;
  laghu_runtime_queue chrome_analysis_runtime_queue;
  bool runtime_queue_attached;
  bool font_fetch_runtime_queue_attached;
  bool javascript_runtime_queue_attached;
  bool html_refresh_runtime_queue_attached;
  bool chrome_analysis_runtime_queue_attached;
  uint32_t runtime_queue_capabilities;
  bool runtime_queue_snapshot_ready;
  bool queue_registered;
  ngx_atomic_t html_refresh_dedup_lock;
  ngx_atomic_t html_refresh_until[LAGHU_NGINX_HTML_REFRESH_DEDUP];
  char html_refresh_keys[LAGHU_NGINX_HTML_REFRESH_DEDUP][LAGHU_RUNTIME_KEY_SIZE];
} ngx_http_laghu_loc_conf_t;

typedef struct {
  laghu_service_config service;
  ngx_str_t operational_cache;
  ngx_array_t *queue_configs;
} ngx_http_laghu_main_conf_t;

typedef struct {
  laghu_http_request request;
  laghu_http_response response;
  laghu_http_environment environment;
  laghu_http_header request_headers[LAGHU_HTTP_MAX_REQUEST_HEADERS];
  laghu_http_header response_headers[LAGHU_HTTP_MAX_RESPONSE_HEADERS];
  laghu_http_transaction transaction;
  laghu_policy policy;
  laghu_runtime_cache_entry cache_entry;
  char policy_key[LAGHU_SHA256_HEX_SIZE];
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  unsigned char *capture;
  size_t capture_length;
  size_t capture_capacity;
  ngx_chain_t *cached_output;
  unsigned char *cached_body;
  ngx_file_t *cached_file;
  bool accept_webp;
  bool capture_enabled;
  bool cache_hit;
  bool cache_sent;
  unsigned int target_count;
  unsigned int target_width[LAGHU_RUNTIME_MAX_TARGETS];
  unsigned int target_height[LAGHU_RUNTIME_MAX_TARGETS];
  uint64_t resize_filter[LAGHU_RUNTIME_MAX_TARGETS];
  bool html_capture;
  bool css_capture;
  bool header_deferred;
  bool log_written;
  uint64_t log_started_ms;
  char trace_id[33U];
  char span_id[17U];
} ngx_http_laghu_request_ctx_t;

extern ngx_http_output_header_filter_pt ngx_http_laghu_next_header_filter;
extern ngx_http_output_body_filter_pt ngx_http_laghu_next_body_filter;
extern time_t ngx_http_laghu_last_queue_warning;
ngx_int_t ngx_http_laghu_transaction_header_filter(ngx_http_request_t *request);
ngx_int_t ngx_http_laghu_transaction_body_filter(ngx_http_request_t *request, ngx_chain_t *chain);

void ngx_http_laghu_log_defer_recommendation(ngx_http_request_t *request, const laghu_http_transaction_result *result);
void ngx_http_laghu_log_transaction(ngx_http_request_t *request, ngx_http_laghu_request_ctx_t *context, const laghu_http_transaction_result *result,
                                    const char *failure);
extern ngx_module_t ngx_http_laghu_module;
extern laghu_rum_engine *ngx_http_laghu_rum;
extern laghu_operational_registry ngx_http_laghu_operational;
ngx_int_t ngx_http_laghu_init_process(ngx_cycle_t *cycle);
void ngx_http_laghu_exit_process(ngx_cycle_t *cycle);
bool ngx_http_laghu_queue_refresh(ngx_http_laghu_loc_conf_t *conf);
bool ngx_http_laghu_font_queue_refresh(ngx_http_laghu_loc_conf_t *conf);
bool ngx_http_laghu_javascript_queue_refresh(ngx_http_laghu_loc_conf_t *conf);
bool ngx_http_laghu_administration_candidate(ngx_http_request_t *request, ngx_http_laghu_loc_conf_t *conf);
laghu_runtime_queue *ngx_http_laghu_image_queue(ngx_http_laghu_loc_conf_t *conf);
uint32_t ngx_http_laghu_image_queue_capabilities(const ngx_http_laghu_loc_conf_t *conf);
laghu_runtime_queue *ngx_http_laghu_font_queue(ngx_http_laghu_loc_conf_t *conf);
laghu_runtime_queue *ngx_http_laghu_javascript_queue(ngx_http_laghu_loc_conf_t *conf);
laghu_runtime_queue *ngx_http_laghu_html_refresh_queue(ngx_http_laghu_loc_conf_t *conf);
laghu_runtime_queue *ngx_http_laghu_chrome_analysis_queue(ngx_http_laghu_loc_conf_t *conf);
void ngx_http_laghu_beacon_body(ngx_http_request_t *request);
void *ngx_http_laghu_create_loc_conf(ngx_conf_t *configuration);
void *ngx_http_laghu_create_main_conf(ngx_conf_t *configuration);
char *ngx_http_laghu_merge_loc_conf(ngx_conf_t *configuration, void *parent, void *child);
char *ngx_http_laghu_command(ngx_conf_t *configuration, ngx_command_t *command, void *conf);
ngx_int_t ngx_http_laghu_variant_handler(ngx_http_request_t *request);
ngx_int_t ngx_http_laghu_admin_endpoint(ngx_http_request_t *request, ngx_http_laghu_loc_conf_t *conf);
ngx_int_t ngx_http_laghu_beacon_endpoint(ngx_http_request_t *request, ngx_http_laghu_loc_conf_t *conf);
ngx_int_t ngx_http_laghu_asset_endpoint(ngx_http_request_t *request, ngx_http_laghu_loc_conf_t *conf);
ngx_int_t ngx_http_laghu_apply_result(ngx_http_request_t *request, const laghu_http_transaction_result *result);
ngx_int_t ngx_http_laghu_send_early_hints(ngx_http_request_t *request, const laghu_http_transaction_result *result);
void ngx_http_laghu_remove_header(ngx_http_request_t *request, const char *name);
bool ngx_http_laghu_normalize(ngx_http_request_t *request, ngx_http_laghu_loc_conf_t *conf, ngx_http_laghu_request_ctx_t *context);
bool ngx_http_laghu_peer_matches(ngx_http_request_t *request, const laghu_service_cidr *cidrs, size_t count);

#endif
