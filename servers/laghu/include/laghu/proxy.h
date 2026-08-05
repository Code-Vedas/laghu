// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_PROXY_H
#define LAGHU_PROXY_H

#include <stdbool.h>
#include <stddef.h>

#include "laghu/http.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_PROXY_HEADER_BYTES 65536U
#define LAGHU_PROXY_LINE_BYTES 8192U
#define LAGHU_PROXY_REQUEST_BODY_BYTES 1048576U
#define LAGHU_PROXY_DEFAULT_WORKERS 4U
#define LAGHU_PROXY_DEFAULT_QUEUE 64U
#define LAGHU_PROXY_DEFAULT_CONNECT_TIMEOUT 5U
#define LAGHU_PROXY_DEFAULT_IO_TIMEOUT 30U
#define LAGHU_PROXY_DEFAULT_DRAIN_TIMEOUT 30U
#define LAGHU_PROXY_MAX_TRUSTED_PROXIES 64U

typedef enum {
  LAGHU_PROXY_FORWARDED_OFF = 0,
  LAGHU_PROXY_FORWARDED_STANDARD,
  LAGHU_PROXY_FORWARDED_X,
  LAGHU_PROXY_FORWARDED_BOTH
} laghu_proxy_forwarded_mode;

typedef struct {
  unsigned char address[16];
  unsigned int family;
  unsigned int prefix;
} laghu_proxy_cidr;

typedef struct {
  char listen_host[256];
  char listen_port[6];
  char origin_host[256];
  char origin_port[6];
  char origin_authority[264];
  char origin_ca_file[LAGHU_RUNTIME_PATH_SIZE];
  char cache_backend_uri[LAGHU_RUNTIME_PATH_SIZE];
  char cache_path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_cache_limits cache_limits;
  char worker_queue_path[LAGHU_RUNTIME_PATH_SIZE];
  char font_fetch_queue_path[LAGHU_RUNTIME_PATH_SIZE];
  char font_provider_config_path[LAGHU_RUNTIME_PATH_SIZE];
  char javascript_queue_path[LAGHU_RUNTIME_PATH_SIZE];
  char javascript_target[LAGHU_JAVASCRIPT_TARGET_SIZE];
  char javascript_observation_config_path[LAGHU_RUNTIME_PATH_SIZE];
  char javascript_defer_config_path[LAGHU_RUNTIME_PATH_SIZE];
  char asset_offload_config_path[LAGHU_RUNTIME_PATH_SIZE];
  char asset_upload_queue_path[LAGHU_RUNTIME_PATH_SIZE];
  char rum_store[LAGHU_RUNTIME_PATH_SIZE];
  char rum_snapshot_path[LAGHU_RUNTIME_PATH_SIZE];
  char rum_client_library[LAGHU_RUNTIME_PATH_SIZE];
  laghu_font_provider_set font_providers;
  laghu_javascript_observation_set javascript_observations;
  laghu_javascript_defer_set javascript_defer;
  laghu_asset_config asset_offload;
  laghu_source_policy source_policy;
  laghu_config config;
  unsigned int workers;
  unsigned int connection_queue;
  unsigned int connect_timeout;
  unsigned int io_timeout;
  unsigned int drain_timeout;
  unsigned int rum_timeout_ms;
  unsigned int rum_ttl;
  unsigned int rum_retry_limit;
  unsigned int rum_sync_interval;
  size_t rum_memory_limit;
  size_t rum_pending_limit;
  laghu_proxy_forwarded_mode forwarded_mode;
  laghu_proxy_cidr trusted_proxies[LAGHU_PROXY_MAX_TRUSTED_PROXIES];
  size_t trusted_proxy_count;
  laghu_proxy_cidr purge_allow[LAGHU_PROXY_MAX_TRUSTED_PROXIES];
  size_t purge_allow_count;
  char purge_token_file[LAGHU_RUNTIME_PATH_SIZE];
  char cache_flush_file[LAGHU_RUNTIME_PATH_SIZE];
  bool purge_method;
  bool purge_query;
  bool statistics;
  bool metrics;
  bool readiness;
  bool readiness_strict;
  bool origin_tls;
  bool font_providers_loaded;
  bool javascript_queue_enabled;
  bool javascript_observations_loaded;
  bool javascript_defer_loaded;
  bool asset_offload_loaded;
  bool service_mode;
  bool rum_store_required;
} laghu_proxy_options;

typedef enum {
  LAGHU_PROXY_PARSE_OK = 0,
  LAGHU_PROXY_PARSE_HELP,
  LAGHU_PROXY_PARSE_VERSION,
  LAGHU_PROXY_PARSE_ERROR
} laghu_proxy_parse_result;

void laghu_proxy_options_init(laghu_proxy_options *options);
laghu_proxy_parse_result laghu_proxy_parse_options(int argc, char **argv,
                                                   laghu_proxy_options *options,
                                                   char *error,
                                                   size_t error_size);
bool laghu_proxy_decode_chunked(laghu_buffer encoded, unsigned char *decoded,
                                size_t capacity, size_t *decoded_length);
int laghu_proxy_run(const laghu_proxy_options *options);
#ifdef _WIN32
int laghu_proxy_run_service(const laghu_proxy_options *options);
#endif

#ifdef __cplusplus
}
#endif

#endif
