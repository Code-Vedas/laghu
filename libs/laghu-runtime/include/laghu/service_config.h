// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_SERVICE_CONFIG_H
#define LAGHU_SERVICE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/assets.h"
#include "laghu/cache.h"
#include "laghu/fonts.h"
#include "laghu/javascript.h"
#include "laghu/layout.h"
#include "laghu/rum.h"
#include "laghu/source.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_SERVICE_CONFIG_MAX_CIDRS 64U
#define LAGHU_SERVICE_CONFIG_DIAGNOSTIC_SIZE 160U
/* Native adapters normalize AF_INET/AF_INET6 to these values before matching.
 */
#define LAGHU_SERVICE_CIDR_FAMILY_IPV4 4U
#define LAGHU_SERVICE_CIDR_FAMILY_IPV6 6U

typedef enum {
  LAGHU_SERVICE_SETTING_UNKNOWN = 0,
  LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND,
  LAGHU_SERVICE_SETTING_IMAGE_CACHE,
  LAGHU_SERVICE_SETTING_FILE_CACHE_SIZE,
  LAGHU_SERVICE_SETTING_FILE_CACHE_INODE_LIMIT,
  LAGHU_SERVICE_SETTING_FILE_CACHE_CLEAN_INTERVAL,
  LAGHU_SERVICE_SETTING_FILE_CACHE_METADATA_SIZE,
  LAGHU_SERVICE_SETTING_WORKER_QUEUE,
  LAGHU_SERVICE_SETTING_HTML_REFRESH_QUEUE,
  LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_QUEUE,
  LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_OUTPUT,
  LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_TIMEOUT,
  LAGHU_SERVICE_SETTING_FONT_FETCH_QUEUE,
  LAGHU_SERVICE_SETTING_FONT_PROVIDER_CONFIG,
  LAGHU_SERVICE_SETTING_JAVASCRIPT_QUEUE,
  LAGHU_SERVICE_SETTING_JAVASCRIPT_TARGET,
  LAGHU_SERVICE_SETTING_JAVASCRIPT_OBSERVATION_CONFIG,
  LAGHU_SERVICE_SETTING_JAVASCRIPT_DEFER_CONFIG,
  LAGHU_SERVICE_SETTING_LAYOUT_RESERVATION_CONFIG,
  LAGHU_SERVICE_SETTING_ASSET_OFFLOAD_CONFIG,
  LAGHU_SERVICE_SETTING_ASSET_UPLOAD_QUEUE,
  LAGHU_SERVICE_SETTING_RUM_STORE,
  LAGHU_SERVICE_SETTING_RUM_STORE_LOCAL_SNAPSHOT,
  LAGHU_SERVICE_SETTING_RUM_STORE_CLIENT_LIBRARY,
  LAGHU_SERVICE_SETTING_RUM_STORE_REQUIRED,
  LAGHU_SERVICE_SETTING_RUM_STORE_TIMEOUT,
  LAGHU_SERVICE_SETTING_RUM_STORE_TTL,
  LAGHU_SERVICE_SETTING_RUM_STORE_RETRY_LIMIT,
  LAGHU_SERVICE_SETTING_RUM_STORE_SYNC_INTERVAL,
  LAGHU_SERVICE_SETTING_RUM_STORE_MEMORY_LIMIT,
  LAGHU_SERVICE_SETTING_RUM_STORE_PENDING_LIMIT,
  LAGHU_SERVICE_SETTING_LOAD_FROM_FILE,
  LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP,
  LAGHU_SERVICE_SETTING_PURGE_METHOD,
  LAGHU_SERVICE_SETTING_PURGE_QUERY,
  LAGHU_SERVICE_SETTING_STATISTICS,
  LAGHU_SERVICE_SETTING_METRICS,
  LAGHU_SERVICE_SETTING_READINESS,
  LAGHU_SERVICE_SETTING_READINESS_POLICY,
  LAGHU_SERVICE_SETTING_PURGE_TOKEN_FILE,
  LAGHU_SERVICE_SETTING_CACHE_FLUSH_FILE,
  LAGHU_SERVICE_SETTING_PURGE_ALLOW,
  LAGHU_SERVICE_SETTING_TRUSTED_PROXY,
  LAGHU_SERVICE_SETTING_COUNT
} laghu_service_setting;

