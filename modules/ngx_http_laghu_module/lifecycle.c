// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>
#include <stdio.h>

#include "laghu/operational.h"
#include "laghu/rum.h"
#include "laghu/types.h"
#include "ngx_http_laghu_internal.h"

#ifdef _WIN32
#define LAGHU_NGINX_LIFECYCLE_CACHE "C:/ProgramData/Laghu/images"
#else
#define LAGHU_NGINX_LIFECYCLE_CACHE "/var/cache/laghu/images"
#endif

laghu_rum_engine *ngx_http_laghu_rum;
laghu_operational_registry ngx_http_laghu_operational;

ngx_int_t ngx_http_laghu_init_process(ngx_cycle_t *cycle) {
  ngx_http_laghu_main_conf_t *conf;
  ngx_uint_t failure_level;
  laghu_rum_options options;
  char snapshot[LAGHU_RUNTIME_PATH_SIZE];
  char error[160U];
  int length;
  laghu_rum_options_init(&options);
  conf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_laghu_module);
  laghu_operational_registry_init(&ngx_http_laghu_operational);
  (void)laghu_operational_registry_open(
      &ngx_http_laghu_operational,
      conf != NULL && conf->operational_cache.len != 0U
          ? (const char *)conf->operational_cache.data
          : LAGHU_NGINX_LIFECYCLE_CACHE,
      LAGHU_OPERATIONAL_SURFACE_NGINX, LAGHU_OPERATIONAL_PROCESS_ADAPTER, true,
      (uint64_t)ngx_time());
  length = conf != NULL && conf->snapshot_path.len != 0U
               ? snprintf(snapshot, sizeof(snapshot), "%s",
                          (const char *)conf->snapshot_path.data)
               : snprintf(snapshot, sizeof(snapshot), "%s/rum.snapshot",
                          LAGHU_NGINX_LIFECYCLE_CACHE);
  if (length <= 0 || (size_t)length >= sizeof(snapshot)) return NGX_ERROR;
  options.snapshot_path = snapshot;
  if (conf != NULL) {
    options.store_uri = (const char *)conf->store_uri.data;
    options.client_library = conf->client_library.len == 0U
                                 ? NULL
                                 : (const char *)conf->client_library.data;
    options.memory_limit = conf->memory_limit;
    options.pending_limit = conf->pending_limit;
    options.ttl_seconds = (unsigned int)conf->ttl_seconds;
    options.sync_interval_seconds = (unsigned int)conf->sync_interval_seconds;
    options.timeout_ms = (unsigned int)conf->timeout_ms;
    options.retry_limit = (unsigned int)conf->retry_limit;
    options.required = conf->required == 1;
  }
  ngx_http_laghu_rum = laghu_rum_engine_create(&options, error, sizeof(error));
  if (ngx_http_laghu_rum == NULL) {
    failure_level = options.required ? NGX_LOG_EMERG : NGX_LOG_WARN;
    ngx_log_error(failure_level, cycle->log, 0,
                  "laghu RUM engine unavailable: %s", error);
    if (options.required) return NGX_ERROR;
    options.store_uri = "local:";
    options.client_library = NULL;
    options.required = false;
    ngx_http_laghu_rum =
        laghu_rum_engine_create(&options, error, sizeof(error));
    if (ngx_http_laghu_rum == NULL)
      ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                    "laghu local RUM fallback unavailable: %s", error);
  }
  return NGX_OK;
}

void ngx_http_laghu_exit_process(ngx_cycle_t *cycle) {
  (void)cycle;
  laghu_operational_registry_close(&ngx_http_laghu_operational);
  laghu_rum_engine_destroy(ngx_http_laghu_rum);
  ngx_http_laghu_rum = NULL;
}
