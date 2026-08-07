// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/operational.h"
#include "laghu/rum.h"
#include "mod_laghu_internal.h"

#define LAGHU_APACHE_LIFECYCLE_CACHE "/var/cache/laghu/images"

laghu_rum_engine *laghu_apache_rum;
laghu_operational_registry laghu_apache_operational;
const char *laghu_apache_operational_cache;
bool laghu_apache_operational_enabled;

struct laghu_apache_queue_binding {
  const laghu_apache_config *parent;
  const laghu_apache_config *child;
  laghu_runtime_queue image_queue;
  laghu_runtime_queue font_queue;
  laghu_runtime_queue javascript_queue;
  laghu_runtime_queue html_refresh_queue;
  char image_queue_path[LAGHU_RUNTIME_PATH_SIZE];
  char font_queue_path[LAGHU_RUNTIME_PATH_SIZE];
  char javascript_queue_path[LAGHU_RUNTIME_PATH_SIZE];
  char html_refresh_queue_path[LAGHU_RUNTIME_PATH_SIZE];
  bool font_enabled;
  volatile apr_uint32_t image_attached;
  volatile apr_uint32_t font_attached;
  volatile apr_uint32_t javascript_attached;
  volatile apr_uint32_t html_refresh_attached;
  volatile apr_uint32_t html_refresh_dedup_lock;
  uint64_t html_refresh_until[LAGHU_APACHE_HTML_REFRESH_DEDUP];
  char html_refresh_keys[LAGHU_APACHE_HTML_REFRESH_DEDUP]
                        [LAGHU_RUNTIME_KEY_SIZE];
};

static laghu_apache_queue_binding
    laghu_apache_queue_bindings[LAGHU_APACHE_QUEUE_CONFIG_LIMIT];
static size_t laghu_apache_queue_binding_count;
static apr_thread_t *laghu_apache_queue_thread;
static volatile apr_uint32_t laghu_apache_queue_stopping;

void laghu_apache_queue_registry_reset(void) {
  memset(laghu_apache_queue_bindings, 0, sizeof(laghu_apache_queue_bindings));
  laghu_apache_queue_binding_count = 0U;
}

bool laghu_apache_queue_registry_add(const laghu_apache_config *parent,
                                     const laghu_apache_config *child,
                                     const laghu_service_config *service) {
  laghu_apache_queue_binding *binding;
  size_t index;
  if (parent == NULL || child == NULL || service == NULL) return false;
  for (index = 0U; index < laghu_apache_queue_binding_count; ++index)
    if (laghu_apache_queue_bindings[index].parent == parent &&
        laghu_apache_queue_bindings[index].child == child)
      return true;
  for (index = 0U; index < laghu_apache_queue_binding_count; ++index) {
    laghu_apache_queue_binding *existing = &laghu_apache_queue_bindings[index];
    if (existing->font_enabled == (service->font_providers != NULL) &&
        strcmp(existing->image_queue_path, service->worker_queue) == 0 &&
        strcmp(existing->javascript_queue_path, service->javascript_queue) ==
            0 &&
        strcmp(existing->html_refresh_queue_path,
               service->html_refresh_queue) == 0 &&
        (!existing->font_enabled ||
         strcmp(existing->font_queue_path, service->font_fetch_queue) == 0))
      return true;
  }
  if (laghu_apache_queue_binding_count >= LAGHU_APACHE_QUEUE_CONFIG_LIMIT)
    return false;
  binding = &laghu_apache_queue_bindings[laghu_apache_queue_binding_count++];
  binding->parent = parent;
  binding->child = child;
  binding->font_enabled = service->font_providers != NULL;
  (void)snprintf(binding->image_queue_path, sizeof(binding->image_queue_path),
                 "%s", service->worker_queue);
  (void)snprintf(binding->font_queue_path, sizeof(binding->font_queue_path),
                 "%s", service->font_fetch_queue);
  (void)snprintf(binding->javascript_queue_path,
                 sizeof(binding->javascript_queue_path), "%s",
                 service->javascript_queue);
  (void)snprintf(binding->html_refresh_queue_path,
                 sizeof(binding->html_refresh_queue_path), "%s",
                 service->html_refresh_queue);
  laghu_runtime_queue_init(&binding->image_queue);
  laghu_runtime_queue_init(&binding->font_queue);
  laghu_runtime_queue_init(&binding->javascript_queue);
  laghu_runtime_queue_init(&binding->html_refresh_queue);
  return true;
}