typedef enum {
  LAGHU_SERVICE_VALUE_STRING = 0,
  LAGHU_SERVICE_VALUE_BOOLEAN,
  LAGHU_SERVICE_VALUE_UNSIGNED,
  LAGHU_SERVICE_VALUE_SIZE,
  LAGHU_SERVICE_VALUE_DURATION,
  LAGHU_SERVICE_VALUE_ENUM,
  LAGHU_SERVICE_VALUE_CIDR,
  LAGHU_SERVICE_VALUE_PAIR
} laghu_service_value_type;

typedef enum { LAGHU_SERVICE_INHERIT_SCALAR = 0, LAGHU_SERVICE_INHERIT_REPLACE, LAGHU_SERVICE_INHERIT_APPEND } laghu_service_inheritance;

typedef enum {
  LAGHU_SERVICE_DIAGNOSTIC_NONE = 0,
  LAGHU_SERVICE_DIAGNOSTIC_ARGUMENT,
  LAGHU_SERVICE_DIAGNOSTIC_DUPLICATE,
  LAGHU_SERVICE_DIAGNOSTIC_RANGE,
  LAGHU_SERVICE_DIAGNOSTIC_FORMAT,
  LAGHU_SERVICE_DIAGNOSTIC_CAPACITY,
  LAGHU_SERVICE_DIAGNOSTIC_CONFLICT,
  LAGHU_SERVICE_DIAGNOSTIC_DEPENDENCY,
  LAGHU_SERVICE_DIAGNOSTIC_IO
} laghu_service_diagnostic_code;

typedef struct {
  laghu_service_setting setting;
  const char *name;
  laghu_service_value_type type;
  laghu_service_inheritance inheritance;
  uint64_t minimum;
  uint64_t maximum;
  bool repeatable;
} laghu_service_setting_descriptor;

typedef struct {
  unsigned char address[16];
  unsigned int family;
  unsigned int prefix;
} laghu_service_cidr;

typedef struct {
  laghu_service_diagnostic_code code;
  laghu_service_setting setting;
  char message[LAGHU_SERVICE_CONFIG_DIAGNOSTIC_SIZE];
} laghu_service_diagnostic;

typedef struct {
  bool native_file_loading;
  bool require_cache;
  bool require_worker_queue;
  bool require_admin_authorization;
  bool respect_x_forwarded_proto;
} laghu_service_finalize_options;

