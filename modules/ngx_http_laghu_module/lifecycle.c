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

#define LAGHU_NGINX_LIFECYCLE_CACHE "/var/cache/laghu/images"

laghu_rum_engine *ngx_http_laghu_rum;
laghu_operational_registry ngx_http_laghu_operational;
static ngx_event_t ngx_http_laghu_queue_retry_event;

static void ngx_http_laghu_open_operational_registry(ngx_cycle_t *cycle) {
  ngx_http_laghu_main_conf_t *conf;
  laghu_operational_snapshot snapshot;
  const char *cache_path;
  if (laghu_operational_registry_snapshot(&ngx_http_laghu_operational, &snapshot)) return;
  conf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_laghu_module);
  cache_path = conf != NULL && conf->operational_cache.len != 0U ? (const char *)conf->operational_cache.data : LAGHU_NGINX_LIFECYCLE_CACHE;
  laghu_operational_registry_close(&ngx_http_laghu_operational);
  laghu_operational_registry_init(&ngx_http_laghu_operational);
  (void)laghu_operational_registry_open(&ngx_http_laghu_operational, cache_path, LAGHU_OPERATIONAL_SURFACE_NGINX, LAGHU_OPERATIONAL_PROCESS_ADAPTER,
                                        true, (uint64_t)ngx_time());
}

static bool ngx_http_laghu_attach_queue(laghu_runtime_queue *queue, bool *attached, const char *path) {
  laghu_runtime_queue_snapshot snapshot;
  if (*attached) return laghu_runtime_queue_snapshot_get(queue, &snapshot);
  if (path == NULL || path[0] == '\0' || !laghu_runtime_queue_open(queue, path)) return false;
  if (!laghu_runtime_queue_snapshot_get(queue, &snapshot)) {
    laghu_runtime_queue_close(queue);
    return false;
  }
  *attached = true;
  return true;
}

static void ngx_http_laghu_refresh_runtime_queue_snapshot(ngx_http_laghu_loc_conf_t *conf) {
  laghu_runtime_queue_snapshot snapshot;
  uint64_t now = (uint64_t)ngx_time();
  conf->runtime_queue_capabilities = 0U;
  conf->runtime_queue_snapshot_ready = false;
  if (!conf->runtime_queue_attached || !laghu_runtime_queue_snapshot_get(&conf->runtime_queue, &snapshot)) return;
  if (snapshot.capabilities == 0U || snapshot.worker_heartbeat == 0U || snapshot.worker_heartbeat > now || now - snapshot.worker_heartbeat > 45U)
    return;
  conf->runtime_queue_capabilities = snapshot.capabilities;
  conf->runtime_queue_snapshot_ready = true;
}

static bool ngx_http_laghu_attach_config_queues(ngx_http_laghu_loc_conf_t *conf) {
  bool complete = true;
  if (conf == NULL || !conf->queue_registered) return true;
  if (!ngx_http_laghu_attach_queue(&conf->runtime_queue, &conf->runtime_queue_attached, conf->service.worker_queue)) complete = false;
  if (conf->service.font_providers != NULL &&
      !ngx_http_laghu_attach_queue(&conf->font_fetch_runtime_queue, &conf->font_fetch_runtime_queue_attached, conf->service.font_fetch_queue))
    complete = false;
  if (!ngx_http_laghu_attach_queue(&conf->javascript_runtime_queue, &conf->javascript_runtime_queue_attached, conf->service.javascript_queue))
    complete = false;
  if (conf->service.html_refresh_queue[0] != '\0' &&
      !ngx_http_laghu_attach_queue(&conf->html_refresh_runtime_queue, &conf->html_refresh_runtime_queue_attached, conf->service.html_refresh_queue))
    complete = false;
  if (conf->service.chrome_analysis_queue[0] != '\0' &&
      !ngx_http_laghu_attach_queue(&conf->chrome_analysis_runtime_queue, &conf->chrome_analysis_runtime_queue_attached,
                                   conf->service.chrome_analysis_queue))
    complete = false;
  if (conf->service.otel_trace_queue[0] != '\0' &&
      !ngx_http_laghu_attach_queue(&conf->otel_trace_runtime_queue, &conf->otel_trace_runtime_queue_attached, conf->service.otel_trace_queue))
    complete = false;
  ngx_http_laghu_refresh_runtime_queue_snapshot(conf);
  return complete;
}

static bool ngx_http_laghu_attach_all_queues(ngx_cycle_t *cycle) {
  ngx_http_laghu_main_conf_t *conf;
  ngx_http_laghu_loc_conf_t **entries;
  ngx_uint_t index;
  bool complete = true;
  conf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_laghu_module);
  if (conf == NULL || conf->queue_configs == NULL) return true;
  entries = conf->queue_configs->elts;
  for (index = 0U; index < conf->queue_configs->nelts; ++index) {
    if (!ngx_http_laghu_attach_config_queues(entries[index])) complete = false;
    (void)laghu_runtime_import_chrome_analysis(ngx_http_laghu_rum, entries[index]->service.chrome_analysis_output, (uint64_t)ngx_time(),
                                               entries[index]->service.rum_ttl);
  }
  return complete;
}

static void ngx_http_laghu_retry_queues(ngx_event_t *event) {
  ngx_cycle_t *cycle = event->data;
  if (cycle != NULL) {
    (void)ngx_http_laghu_attach_all_queues(cycle);
    ngx_http_laghu_open_operational_registry(cycle);
    ngx_add_timer(event, 1000U);
  }
}