laghu_apache_queue_binding *laghu_apache_queue_binding_find_service(
    const laghu_service_config *service) {
  bool font_enabled;
  size_t index;
  if (service == NULL) return NULL;
  font_enabled = service->font_providers != NULL;
  for (index = 0U; index < laghu_apache_queue_binding_count; ++index) {
    laghu_apache_queue_binding *binding = &laghu_apache_queue_bindings[index];
    if (binding->font_enabled == font_enabled &&
        strcmp(binding->image_queue_path, service->worker_queue) == 0 &&
        strcmp(binding->javascript_queue_path, service->javascript_queue) ==
            0 &&
        strcmp(binding->html_refresh_queue_path, service->html_refresh_queue) ==
            0 &&
        (!font_enabled ||
         strcmp(binding->font_queue_path, service->font_fetch_queue) == 0))
      return binding;
  }
  return NULL;
}

static laghu_runtime_queue *laghu_apache_attached_queue(
    laghu_apache_queue_binding *binding, unsigned int kind) {
  if (binding == NULL) return NULL;
  if (kind == 0U && apr_atomic_read32(&binding->image_attached) != 0U)
    return (laghu_runtime_queue *)&binding->image_queue;
  if (kind == 1U && binding->font_enabled &&
      apr_atomic_read32(&binding->font_attached) != 0U)
    return (laghu_runtime_queue *)&binding->font_queue;
  if (kind == 2U && apr_atomic_read32(&binding->javascript_attached) != 0U)
    return (laghu_runtime_queue *)&binding->javascript_queue;
  if (kind == 3U && apr_atomic_read32(&binding->html_refresh_attached) != 0U)
    return (laghu_runtime_queue *)&binding->html_refresh_queue;
  return NULL;
}

laghu_runtime_queue *laghu_apache_image_queue(laghu_apache_config *config) {
  return config == NULL
             ? NULL
             : laghu_apache_attached_queue(config->queue_binding, 0U);
}

laghu_runtime_queue *laghu_apache_font_queue(laghu_apache_config *config) {
  return config == NULL
             ? NULL
             : laghu_apache_attached_queue(config->queue_binding, 1U);
}

laghu_runtime_queue *laghu_apache_javascript_queue(
    laghu_apache_config *config) {
  return config == NULL
             ? NULL
             : laghu_apache_attached_queue(config->queue_binding, 2U);
}

laghu_runtime_queue *laghu_apache_html_refresh_queue(
    laghu_apache_config *config) {
  return config == NULL
             ? NULL
             : laghu_apache_attached_queue(config->queue_binding, 3U);
}

bool laghu_apache_html_refresh_try_publish(laghu_apache_config *config,
                                           const laghu_runtime_job *job,
                                           uint64_t now) {
  laghu_apache_queue_binding *binding;
  laghu_runtime_queue *queue;
  unsigned int index, candidate = 0U;
  bool published = false;
  if (config == NULL || job == NULL ||
      job->kind != LAGHU_RUNTIME_JOB_HTML_REFRESH)
    return false;
  binding = config->queue_binding;
  queue = laghu_apache_html_refresh_queue(config);
  if (binding == NULL || queue == NULL ||
      apr_atomic_cas32(&binding->html_refresh_dedup_lock, 1U, 0U) != 0U)
    return false;
  for (index = 0U; index < LAGHU_APACHE_HTML_REFRESH_DEDUP; ++index) {
    if (binding->html_refresh_until[index] > now &&
        memcmp(binding->html_refresh_keys[index], job->index_key,
               sizeof(job->index_key)) == 0) {
      goto done;
    }
    if (binding->html_refresh_until[index] <
        binding->html_refresh_until[candidate])
      candidate = index;
  }
  if (laghu_runtime_queue_try_publish(queue, job)) {
    memcpy(binding->html_refresh_keys[candidate], job->index_key,
           sizeof(job->index_key));
    binding->html_refresh_until[candidate] = now + 1U;
    published = true;
  }
done:
  apr_atomic_set32(&binding->html_refresh_dedup_lock, 0U);
  return published;
}

