// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/operational.h"
#include "laghu/rum.h"
#include "mod_laghu_internal.h"

#ifdef _WIN32
#define LAGHU_APACHE_LIFECYCLE_CACHE "C:/ProgramData/Laghu/images"
#else
#define LAGHU_APACHE_LIFECYCLE_CACHE "/var/cache/laghu/images"
#endif

laghu_rum_engine *laghu_apache_rum;
laghu_operational_registry laghu_apache_operational;
const char *laghu_apache_operational_cache;
bool laghu_apache_operational_enabled;

static apr_status_t laghu_apache_rum_cleanup(void *data) {
  (void)data;
  laghu_rum_engine_destroy(laghu_apache_rum);
  laghu_apache_rum = NULL;
  laghu_operational_registry_close(&laghu_apache_operational);
  return APR_SUCCESS;
}

void laghu_apache_child_init(apr_pool_t *pool, server_rec *server) {
  laghu_apache_config *config =
      ap_get_module_config(server->module_config, &laghu_module);
  laghu_rum_options options;
  char snapshot[LAGHU_RUNTIME_PATH_SIZE];
  char error[160U];
  int length;
  laghu_rum_options_init(&options);
  laghu_operational_registry_init(&laghu_apache_operational);
  if (laghu_apache_operational_enabled &&
      !laghu_operational_registry_open(
          &laghu_apache_operational, laghu_apache_operational_cache,
          LAGHU_OPERATIONAL_SURFACE_APACHE, LAGHU_OPERATIONAL_PROCESS_ADAPTER,
          true, (uint64_t)apr_time_sec(apr_time_now())))
    ap_log_error(
        APLOG_MARK, APLOG_WARNING, 0, server,
        "Laghu operational registry unavailable; observability disabled");
  if (config != NULL && config->core.mode == LAGHU_MODE_ON &&
      !laghu_cache_backend_register_path(config->image_cache != NULL
                                             ? config->image_cache
                                             : LAGHU_APACHE_LIFECYCLE_CACHE,
                                         &config->cache_limits))
    ap_log_error(APLOG_MARK, APLOG_WARNING, 0, server,
                 "Laghu file cache backend unavailable; serving origin");
  length =
      config != NULL && config->rum_snapshot != NULL
          ? snprintf(snapshot, sizeof(snapshot), "%s", config->rum_snapshot)
          : snprintf(snapshot, sizeof(snapshot), "%s/rum.snapshot",
                     LAGHU_APACHE_LIFECYCLE_CACHE);
  if (length <= 0 || (size_t)length >= sizeof(snapshot)) return;
  options.snapshot_path = snapshot;
  if (config != NULL) {
    options.store_uri = config->rum_store;
    options.client_library = config->rum_client_library;
    options.memory_limit = config->rum_memory_limit;
    options.pending_limit = config->rum_pending_limit;
    options.ttl_seconds = config->rum_ttl;
    options.sync_interval_seconds = config->rum_sync_interval;
    options.timeout_ms = config->rum_timeout_ms;
    options.retry_limit = config->rum_retry_limit;
    options.required = config->rum_required;
  }
  laghu_apache_rum = laghu_rum_engine_create(&options, error, sizeof(error));
  if (laghu_apache_rum == NULL) {
    ap_log_error(APLOG_MARK, options.required ? APLOG_CRIT : APLOG_WARNING, 0,
                 server, "Laghu RUM engine unavailable: %s", error);
    if (options.required) exit(APEXIT_CHILDFATAL);
    options.store_uri = "local:";
    options.client_library = NULL;
    options.required = false;
    laghu_apache_rum = laghu_rum_engine_create(&options, error, sizeof(error));
    if (laghu_apache_rum == NULL)
      ap_log_error(APLOG_MARK, APLOG_WARNING, 0, server,
                   "Laghu local RUM fallback unavailable: %s", error);
  }
  if (laghu_apache_rum != NULL)
    apr_pool_cleanup_register(pool, NULL, laghu_apache_rum_cleanup,
                              apr_pool_cleanup_null);
}
