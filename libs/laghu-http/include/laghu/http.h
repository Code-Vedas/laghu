// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_HTTP_H
#define LAGHU_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_HTTP_ABI_VERSION 1U
#define LAGHU_HTTP_MAX_REQUEST_HEADERS 64U
#define LAGHU_HTTP_MAX_RESPONSE_HEADERS 64U
#define LAGHU_HTTP_MAX_HEADER_OPERATIONS 32U
#define LAGHU_HTTP_MAX_HEADER_NAME 64U
#define LAGHU_HTTP_MAX_HEADER_VALUE 8192U

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
  const char *worker_queue_path;
  laghu_runtime_queue *queue;
  const char *font_fetch_queue_path;
  laghu_runtime_queue *font_fetch_queue;
  const char *javascript_queue_path;
  laghu_runtime_queue *javascript_queue;
  const char *javascript_target;
  const laghu_font_provider_set *font_providers;
  const laghu_javascript_observation_set *javascript_observations;
  uint64_t now;
} laghu_http_environment;

typedef enum {
  LAGHU_HTTP_HEADER_SET = 0,
  LAGHU_HTTP_HEADER_APPEND,
  LAGHU_HTTP_HEADER_REMOVE
} laghu_http_header_operation_kind;

typedef struct {
  laghu_http_header_operation_kind kind;
  char name[LAGHU_HTTP_MAX_HEADER_NAME + 1U];
  char *value;
} laghu_http_header_operation;

typedef enum {
  LAGHU_HTTP_ACTION_BYPASS = 0,
  LAGHU_HTTP_ACTION_CAPTURE_HTML,
  LAGHU_HTTP_ACTION_CAPTURE_CSS,
  LAGHU_HTTP_ACTION_CAPTURE_JAVASCRIPT,
  LAGHU_HTTP_ACTION_CAPTURE_IMAGE,
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
  size_t capture_limit;
  laghu_http_header_operation
      header_operations[LAGHU_HTTP_MAX_HEADER_OPERATIONS];
  size_t header_operation_count;
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];
  char cache_key[LAGHU_RUNTIME_KEY_SIZE];
  bool job_published;
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
  bool accept_webp;
  bool prepared;
} laghu_http_transaction;

void laghu_http_transaction_init(laghu_http_transaction *transaction);
bool laghu_http_transaction_prepare(laghu_http_transaction *transaction,
                                    const laghu_http_request *request,
                                    const laghu_http_response *response,
                                    const laghu_http_environment *environment,
                                    laghu_http_transaction_result *result);
bool laghu_http_transaction_finalize(laghu_http_transaction *transaction,
                                     laghu_buffer captured_body,
                                     laghu_http_transaction_result *result);
void laghu_http_transaction_result_release(
    laghu_http_transaction_result *result);

#ifdef __cplusplus
}
#endif

#endif
