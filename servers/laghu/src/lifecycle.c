// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "server_internal.h"

volatile sig_atomic_t proxy_stop_requests;
volatile sig_atomic_t proxy_reload_requests;

static laghu_socket proxy_listen(const char *host, const char *port) {
  struct addrinfo hints, *addresses = NULL, *address;
  laghu_socket listener = LAGHU_INVALID_SOCKET;
  int enabled = 1;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  if (getaddrinfo(host, port, &hints, &addresses) != 0) return listener;
  for (address = addresses; address != NULL; address = address->ai_next) {
    listener = (laghu_socket)socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (listener == LAGHU_INVALID_SOCKET) continue;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&enabled, sizeof(enabled));
    if (bind(listener, address->ai_addr, (laghu_socklen)address->ai_addrlen) == 0 && listen(listener, SOMAXCONN) == 0) break;
    laghu_close(listener);
    listener = LAGHU_INVALID_SOCKET;
  }
  freeaddrinfo(addresses);
  return listener;
}

uint64_t proxy_monotonic_ms(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0U;
  return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static void proxy_pause_ms(unsigned int milliseconds) {
  struct timespec value = {(time_t)(milliseconds / 1000U), (long)(milliseconds % 1000U) * 1000000L};
  (void)nanosleep(&value, NULL);
}

/* Health probes can wait for a bounded network timeout.  They are lifecycle
 * maintenance, never work performed by the listener or a request worker. */
static void *proxy_health_worker_main(void *argument) {
  proxy_queue *queue = argument;
  for (;;) {
    bool stopping;
    proxy_queue_lock(queue);
    stopping = queue->stopping;
    proxy_queue_unlock(queue);
    if (stopping) break;
    proxy_maintain_upstream_health(queue);
    proxy_pause_ms(100U);
  }
  return NULL;
}

bool proxy_cache_probe(const char *cache_path) {
  char path[LAGHU_RUNTIME_PATH_SIZE];
  int count;
  int file;
  unsigned char marker = 0x4cU;
  count = snprintf(path, sizeof(path), "%s/.laghu-ready-%ld-%llu-%llu", cache_path, (long)getpid(), (unsigned long long)(uintptr_t)pthread_self(),
                   (unsigned long long)proxy_monotonic_ms());
  if (count <= 0 || (size_t)count >= sizeof(path)) return false;
  file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (file < 0) return false;
  if (write(file, &marker, 1U) != 1 || fsync(file) != 0) {
    close(file);
    (void)unlink(path);
    return false;
  }
  if (close(file) != 0) {
    (void)unlink(path);
    return false;
  }
  return unlink(path) == 0;
}

static bool proxy_queue_path_valid(const char *path) {
  laghu_runtime_queue queue;
  bool exists;
  bool valid = false;
  unsigned int attempt;
  exists = access(path, F_OK) == 0;
  if (!exists && errno != ENOENT) return false;
  if (!exists) {
    char parent[LAGHU_RUNTIME_PATH_SIZE];
    const char *separator = strrchr(path, '/');
    if (separator == NULL) return true;
    if (separator == path) {
      parent[0] = *separator;
      parent[1] = '\0';
    } else {
      size_t length = (size_t)(separator - path);
      if (length >= sizeof(parent)) return false;
      memcpy(parent, path, length);
      parent[length] = '\0';
    }
    return access(parent, R_OK | X_OK) == 0;
  }
  for (attempt = 0U; attempt < 10U; ++attempt) {
    laghu_runtime_queue_init(&queue);
    valid = laghu_runtime_queue_open(&queue, path);
    laghu_runtime_queue_close(&queue);
    if (valid) break;
    proxy_pause_ms(10U);
  }
  return valid;
}

static bool proxy_attach_queue(proxy_queue *queue, laghu_runtime_queue *runtime, bool *ready, const char *path) {
  laghu_runtime_queue candidate;
  bool attached;
  if (path == NULL || path[0] == '\0') return true;
  laghu_runtime_queue_init(&candidate);
  proxy_queue_lock(queue);
  attached = *ready;
  proxy_queue_unlock(queue);
  if (attached) return true;
  if (!laghu_runtime_queue_open(&candidate, path)) return false;
  proxy_queue_lock(queue);
  attached = *ready;
  if (!attached) {
    attached = laghu_runtime_queue_move(runtime, &candidate);
    if (attached) *ready = true;
  }
  proxy_queue_unlock(queue);
  laghu_runtime_queue_close(&candidate);
  return attached;
}

void proxy_maintain_queue_attachments(proxy_queue *queue) {
  const laghu_proxy_options *options;
  const laghu_service_config *service;
  if (queue == NULL) return;
  options = proxy_options_acquire(queue);
  service = &options->service;
  if (queue->image_queue_required) (void)proxy_attach_queue(queue, &queue->runtime_queue, &queue->runtime_queue_ready, service->worker_queue);
  if (service->html_refresh_queue[0] != '\0')
    (void)proxy_attach_queue(queue, &queue->html_refresh_queue, &queue->html_refresh_queue_ready, service->html_refresh_queue);
  if (service->font_providers != NULL)
    (void)proxy_attach_queue(queue, &queue->font_fetch_queue, &queue->font_fetch_queue_ready, service->font_fetch_queue);
  if (service->javascript_queue[0] != '\0')
    (void)proxy_attach_queue(queue, &queue->javascript_queue, &queue->javascript_queue_ready, service->javascript_queue);
  if (service->chrome_analysis_queue[0] != '\0')
    (void)proxy_attach_queue(queue, &queue->chrome_analysis_queue, &queue->chrome_analysis_queue_ready, service->chrome_analysis_queue);
  if (service->otel_trace_queue[0] != '\0')
    (void)proxy_attach_queue(queue, &queue->otel_trace_queue, &queue->otel_trace_queue_ready, service->otel_trace_queue);
  proxy_options_release(queue);
}

static void proxy_signal_handler(int signal_number) {
  if (signal_number == SIGHUP) {
    if (proxy_reload_requests < 2) ++proxy_reload_requests;
    return;
  }
  if (proxy_stop_requests < 2) ++proxy_stop_requests;
}

static bool proxy_listener_ready(laghu_socket listener) {
  struct pollfd readable;
  readable.fd = listener;
  readable.events = POLLIN;
  readable.revents = 0;
  return poll(&readable, 1U, 200) > 0 && (readable.revents & POLLIN) != 0;
}

static void proxy_begin_drain(proxy_queue *queue) {
  proxy_queue_lock(queue);
  queue->state = PROXY_DRAINING;
  queue->stopping = true;
  pthread_cond_broadcast(&queue->ready);
  proxy_queue_unlock(queue);
  for (;;) {
    laghu_socket client;
    proxy_queue_lock(queue);
    if (queue->count == 0U) {
      proxy_queue_unlock(queue);
      break;
    }
    client = queue->items[queue->head].socket;
    queue->head = (queue->head + 1U) % queue->capacity;
    --queue->count;
    proxy_queue_unlock(queue);
    proxy_reject_connection(queue, client, "shutdown");
  }
}

static unsigned int proxy_active_count(proxy_queue *queue) {
  unsigned int count;
  proxy_queue_lock(queue);
  count = queue->active_count;
  proxy_queue_unlock(queue);
  return count;
}

static void proxy_force_workers(proxy_queue *queue, proxy_worker *workers, unsigned int worker_count) {
  unsigned int index;
  proxy_queue_lock(queue);
  queue->state = PROXY_FORCING;
  for (index = 0U; index < worker_count; ++index) {
    if (workers[index].active_client != LAGHU_INVALID_SOCKET) (void)shutdown(workers[index].active_client, LAGHU_SHUT_BOTH);
    if (workers[index].active_origin != LAGHU_INVALID_SOCKET) (void)shutdown(workers[index].active_origin, LAGHU_SHUT_BOTH);
  }
  proxy_queue_unlock(queue);
}

typedef enum { PROXY_RELOAD_REQUEST_NONE = 0, PROXY_RELOAD_REQUEST_READY, PROXY_RELOAD_REQUEST_INVALID } proxy_reload_request;

static bool proxy_reload_request_names(const char *pid_path, char *request_name, size_t request_capacity, char *processing_name,
                                       size_t processing_capacity) {
  const char *base;
  int written;
  if (pid_path == NULL || pid_path[0] != '/' || request_name == NULL || processing_name == NULL) return false;
  base = strrchr(pid_path, '/');
  if (base == NULL || base[1] == '\0') return false;
  ++base;
  written = snprintf(request_name, request_capacity, "%s.reload", base);
  if (written <= 0 || (size_t)written >= request_capacity) return false;
  written = snprintf(processing_name, processing_capacity, "%s.reload.processing", base);
  return written > 0 && (size_t)written < processing_capacity;
}

static int proxy_reload_directory_open(const char *pid_path) {
  const char *separator;
  char directory[LAGHU_RUNTIME_PATH_SIZE];
  struct stat listed;
  struct stat opened;
  size_t length;
  int file;
  if (pid_path == NULL || pid_path[0] != '/') return -1;
  separator = strrchr(pid_path, '/');
  if (separator == NULL || separator == pid_path + 1U) return -1;
  length = separator == pid_path ? 1U : (size_t)(separator - pid_path);
  if (length >= sizeof(directory)) return -1;
  memcpy(directory, pid_path, length);
  directory[length] = '\0';
  if (lstat(directory, &listed) != 0 || !S_ISDIR(listed.st_mode) || listed.st_uid != geteuid() || (listed.st_mode & (S_IWGRP | S_IWOTH)) != 0U)
    return -1;
  file = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (file < 0) return -1;
  if (fstat(file, &opened) != 0 || !S_ISDIR(opened.st_mode) || opened.st_uid != geteuid() || (opened.st_mode & (S_IWGRP | S_IWOTH)) != 0U ||
      opened.st_dev != listed.st_dev || opened.st_ino != listed.st_ino) {
    (void)close(file);
    return -1;
  }
  return file;
}

static bool proxy_reload_write(int file, const void *input, size_t length) {
  const unsigned char *bytes = input;
  while (length != 0U) {
    ssize_t written = write(file, bytes, length);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    bytes += (size_t)written;
    length -= (size_t)written;
  }
  return true;
}

static void proxy_reload_request_discard(const char *pid_path) {
  char request_name[LAGHU_RUNTIME_PATH_SIZE + 8U];
  char processing_name[LAGHU_RUNTIME_PATH_SIZE + 32U];
  int directory = proxy_reload_directory_open(pid_path);
  if (directory < 0 || !proxy_reload_request_names(pid_path, request_name, sizeof(request_name), processing_name, sizeof(processing_name))) {
    if (directory >= 0) (void)close(directory);
    return;
  }
  (void)unlinkat(directory, request_name, 0);
  (void)unlinkat(directory, processing_name, 0);
  (void)fsync(directory);
  (void)close(directory);
}

/* The command process only publishes a bounded path. The daemon still opens
 * and validates that candidate itself, so a request cannot publish caller
 * memory or make request workers touch the filesystem. */
static bool proxy_reload_request_publish(const char *pid_path, const char *config_path, char *error, size_t error_size) {
  char request_name[LAGHU_RUNTIME_PATH_SIZE + 8U];
  char processing_name[LAGHU_RUNTIME_PATH_SIZE + 32U];
  char temporary_name[LAGHU_RUNTIME_PATH_SIZE + 64U] = {0};
  size_t config_length;
  unsigned int attempt;
  int directory;
  int file = -1;
  if (error_size != 0U) error[0] = '\0';
  if (config_path == NULL || config_path[0] != '/' || (config_length = strlen(config_path)) + 1U > LAGHU_RUNTIME_PATH_SIZE ||
      !proxy_reload_request_names(pid_path, request_name, sizeof(request_name), processing_name, sizeof(processing_name)))
    goto invalid;
  directory = proxy_reload_directory_open(pid_path);
  if (directory < 0) goto invalid;
  for (attempt = 0U; attempt != 32U; ++attempt) {
    int written = snprintf(temporary_name, sizeof(temporary_name), "%s-%ld-%u", request_name, (long)getpid(), attempt);
    if (written <= 0 || (size_t)written >= sizeof(temporary_name)) break;
    file = openat(directory, temporary_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (file >= 0) break;
    if (errno != EEXIST) break;
  }
  if (file < 0 || !proxy_reload_write(file, config_path, config_length + 1U) || fsync(file) != 0) {
    if (file >= 0) (void)close(file);
    (void)unlinkat(directory, temporary_name, 0);
    (void)close(directory);
    goto invalid;
  }
  if (close(file) != 0 || renameat(directory, temporary_name, directory, request_name) != 0) {
    (void)unlinkat(directory, temporary_name, 0);
    (void)close(directory);
    goto invalid;
  }
  (void)fsync(directory);
  (void)close(directory);
  return true;
invalid:
  if (error_size != 0U) (void)snprintf(error, error_size, "unable to publish reload request");
  return false;
}

static proxy_reload_request proxy_reload_request_consume(proxy_queue *queue, char *config_path, size_t config_capacity) {
  char request_name[LAGHU_RUNTIME_PATH_SIZE + 8U];
  char processing_name[LAGHU_RUNTIME_PATH_SIZE + 32U];
  char pid_path[LAGHU_RUNTIME_PATH_SIZE];
  const laghu_proxy_options *options;
  struct stat status;
  size_t used = 0U;
  int directory;
  int file;
  bool valid = false;
  if (queue == NULL || config_path == NULL || config_capacity == 0U) return PROXY_RELOAD_REQUEST_NONE;
  options = proxy_options_acquire(queue);
  if (options == NULL || snprintf(pid_path, sizeof(pid_path), "%s", options->pid_file) <= 0) {
    proxy_options_release(queue);
    return PROXY_RELOAD_REQUEST_NONE;
  }
  proxy_options_release(queue);
  if (!proxy_reload_request_names(pid_path, request_name, sizeof(request_name), processing_name, sizeof(processing_name)))
    return PROXY_RELOAD_REQUEST_NONE;
  directory = proxy_reload_directory_open(pid_path);
  if (directory < 0) return PROXY_RELOAD_REQUEST_INVALID;
  if (renameat(directory, request_name, directory, processing_name) != 0) {
    (void)close(directory);
    return errno == ENOENT ? PROXY_RELOAD_REQUEST_NONE : PROXY_RELOAD_REQUEST_INVALID;
  }
  file = openat(directory, processing_name, O_RDONLY | O_NOFOLLOW);
  if (file >= 0 && fstat(file, &status) == 0 && S_ISREG(status.st_mode) && status.st_uid == geteuid() &&
      (status.st_mode & (S_IRWXG | S_IRWXO)) == 0U) {
    for (;;) {
      ssize_t length;
      if (used == config_capacity) break;
      length = read(file, config_path + used, config_capacity - used);
      if (length < 0 && errno == EINTR) continue;
      if (length <= 0) break;
      used += (size_t)length;
    }
    valid = used != 0U && used < config_capacity && config_path[used - 1U] == '\0' && memchr(config_path, '\0', used) == config_path + used - 1U &&
            config_path[0] == '/';
  }
  if (file >= 0 && close(file) != 0) valid = false;
  (void)unlinkat(directory, processing_name, 0);
  (void)fsync(directory);
  (void)close(directory);
  return valid ? PROXY_RELOAD_REQUEST_READY : PROXY_RELOAD_REQUEST_INVALID;
}

static bool proxy_reload_string_equal(const char *left, const char *right) { return strcmp(left, right) == 0; }

/* Queue mappings, cache registrations, the RUM engine, and TLS contexts are
 * lifecycle-owned.  A reload swaps only a fully validated immutable request
 * snapshot while those ownership boundaries stay fixed. */
static bool proxy_reload_service_compatible(const laghu_service_config *left, const laghu_service_config *right) {
  return left->present == right->present && !memcmp(&left->cache_limits, &right->cache_limits, sizeof(left->cache_limits)) &&
         left->purge_allow_count == right->purge_allow_count && left->trusted_proxy_count == right->trusted_proxy_count &&
         !memcmp(left->purge_allow, right->purge_allow, sizeof(left->purge_allow)) &&
         !memcmp(left->trusted_proxies, right->trusted_proxies, sizeof(left->trusted_proxies)) &&
         proxy_reload_string_equal(left->file_cache_backend, right->file_cache_backend) &&
         proxy_reload_string_equal(left->image_cache, right->image_cache) && proxy_reload_string_equal(left->worker_queue, right->worker_queue) &&
         proxy_reload_string_equal(left->html_refresh_queue, right->html_refresh_queue) &&
         proxy_reload_string_equal(left->chrome_analysis_queue, right->chrome_analysis_queue) &&
         proxy_reload_string_equal(left->chrome_analysis_output, right->chrome_analysis_output) &&
         proxy_reload_string_equal(left->font_fetch_queue, right->font_fetch_queue) &&
         proxy_reload_string_equal(left->font_provider_config, right->font_provider_config) &&
         proxy_reload_string_equal(left->javascript_queue, right->javascript_queue) &&
         proxy_reload_string_equal(left->javascript_target, right->javascript_target) &&
         proxy_reload_string_equal(left->javascript_observation_config, right->javascript_observation_config) &&
         proxy_reload_string_equal(left->javascript_defer_config, right->javascript_defer_config) &&
         proxy_reload_string_equal(left->layout_reservation_config, right->layout_reservation_config) &&
         proxy_reload_string_equal(left->otel_endpoint, right->otel_endpoint) &&
         proxy_reload_string_equal(left->otel_trace_queue, right->otel_trace_queue) &&
         proxy_reload_string_equal(left->otel_ca_file, right->otel_ca_file) &&
         proxy_reload_string_equal(left->asset_offload_config, right->asset_offload_config) &&
         proxy_reload_string_equal(left->asset_upload_queue, right->asset_upload_queue) &&
         proxy_reload_string_equal(left->rum_store, right->rum_store) &&
         proxy_reload_string_equal(left->rum_snapshot_path, right->rum_snapshot_path) &&
         proxy_reload_string_equal(left->rum_client_library, right->rum_client_library) &&
         proxy_reload_string_equal(left->purge_token_file, right->purge_token_file) &&
         proxy_reload_string_equal(left->cache_flush_file, right->cache_flush_file) &&
         !memcmp(&left->source_policy, &right->source_policy, sizeof(left->source_policy)) && left->rum_timeout_ms == right->rum_timeout_ms &&
         left->otel_sampling_rate == right->otel_sampling_rate && left->chrome_analysis_timeout_ms == right->chrome_analysis_timeout_ms &&
         left->rum_ttl == right->rum_ttl && left->rum_retry_limit == right->rum_retry_limit && left->rum_sync_interval == right->rum_sync_interval &&
         left->rum_memory_limit == right->rum_memory_limit && left->rum_pending_limit == right->rum_pending_limit &&
         left->purge_method == right->purge_method && left->purge_query == right->purge_query && left->statistics == right->statistics &&
         left->metrics == right->metrics && left->readiness == right->readiness && left->readiness_strict == right->readiness_strict &&
         left->rum_store_required == right->rum_store_required;
}

static bool proxy_reload_compatible(const laghu_proxy_options *active, const laghu_proxy_options *candidate) {
  proxy_lifecycle_requirements active_requirements;
  proxy_lifecycle_requirements candidate_requirements;
  size_t index;
  if (!proxy_options_lifecycle_requirements(active, &active_requirements) ||
      !proxy_options_lifecycle_requirements(candidate, &candidate_requirements) ||
      memcmp(&active_requirements, &candidate_requirements, sizeof(active_requirements)) != 0)
    return false;
  if (!proxy_reload_string_equal(active->listen_host, candidate->listen_host) ||
      !proxy_reload_string_equal(active->listen_port, candidate->listen_port) || active->workers != candidate->workers ||
      active->connection_queue != candidate->connection_queue || active->origin_pool_size != candidate->origin_pool_size ||
      active->origin_tls != candidate->origin_tls || active->downstream_tls != candidate->downstream_tls ||
      !proxy_reload_string_equal(active->origin_ca_file, candidate->origin_ca_file) ||
      !proxy_reload_string_equal(active->tls_certificate, candidate->tls_certificate) ||
      !proxy_reload_string_equal(active->tls_private_key, candidate->tls_private_key) ||
      !proxy_reload_string_equal(active->pid_file, candidate->pid_file) || !proxy_reload_service_compatible(&active->service, &candidate->service))
    return false;
  if (active->site_count != candidate->site_count) return false;
  for (index = 0U; index < active->site_count; ++index) {
    if (!proxy_reload_string_equal(active->sites[index].host, candidate->sites[index].host) ||
        !proxy_reload_string_equal(active->sites[index].tls_certificate, candidate->sites[index].tls_certificate) ||
        !proxy_reload_string_equal(active->sites[index].tls_private_key, candidate->sites[index].tls_private_key) ||
        !proxy_reload_service_compatible(&active->sites[index].service, &candidate->sites[index].service))
      return false;
  }
  if (active->route_count != candidate->route_count) return false;
  for (index = 0U; index < active->route_count; ++index)
    if (!proxy_reload_service_compatible(&active->routes[index].service, &candidate->routes[index].service)) return false;
  return true;
}

static void proxy_reload_snapshot_dispose(proxy_reload_snapshot *snapshot) {
  if (snapshot == NULL) return;
  laghu_proxy_options_dispose(&snapshot->options);
  free(snapshot);
}

static void proxy_reload_generation_dispose(proxy_queue *queue) {
  proxy_static_roots_dispose(queue->static_roots);
  queue->static_roots = NULL;
  proxy_reload_snapshot_dispose(queue->current_snapshot);
  queue->current_snapshot = NULL;
}

static void proxy_reload_configuration(proxy_queue *queue) {
  proxy_reload_snapshot *snapshot;
  proxy_static_roots *roots;
  char requested_path[LAGHU_RUNTIME_PATH_SIZE];
  char error[256] = {0};
  const laghu_proxy_options *active;
  proxy_reload_snapshot *previous_snapshot;
  proxy_static_roots *previous_roots;
  const char *config_path;
  proxy_reload_request request;
  if (queue->config_path[0] == '\0') {
    proxy_log_event(queue, "reload", "retained");
    return;
  }
  request = proxy_reload_request_consume(queue, requested_path, sizeof(requested_path));
  if (request == PROXY_RELOAD_REQUEST_INVALID) {
    proxy_log_event(queue, "reload", "retained");
    return;
  }
  config_path = request == PROXY_RELOAD_REQUEST_READY ? requested_path : queue->config_path;
  snapshot = calloc(1U, sizeof(*snapshot));
  if (snapshot == NULL) {
    proxy_log_event(queue, "reload", "retained");
    return;
  }
  laghu_proxy_options_init(&snapshot->options);
  if (laghu_proxy_load_yaml(config_path, &snapshot->options, error, sizeof(error)) != LAGHU_PROXY_PARSE_OK) {
    laghu_proxy_options_dispose(&snapshot->options);
    free(snapshot);
    proxy_log_event(queue, "reload", "retained");
    return;
  }
  active = proxy_options_acquire(queue);
  if (!proxy_reload_compatible(active, &snapshot->options)) {
    proxy_options_release(queue);
    laghu_proxy_options_dispose(&snapshot->options);
    free(snapshot);
    proxy_log_event(queue, "reload", "retained");
    return;
  }
  proxy_options_release(queue);
  roots = proxy_static_roots_create(&snapshot->options);
  if (roots == NULL) {
    laghu_proxy_options_dispose(&snapshot->options);
    free(snapshot);
    proxy_log_event(queue, "reload", "retained");
    return;
  }
  /* Every request, worker-side helper, health probe, listener helper, TLS SNI
   * callback, and log decision borrows options through this boundary. New
   * borrows stop before the current readers quiesce; no queue mutex is held
   * while waiting, so reload cannot invert the normal options-then-queue order. */
  (void)pthread_mutex_lock(&queue->options_lock);
  queue->options_writer_pending = true;
  while (queue->options_readers != 0U) (void)pthread_cond_wait(&queue->options_quiescent, &queue->options_lock);
  previous_snapshot = queue->current_snapshot;
  previous_roots = queue->static_roots;
  queue->static_roots = roots;
  queue->current_snapshot = snapshot;
  queue->options = &snapshot->options;
  if (request == PROXY_RELOAD_REQUEST_READY) (void)snprintf(queue->config_path, sizeof(queue->config_path), "%s", config_path);
  queue->options_writer_pending = false;
  (void)pthread_cond_broadcast(&queue->options_quiescent);
  (void)pthread_mutex_unlock(&queue->options_lock);
  proxy_static_roots_dispose(previous_roots);
  proxy_reload_snapshot_dispose(previous_snapshot);
  proxy_queue_lock(queue);
  memset(queue->upstream_health, 0, sizeof(queue->upstream_health));
  proxy_queue_unlock(queue);
  proxy_log_event(queue, "reload", "applied");
}

static bool proxy_pid_create(const char *path) {
  char value[32U];
  int file;
  int length;
  if (path == NULL || path[0] == '\0') return true;
  file = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
  if (file < 0) return false;
  length = snprintf(value, sizeof(value), "%ld\n", (long)getpid());
  if (length <= 0 || (size_t)length >= sizeof(value) || write(file, value, (size_t)length) != length || fsync(file) != 0 || close(file) != 0) {
    (void)close(file);
    (void)unlink(path);
    return false;
  }
  return true;
}

int laghu_proxy_run_with_config(const laghu_proxy_options *options, const char *config_path) {
  proxy_queue queue;
  proxy_lifecycle_requirements requirements;
  proxy_worker *workers = NULL;
  struct stat pid_status;
  laghu_socket listener = LAGHU_INVALID_SOCKET;
  unsigned int index, started = 0U;
  int result = 1;
  bool lock_ready = false, options_lock_ready = false, options_condition_ready = false, pid_created = false;
  bool health_started = false, health_failed = false;
  if (options == NULL || !proxy_options_lifecycle_requirements(options, &requirements)) return 1;
  bool ready_condition = false;
  proxy_static_roots *roots;
  pthread_t *threads = NULL;
  pthread_t health_thread;
  memset(&queue, 0, sizeof(queue));
  laghu_operational_registry_init(&queue.operational);
  laghu_runtime_queue_init(&queue.runtime_queue);
  laghu_runtime_queue_init(&queue.html_refresh_queue);
  laghu_runtime_queue_init(&queue.font_fetch_queue);
  laghu_runtime_queue_init(&queue.javascript_queue);
  laghu_runtime_queue_init(&queue.chrome_analysis_queue);
  laghu_runtime_queue_init(&queue.otel_trace_queue);
  queue.options = options;
  roots = proxy_static_roots_create(options);
  if (roots == NULL) goto cleanup;
  queue.static_roots = roots;
  queue.image_queue_required = requirements.image_queue;
  if (config_path != NULL && strlen(config_path) < sizeof(queue.config_path))
    (void)snprintf(queue.config_path, sizeof(queue.config_path), "%s", config_path);
  if (options->pid_file[0] != '\0') {
    char request_name[LAGHU_RUNTIME_PATH_SIZE + 8U];
    char processing_name[LAGHU_RUNTIME_PATH_SIZE + 32U];
    if (!proxy_reload_request_names(options->pid_file, request_name, sizeof(request_name), processing_name, sizeof(processing_name))) goto cleanup;
    if (lstat(options->pid_file, &pid_status) != 0 && errno == ENOENT) proxy_reload_request_discard(options->pid_file);
  }
  queue.capacity = options->connection_queue;
  queue.items = calloc(queue.capacity, sizeof(*queue.items));
  if (options->origin_pool_size != 0U) queue.origins = calloc(options->origin_pool_size, sizeof(*queue.origins));
  workers = calloc(options->workers, sizeof(*workers));
  threads = calloc(options->workers, sizeof(*threads));
  if (queue.items == NULL || (options->origin_pool_size != 0U && queue.origins == NULL) || workers == NULL || threads == NULL) goto cleanup;
  if (pthread_mutex_init(&queue.lock, NULL) != 0) goto cleanup;
  lock_ready = true;
  if (pthread_mutex_init(&queue.options_lock, NULL) != 0) goto cleanup;
  options_lock_ready = true;
  if (pthread_cond_init(&queue.options_quiescent, NULL) != 0) goto cleanup;
  options_condition_ready = true;
  if (pthread_cond_init(&queue.ready, NULL) != 0) goto cleanup;
  ready_condition = true;
  queue.state = PROXY_STARTING;
  queue.cache_readiness = -1;
  queue.optimizer_readiness = -1;
  queue.request_prefix = proxy_monotonic_ms() << 16U;
  if (requirements.rum) {
    laghu_rum_options rum_options;
    char snapshot[LAGHU_RUNTIME_PATH_SIZE];
    char rum_error[160U];
    laghu_rum_options_init(&rum_options);
    if (options->service.rum_snapshot_path[0] != '\0')
      (void)snprintf(snapshot, sizeof(snapshot), "%s", options->service.rum_snapshot_path);
    else if (snprintf(snapshot, sizeof(snapshot), "%s/rum.snapshot", options->service.image_cache) <= 0)
      goto cleanup;
    rum_options.store_uri = options->service.rum_store;
    rum_options.snapshot_path = snapshot;
    rum_options.client_library = options->service.rum_client_library[0] == '\0' ? NULL : options->service.rum_client_library;
    rum_options.memory_limit = options->service.rum_memory_limit;
    rum_options.pending_limit = options->service.rum_pending_limit;
    rum_options.ttl_seconds = options->service.rum_ttl;
    rum_options.sync_interval_seconds = options->service.rum_sync_interval;
    rum_options.timeout_ms = options->service.rum_timeout_ms;
    rum_options.retry_limit = options->service.rum_retry_limit;
    rum_options.required = options->service.rum_store_required;
    queue.rum = laghu_rum_engine_create(&rum_options, rum_error, sizeof(rum_error));
    if (queue.rum == NULL && options->service.rum_store_required) goto cleanup;
    if (queue.rum == NULL) {
      proxy_log_event(&queue, "rum_store_fallback", "degraded");
      rum_options.store_uri = "local:";
      rum_options.client_library = NULL;
      rum_options.required = false;
      queue.rum = laghu_rum_engine_create(&rum_options, rum_error, sizeof(rum_error));
    }
    if (queue.rum == NULL) goto cleanup;
  }
  if (requirements.cache && !proxy_cache_probe(options->service.image_cache)) {
    proxy_log_startup_failure(&queue, "cache_unavailable");
    goto cleanup;
  }
  if ((requirements.image_queue && !proxy_queue_path_valid(options->service.worker_queue)) ||
      (options->service.html_refresh_queue[0] != '\0' && !proxy_queue_path_valid(options->service.html_refresh_queue)) ||
      (options->service.font_providers != NULL && !proxy_queue_path_valid(options->service.font_fetch_queue)) ||
      (options->service.javascript_queue[0] != '\0' && !proxy_queue_path_valid(options->service.javascript_queue)) ||
      (options->service.chrome_analysis_queue[0] != '\0' && !proxy_queue_path_valid(options->service.chrome_analysis_queue)) ||
      (options->service.otel_trace_queue[0] != '\0' && !proxy_queue_path_valid(options->service.otel_trace_queue))) {
    proxy_log_startup_failure(&queue, "queue_unavailable");
    goto cleanup;
  }
  /* Attachment is lifecycle maintenance.  Request workers only receive an
   * already-mapped queue and therefore never open persisted state. */
  proxy_maintain_queue_attachments(&queue);
  if (options->service.source_policy.mode != LAGHU_SOURCE_FILE_OFF &&
      !laghu_source_registry_publish(options->service.asset_upload_queue, &options->service.source_policy)) {
    proxy_log_startup_failure(&queue, "source_registry");
    goto cleanup;
  }
  if (requirements.cache && !laghu_cache_backend_register_path(options->service.image_cache, &options->service.cache_limits)) {
    proxy_log_startup_failure(&queue, "cache_backend");
    goto cleanup;
  }
  if (requirements.operational &&
      !laghu_operational_registry_open(&queue.operational, options->service.image_cache, LAGHU_OPERATIONAL_SURFACE_STANDALONE,
                                       LAGHU_OPERATIONAL_PROCESS_ADAPTER, true, (uint64_t)time(NULL))) {
    proxy_log_event(&queue, "observability", "unavailable");
  }
  if (proxy_options_has_tls_upstream(options) && (queue.tls_context = proxy_tls_context(options)) == NULL) {
    proxy_log_startup_failure(&queue, "origin_tls");
    goto cleanup;
  }
  if (options->downstream_tls && (queue.downstream_tls_context = proxy_downstream_tls_context(&queue, options)) == NULL) {
    proxy_log_startup_failure(&queue, "downstream_tls");
    goto cleanup;
  }
  queue.downstream_tls_options = options;
  listener = proxy_listen(options->listen_host, options->listen_port);
  if (listener == LAGHU_INVALID_SOCKET) {
    proxy_log_startup_failure(&queue, "listen");
    goto cleanup;
  }
  if (!proxy_pid_create(options->pid_file)) {
    proxy_log_startup_failure(&queue, "pid_file");
    goto cleanup;
  }
  pid_created = options->pid_file[0] != '\0';
  proxy_stop_requests = 0;
  proxy_reload_requests = 0;
  {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = proxy_signal_handler;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGHUP, &action, NULL);
    action.sa_handler = SIG_IGN;
    (void)sigaction(SIGPIPE, &action, NULL);
  }
  for (index = 0U; index < options->workers; ++index) {
    workers[index].queue = &queue;
    workers[index].active_client = LAGHU_INVALID_SOCKET;
    workers[index].active_origin = LAGHU_INVALID_SOCKET;
    if (pthread_create(&threads[index], NULL, proxy_worker_main, &workers[index]) != 0) break;
    ++started;
  }
  if (started != options->workers) {
    proxy_log_event(&queue, "startup_failure", "forcing");
    proxy_stop_requests = 2;
  } else {
    queue.state = PROXY_RUNNING;
    if (pthread_create(&health_thread, NULL, proxy_health_worker_main, &queue) != 0) {
      proxy_log_startup_failure(&queue, "health_worker");
      health_failed = true;
      proxy_stop_requests = 2;
    } else {
      health_started = true;
      proxy_log_event(&queue, "startup", "running");
      while (proxy_stop_requests == 0) {
        if (proxy_reload_requests != 0) {
          proxy_reload_requests = 0;
          proxy_reload_configuration(&queue);
        }
        proxy_maintain_queue_attachments(&queue);
        {
          const laghu_proxy_options *current = proxy_options_acquire(&queue);
          (void)laghu_runtime_import_chrome_analysis(queue.rum, current->service.chrome_analysis_output, (uint64_t)time(NULL),
                                                     current->service.rum_ttl);
          proxy_options_release(&queue);
        }
        if (proxy_listener_ready(listener)) {
          proxy_connection connection;
          memset(&connection, 0, sizeof(connection));
          connection.peer_length = (laghu_socklen)sizeof(connection.peer);
          connection.socket = accept(listener, (struct sockaddr *)&connection.peer, &connection.peer_length);
          if (connection.socket != LAGHU_INVALID_SOCKET && !queue_push(&queue, &connection))
            proxy_reject_connection(&queue, connection.socket, "queue_saturated");
        }
      }
    }
  }
  laghu_close(listener);
  listener = LAGHU_INVALID_SOCKET;
  proxy_begin_drain(&queue);
  if (health_started) (void)pthread_join(health_thread, NULL);
  proxy_log_event(&queue, "shutdown", "draining");
  {
    uint64_t deadline = proxy_monotonic_ms() + (uint64_t)options->drain_timeout * 1000U;
    while (proxy_active_count(&queue) != 0U && proxy_stop_requests < 2 && proxy_monotonic_ms() < deadline) proxy_pause_ms(20U);
  }
  if (proxy_active_count(&queue) != 0U) {
    proxy_force_workers(&queue, workers, started);
    proxy_log_event(&queue, "shutdown", "forcing");
  }
  for (index = 0U; index < started; ++index) {
    (void)pthread_join(threads[index], NULL);
  }
  queue.state = PROXY_STOPPED;
  proxy_log_event(&queue, "shutdown", "stopped");
  result = started == options->workers && !health_failed ? 0 : 1;
cleanup:
  if (lock_ready) proxy_origin_pool_close(&queue);
  laghu_runtime_queue_close(&queue.runtime_queue);
  laghu_runtime_queue_close(&queue.html_refresh_queue);
  laghu_runtime_queue_close(&queue.font_fetch_queue);
  laghu_runtime_queue_close(&queue.javascript_queue);
  laghu_runtime_queue_close(&queue.chrome_analysis_queue);
  laghu_runtime_queue_close(&queue.otel_trace_queue);
  laghu_operational_registry_close(&queue.operational);
  laghu_rum_engine_destroy(queue.rum);
  if (listener != LAGHU_INVALID_SOCKET) laghu_close(listener);
  SSL_CTX_free(queue.tls_context);
  SSL_CTX_free(queue.downstream_tls_context);
  if (pid_created) (void)unlink(options->pid_file);
  proxy_reload_generation_dispose(&queue);
  if (ready_condition) pthread_cond_destroy(&queue.ready);
  if (options_lock_ready) {
    if (options_condition_ready) pthread_cond_destroy(&queue.options_quiescent);
    pthread_mutex_destroy(&queue.options_lock);
  }
  if (lock_ready) pthread_mutex_destroy(&queue.lock);
  free(threads);
  free(workers);
  free(queue.items);
  free(queue.origins);
  return result;
}

int laghu_proxy_run(const laghu_proxy_options *options) { return laghu_proxy_run_with_config(options, NULL); }

int laghu_proxy_reload(const char *config_path, char *error, size_t error_size) {
  laghu_proxy_options *options;
  struct stat status;
  char value[32U];
  char *end = NULL;
  ssize_t length;
  long process;
  int file;
  int result = 2;
  if (error_size != 0U) error[0] = '\0';
  options = calloc(1U, sizeof(*options));
  if (options == NULL) {
    if (error_size != 0U) (void)snprintf(error, error_size, "reload allocation failed");
    return result;
  }
  laghu_proxy_options_init(options);
  if (config_path == NULL || config_path[0] != '/' || laghu_proxy_load_yaml(config_path, options, error, error_size) != LAGHU_PROXY_PARSE_OK ||
      options->pid_file[0] == '\0') {
    if (error_size != 0U && error[0] == '\0') (void)snprintf(error, error_size, "reload requires runtime.pid_file");
    goto done;
  }
  if (lstat(options->pid_file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != getuid() ||
      (status.st_mode & (S_IWGRP | S_IWOTH)) != 0U || (file = open(options->pid_file, O_RDONLY | O_NOFOLLOW)) < 0) {
    if (error_size != 0U) (void)snprintf(error, error_size, "invalid pid file");
    goto done;
  }
  length = read(file, value, sizeof(value) - 1U);
  (void)close(file);
  if (length <= 0 || (size_t)length >= sizeof(value)) {
    if (error_size != 0U) (void)snprintf(error, error_size, "invalid pid file");
    goto done;
  }
  value[length] = '\0';
  errno = 0;
  process = strtol(value, &end, 10);
  if (errno != 0 || end == value || (*end != '\n' && *end != '\0') || process <= 1) {
    if (error_size != 0U) (void)snprintf(error, error_size, "invalid pid file");
    goto done;
  }
  if (!proxy_reload_request_publish(options->pid_file, config_path, error, error_size)) goto done;
  if (kill((pid_t)process, SIGHUP) != 0) {
    proxy_reload_request_discard(options->pid_file);
    if (error_size != 0U) (void)snprintf(error, error_size, "unable to signal running laghu");
    goto done;
  }
  result = 0;
done:
  laghu_proxy_options_dispose(options);
  free(options);
  return result;
}
