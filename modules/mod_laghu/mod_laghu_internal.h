// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_APACHE_INTERNAL_H
#define LAGHU_APACHE_INTERNAL_H

#include <ctype.h>
#include <errno.h>
#include <httpd.h>
#include <stdint.h>
#include <stdlib.h>

/* Apache requires httpd.h to define its public record types first. */
#include <apr_atomic.h>
#include <apr_buckets.h>
#include <apr_network_io.h>
#include <apr_strings.h>
#include <apr_tables.h>
#include <apr_thread_proc.h>
#include <http_config.h>
#include <http_core.h>
#include <http_log.h>
#include <http_protocol.h>
#include <http_request.h>
#include <util_filter.h>

#include "laghu/assets.h"
#include "laghu/cache.h"
#include "laghu/catalog.h"
#include "laghu/config.h"
#include "laghu/core.h"
#include "laghu/css.h"
#include "laghu/fonts.h"
#include "laghu/http.h"
#include "laghu/image.h"
#include "laghu/instrumentation.h"
#include "laghu/javascript.h"
#include "laghu/operational.h"
#include "laghu/queue.h"
#include "laghu/rum.h"
#include "laghu/service_config.h"
#include "laghu/source.h"

#define LAGHU_APACHE_FILTER "LAGHU"
#define LAGHU_APACHE_INITIAL_CAPTURE (64U * 1024U)
#define LAGHU_APACHE_QUEUE_CONFIG_LIMIT 128U

#define LAGHU_DEFAULT_QUEUE "/run/laghu/jobs.queue"
#define LAGHU_DEFAULT_CACHE "/var/cache/laghu/images"
#define LAGHU_DEFAULT_FONT_QUEUE "/run/laghu/fonts.queue"
#define LAGHU_DEFAULT_JAVASCRIPT_QUEUE "/run/laghu/javascript.queue"

typedef struct laghu_apache_queue_binding laghu_apache_queue_binding;

typedef struct {
  laghu_config core;
  laghu_service_config service;
  laghu_apache_queue_binding *queue_binding;
} laghu_apache_config;

typedef struct {
  laghu_apache_config *config;
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
  unsigned char *selected_body;
  size_t selected_length;
  laghu_http_action action;
  laghu_image_filter_mask filters;
  bool accept_webp;
  bool decided;
  bool capture_enabled;
  bool cache_hit;
  bool cache_sent;
  unsigned int target_count;
  unsigned int target_width[LAGHU_RUNTIME_MAX_TARGETS];
  unsigned int target_height[LAGHU_RUNTIME_MAX_TARGETS];
  uint64_t resize_filter[LAGHU_RUNTIME_MAX_TARGETS];
  bool html_capture;
  bool css_capture;
} laghu_apache_context;

extern module AP_MODULE_DECLARE_DATA laghu_module;
void laghu_apache_log_defer_recommendation(
    request_rec *request, const laghu_http_transaction_result *result);
extern laghu_rum_engine *laghu_apache_rum;
extern laghu_operational_registry laghu_apache_operational;
extern const char *laghu_apache_operational_cache;
extern bool laghu_apache_operational_enabled;
void laghu_apache_child_init(apr_pool_t *pool, server_rec *server);
void laghu_apache_queue_registry_reset(void);
bool laghu_apache_queue_registry_add(const laghu_apache_config *parent,
                                     const laghu_apache_config *child,
                                     const laghu_service_config *service);
laghu_apache_queue_binding *laghu_apache_queue_binding_find_service(
    const laghu_service_config *service);
laghu_runtime_queue *laghu_apache_image_queue(laghu_apache_config *config);
laghu_runtime_queue *laghu_apache_font_queue(laghu_apache_config *config);
laghu_runtime_queue *laghu_apache_javascript_queue(laghu_apache_config *config);
void *laghu_apache_create_config(apr_pool_t *pool, char *path);
void *laghu_apache_create_server_config(apr_pool_t *pool, server_rec *server);
void laghu_apache_service_defaults(laghu_service_config *service);
bool laghu_apache_service_resolve(laghu_service_config *resolved,
                                  const laghu_service_config *parent,
                                  const laghu_service_config *child,
                                  const laghu_config *core,
                                  laghu_service_diagnostic *diagnostic);
void *laghu_apache_merge_config(apr_pool_t *pool, void *parent_value,
                                void *child_value);
const char *laghu_apache_command(cmd_parms *command, void *value,
                                 const char *arguments);
bool laghu_apache_normalize(request_rec *request,
                            laghu_apache_context *context);
bool laghu_apache_peer_matches(request_rec *request,
                               const laghu_service_cidr *cidrs, size_t count);
bool laghu_apache_apply_result(request_rec *request,
                               const laghu_http_transaction_result *result);
apr_status_t laghu_apache_transaction_filter(ap_filter_t *filter,
                                             apr_bucket_brigade *brigade);
void laghu_apache_insert_filter(request_rec *request);
int laghu_apache_variant_handler(request_rec *request);
int laghu_apache_admin_endpoint(request_rec *request,
                                laghu_apache_config *config);
int laghu_apache_beacon_endpoint(request_rec *request,
                                 laghu_apache_config *config);
int laghu_apache_asset_endpoint(request_rec *request,
                                laghu_apache_config *config);
int laghu_apache_post_config(apr_pool_t *configuration_pool,
                             apr_pool_t *log_pool, apr_pool_t *temporary_pool,
                             server_rec *server);

#endif