static bool laghu_apache_attach_queue(laghu_runtime_queue *queue,
                                      volatile apr_uint32_t *attached,
                                      const char *path) {
  laghu_runtime_queue_snapshot snapshot;
  if (apr_atomic_read32(attached) != 0U) return true;
  if (path == NULL || path[0] == '\0' || !laghu_runtime_queue_open(queue, path))
    return false;
  if (!laghu_runtime_queue_snapshot_get(queue, &snapshot)) {
    laghu_runtime_queue_close(queue);
    return false;
  }
  apr_atomic_set32(attached, 1U);
  return true;
}

static bool laghu_apache_attach_all_queues(void) {
  size_t index;
  bool complete = true;
  for (index = 0U; index < laghu_apache_queue_binding_count; ++index) {
    laghu_apache_queue_binding *binding = &laghu_apache_queue_bindings[index];
    if (!laghu_apache_attach_queue(&binding->image_queue,
                                   &binding->image_attached,
                                   binding->image_queue_path))
      complete = false;
    if (binding->font_enabled &&
        !laghu_apache_attach_queue(&binding->font_queue,
                                   &binding->font_attached,
                                   binding->font_queue_path))
      complete = false;
    if (!laghu_apache_attach_queue(&binding->javascript_queue,
                                   &binding->javascript_attached,
                                   binding->javascript_queue_path))
      complete = false;
    if (binding->html_refresh_queue_path[0] != '\0' &&
        !laghu_apache_attach_queue(&binding->html_refresh_queue,
                                   &binding->html_refresh_attached,
                                   binding->html_refresh_queue_path))
      complete = false;
  }
  return complete;
}

static void *APR_THREAD_FUNC
laghu_apache_queue_maintenance(apr_thread_t *thread, void *data) {
  unsigned int tick;
  (void)thread;
  (void)data;
  while (apr_atomic_read32(&laghu_apache_queue_stopping) == 0U) {
    (void)laghu_apache_attach_all_queues();
    for (tick = 0U;
         tick < 10U && apr_atomic_read32(&laghu_apache_queue_stopping) == 0U;
         ++tick)
      apr_sleep(100000U);
  }
  return NULL;
}

static void laghu_apache_close_all_queues(void) {
  size_t index;
  for (index = 0U; index < laghu_apache_queue_binding_count; ++index) {
    laghu_apache_queue_binding *binding = &laghu_apache_queue_bindings[index];
    if (apr_atomic_read32(&binding->image_attached) != 0U)
      laghu_runtime_queue_close(&binding->image_queue);
    if (apr_atomic_read32(&binding->font_attached) != 0U)
      laghu_runtime_queue_close(&binding->font_queue);
    if (apr_atomic_read32(&binding->javascript_attached) != 0U)
      laghu_runtime_queue_close(&binding->javascript_queue);
    if (apr_atomic_read32(&binding->html_refresh_attached) != 0U)
      laghu_runtime_queue_close(&binding->html_refresh_queue);
    apr_atomic_set32(&binding->image_attached, 0U);
    apr_atomic_set32(&binding->font_attached, 0U);
    apr_atomic_set32(&binding->javascript_attached, 0U);
    apr_atomic_set32(&binding->html_refresh_attached, 0U);
  }
}

static bool laghu_apache_open_operational(uint64_t now) {
  unsigned int attempt;
  /* Apache starts several children together.  Registry locking is deliberately
   * nonblocking, so make a bounded startup-only retry before giving a child an
   * unavailable operational view. */
  for (attempt = 0U; attempt < 16U; ++attempt) {
    if (laghu_operational_registry_open(
            &laghu_apache_operational, laghu_apache_operational_cache,
            LAGHU_OPERATIONAL_SURFACE_APACHE, LAGHU_OPERATIONAL_PROCESS_ADAPTER,
            true, now))
      return true;
  }
  return false;
}