static void ngx_http_laghu_close_config_queues(ngx_http_laghu_loc_conf_t *conf) {
  if (conf == NULL || !conf->queue_registered) return;
  if (conf->runtime_queue_attached) laghu_runtime_queue_close(&conf->runtime_queue);
  if (conf->font_fetch_runtime_queue_attached) laghu_runtime_queue_close(&conf->font_fetch_runtime_queue);
  if (conf->javascript_runtime_queue_attached) laghu_runtime_queue_close(&conf->javascript_runtime_queue);
  if (conf->html_refresh_runtime_queue_attached) laghu_runtime_queue_close(&conf->html_refresh_runtime_queue);
  if (conf->chrome_analysis_runtime_queue_attached) laghu_runtime_queue_close(&conf->chrome_analysis_runtime_queue);
  if (conf->otel_trace_runtime_queue_attached) laghu_runtime_queue_close(&conf->otel_trace_runtime_queue);
  conf->runtime_queue_attached = false;
  conf->font_fetch_runtime_queue_attached = false;
  conf->javascript_runtime_queue_attached = false;
  conf->html_refresh_runtime_queue_attached = false;
  conf->chrome_analysis_runtime_queue_attached = false;
  conf->otel_trace_runtime_queue_attached = false;
  conf->runtime_queue_capabilities = 0U;
  conf->runtime_queue_snapshot_ready = false;
}

static void ngx_http_laghu_close_all_queues(ngx_cycle_t *cycle) {
  ngx_http_laghu_main_conf_t *conf;
  ngx_http_laghu_loc_conf_t **entries;
  ngx_uint_t index;
  conf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_laghu_module);
  if (conf == NULL || conf->queue_configs == NULL) return;
  entries = conf->queue_configs->elts;
  for (index = 0U; index < conf->queue_configs->nelts; ++index) ngx_http_laghu_close_config_queues(entries[index]);
}

ngx_int_t ngx_http_laghu_init_process(ngx_cycle_t *cycle) {
  ngx_http_laghu_main_conf_t *conf;
  ngx_uint_t failure_level;
  laghu_rum_options options;
  char snapshot[LAGHU_RUNTIME_PATH_SIZE];
  char error[160U];
  int length;
  laghu_rum_options_init(&options);
  conf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_laghu_module);
  ngx_memzero(&ngx_http_laghu_queue_retry_event, sizeof(ngx_http_laghu_queue_retry_event));
  ngx_http_laghu_queue_retry_event.handler = ngx_http_laghu_retry_queues;
  ngx_http_laghu_queue_retry_event.data = cycle;
  ngx_http_laghu_queue_retry_event.log = cycle->log;
  (void)ngx_http_laghu_attach_all_queues(cycle);
  ngx_add_timer(&ngx_http_laghu_queue_retry_event, 1000U);
  ngx_http_laghu_open_operational_registry(cycle);
  length = conf != NULL && conf->service.rum_snapshot_path[0] != '\0'
               ? snprintf(snapshot, sizeof(snapshot), "%s", conf->service.rum_snapshot_path)
               : snprintf(snapshot, sizeof(snapshot), "%s/rum.snapshot", LAGHU_NGINX_LIFECYCLE_CACHE);
  if (length <= 0 || (size_t)length >= sizeof(snapshot)) return NGX_ERROR;
  options.snapshot_path = snapshot;
  if (conf != NULL) {
    options.store_uri = conf->service.rum_store;
    options.client_library = conf->service.rum_client_library[0] == '\0' ? NULL : conf->service.rum_client_library;
    options.memory_limit = conf->service.rum_memory_limit;
    options.pending_limit = conf->service.rum_pending_limit;
    options.ttl_seconds = conf->service.rum_ttl;
    options.sync_interval_seconds = conf->service.rum_sync_interval;
    options.timeout_ms = conf->service.rum_timeout_ms;
    options.retry_limit = conf->service.rum_retry_limit;
    options.required = conf->service.rum_store_required;
  }
  ngx_http_laghu_rum = laghu_rum_engine_create(&options, error, sizeof(error));
  if (ngx_http_laghu_rum == NULL) {
    failure_level = options.required ? NGX_LOG_EMERG : NGX_LOG_WARN;
    ngx_log_error(failure_level, cycle->log, 0, "laghu RUM engine unavailable: %s", error);
    if (options.required) return NGX_ERROR;
    options.store_uri = "local:";
    options.client_library = NULL;
    options.required = false;
    ngx_http_laghu_rum = laghu_rum_engine_create(&options, error, sizeof(error));
    if (ngx_http_laghu_rum == NULL) ngx_log_error(NGX_LOG_WARN, cycle->log, 0, "laghu local RUM fallback unavailable: %s", error);
  }
  return NGX_OK;
}

void ngx_http_laghu_exit_process(ngx_cycle_t *cycle) {
  if (ngx_http_laghu_queue_retry_event.timer_set) ngx_del_timer(&ngx_http_laghu_queue_retry_event);
  ngx_http_laghu_close_all_queues(cycle);
  laghu_operational_registry_close(&ngx_http_laghu_operational);
  laghu_rum_engine_destroy(ngx_http_laghu_rum);
  ngx_http_laghu_rum = NULL;
}