typedef struct {
  uint64_t present;
  /* The explicitly configured backend URI.  For the file backend, finalize
   * decodes it into image_cache; consumers use image_cache as the resolved
   * local cache path regardless of whether legacy ImageCache was configured. */
  char file_cache_backend[LAGHU_RUNTIME_PATH_SIZE];
  char image_cache[LAGHU_RUNTIME_PATH_SIZE];
  laghu_cache_limits cache_limits;
  char worker_queue[LAGHU_RUNTIME_PATH_SIZE];
  char html_refresh_queue[LAGHU_RUNTIME_PATH_SIZE];
  /* An explicit queue opts in to asynchronous browser analysis. */
  char chrome_analysis_queue[LAGHU_RUNTIME_PATH_SIZE];
  char chrome_analysis_output[LAGHU_RUNTIME_PATH_SIZE];
  char font_fetch_queue[LAGHU_RUNTIME_PATH_SIZE];
  char font_provider_config[LAGHU_RUNTIME_PATH_SIZE];
  char javascript_queue[LAGHU_RUNTIME_PATH_SIZE];
  char javascript_target[LAGHU_JAVASCRIPT_TARGET_SIZE];
  char javascript_observation_config[LAGHU_RUNTIME_PATH_SIZE];
  char javascript_defer_config[LAGHU_RUNTIME_PATH_SIZE];
  char layout_reservation_config[LAGHU_RUNTIME_PATH_SIZE];
  char asset_offload_config[LAGHU_RUNTIME_PATH_SIZE];
  char asset_upload_queue[LAGHU_RUNTIME_PATH_SIZE];
  char rum_store[LAGHU_RUNTIME_PATH_SIZE];
  char rum_snapshot_path[LAGHU_RUNTIME_PATH_SIZE];
  char rum_client_library[LAGHU_RUNTIME_PATH_SIZE];
  char purge_token_file[LAGHU_RUNTIME_PATH_SIZE];
  char cache_flush_file[LAGHU_RUNTIME_PATH_SIZE];
  /* Prepared records are allocated only during startup finalization.  Keeping
   * them indirect preserves normal thread stacks for config merge callers. */
  laghu_font_provider_set *font_providers;
  laghu_javascript_observation_set *javascript_observations;
  laghu_javascript_defer_set *javascript_defer;
  laghu_layout_reservation_set *layout_reservations;
  laghu_asset_config *asset_offload;
  laghu_source_policy source_policy;
  laghu_service_cidr purge_allow[LAGHU_SERVICE_CONFIG_MAX_CIDRS];
  laghu_service_cidr trusted_proxies[LAGHU_SERVICE_CONFIG_MAX_CIDRS];
  size_t purge_allow_count;
  size_t trusted_proxy_count;
  unsigned int rum_timeout_ms;
  unsigned int chrome_analysis_timeout_ms;
  unsigned int rum_ttl;
  unsigned int rum_retry_limit;
  unsigned int rum_sync_interval;
  size_t rum_memory_limit;
  size_t rum_pending_limit;
  bool purge_method;
  bool purge_query;
  bool statistics;
  bool metrics;
  bool readiness;
  bool readiness_strict;
  bool rum_store_required;
  laghu_font_provider_set *owned_font_providers;
  laghu_javascript_observation_set *owned_javascript_observations;
  laghu_javascript_defer_set *owned_javascript_defer;
  laghu_layout_reservation_set *owned_layout_reservations;
  laghu_asset_config *owned_asset_offload;
} laghu_service_config;

void laghu_service_config_init(laghu_service_config *config);
void laghu_service_config_dispose(laghu_service_config *config);
laghu_service_setting laghu_service_setting_find(const char *name);
const laghu_service_setting_descriptor *laghu_service_setting_describe(laghu_service_setting setting);
bool laghu_service_config_apply(laghu_service_config *config, laghu_service_setting setting, const char *value, laghu_service_diagnostic *diagnostic);
bool laghu_service_config_apply_pair(laghu_service_config *config, laghu_service_setting setting, const char *first, const char *second,
                                     laghu_service_diagnostic *diagnostic);
bool laghu_service_config_merge(laghu_service_config *merged, const laghu_service_config *parent, const laghu_service_config *child,
                                laghu_service_diagnostic *diagnostic);
/* Resolves backend values and checks cross-setting invariants without file,
 * network, queue, or other request-path I/O.  File-backed resources must
 * already have been prepared. */
bool laghu_service_config_validate(laghu_service_config *config, const laghu_service_finalize_options *options, laghu_service_diagnostic *diagnostic);
bool laghu_service_config_finalize(laghu_service_config *config, const laghu_service_finalize_options *options, laghu_service_diagnostic *diagnostic);
/* Startup-only. Loads file-backed service resources once; it performs no
 * request-path I/O and clears every prepared object after any load failure. */
bool laghu_service_config_prepare_resources(laghu_service_config *config, laghu_service_diagnostic *diagnostic);
bool laghu_service_cidr_parse(const char *value, laghu_service_cidr *cidr);
bool laghu_service_cidr_equal(const laghu_service_cidr *left, const laghu_service_cidr *right);
bool laghu_service_cidr_matches(const laghu_service_cidr *cidr, const unsigned char address[16],
                                /* LAGHU_SERVICE_CIDR_FAMILY_IPV4 or IPV6. */
                                unsigned int family);

#ifdef __cplusplus
}
#endif

#endif