static apr_status_t laghu_apache_rum_cleanup(void *data) {
  apr_status_t exit_status;
  (void)data;
  apr_atomic_set32(&laghu_apache_queue_stopping, 1U);
  if (laghu_apache_queue_thread != NULL) {
    (void)apr_thread_join(&exit_status, laghu_apache_queue_thread);
    laghu_apache_queue_thread = NULL;
  }
  laghu_apache_close_all_queues();
  laghu_rum_engine_destroy(laghu_apache_rum);
  laghu_apache_rum = NULL;
  laghu_operational_registry_close(&laghu_apache_operational);
  return APR_SUCCESS;
}

void laghu_apache_child_init(apr_pool_t *pool, server_rec *server) {
  laghu_apache_config *config =
      ap_get_module_config(server->module_config, &laghu_module);
  laghu_rum_options options;
  bool queues_attached;
  char snapshot[LAGHU_RUNTIME_PATH_SIZE];
  char error[160U];
  int length;
  laghu_rum_options_init(&options);
  (void)apr_atomic_init(pool);
  apr_atomic_set32(&laghu_apache_queue_stopping, 0U);
  laghu_apache_queue_thread = NULL;
  queues_attached = laghu_apache_attach_all_queues();
  if (!queues_attached && laghu_apache_queue_binding_count != 0U &&
      apr_thread_create(&laghu_apache_queue_thread, NULL,
                        laghu_apache_queue_maintenance, NULL,
                        pool) != APR_SUCCESS) {
    laghu_apache_queue_thread = NULL;
    ap_log_error(
        APLOG_MARK, APLOG_WARNING, 0, server,
        "Laghu queue maintenance unavailable; queue work may be disabled");
  }
  laghu_operational_registry_init(&laghu_apache_operational);
  apr_pool_cleanup_register(pool, NULL, laghu_apache_rum_cleanup,
                            apr_pool_cleanup_null);
  if (laghu_apache_operational_enabled &&
      !laghu_apache_open_operational((uint64_t)apr_time_sec(apr_time_now())))
    ap_log_error(
        APLOG_MARK, APLOG_WARNING, 0, server,
        "Laghu operational registry unavailable; observability disabled");
  if (config != NULL && config->core.mode == LAGHU_MODE_ON &&
      !laghu_cache_backend_register_path(config->service.image_cache[0] != '\0'
                                             ? config->service.image_cache
                                             : LAGHU_APACHE_LIFECYCLE_CACHE,
                                         &config->service.cache_limits))
    ap_log_error(APLOG_MARK, APLOG_WARNING, 0, server,
                 "Laghu file cache backend unavailable; serving origin");
  length = config != NULL && config->service.rum_snapshot_path[0] != '\0'
               ? snprintf(snapshot, sizeof(snapshot), "%s",
                          config->service.rum_snapshot_path)
               : snprintf(snapshot, sizeof(snapshot), "%s/rum.snapshot",
                          LAGHU_APACHE_LIFECYCLE_CACHE);
  if (length <= 0 || (size_t)length >= sizeof(snapshot)) return;
  options.snapshot_path = snapshot;
  if (config != NULL) {
    options.store_uri = config->service.rum_store;
    options.client_library = config->service.rum_client_library[0] == '\0'
                                 ? NULL
                                 : config->service.rum_client_library;
    options.memory_limit = config->service.rum_memory_limit;
    options.pending_limit = config->service.rum_pending_limit;
    options.ttl_seconds = config->service.rum_ttl;
    options.sync_interval_seconds = config->service.rum_sync_interval;
    options.timeout_ms = config->service.rum_timeout_ms;
    options.retry_limit = config->service.rum_retry_limit;
    options.required = config->service.rum_store_required;
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
}
