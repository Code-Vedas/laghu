// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "server_internal.h"

#ifdef _WIN32
volatile LONG proxy_stop_requests;
#else
volatile sig_atomic_t proxy_stop_requests;
#endif

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
    listener = (laghu_socket)socket(address->ai_family, address->ai_socktype,
                                    address->ai_protocol);
    if (listener == LAGHU_INVALID_SOCKET) continue;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&enabled,
                     sizeof(enabled));
    if (bind(listener, address->ai_addr, (laghu_socklen)address->ai_addrlen) ==
            0 &&
        listen(listener, SOMAXCONN) == 0)
      break;
    laghu_close(listener);
    listener = LAGHU_INVALID_SOCKET;
  }
  freeaddrinfo(addresses);
  return listener;
}

uint64_t proxy_monotonic_ms(void) {
#ifdef _WIN32
  return (uint64_t)GetTickCount64();
#else
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0U;
  return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
#endif
}

static void proxy_pause_ms(unsigned int milliseconds) {
#ifdef _WIN32
  Sleep(milliseconds);
#else
  struct timespec value = {(time_t)(milliseconds / 1000U),
                           (long)(milliseconds % 1000U) * 1000000L};
  (void)nanosleep(&value, NULL);
#endif
}

bool proxy_cache_probe(const char *cache_path) {
  char path[LAGHU_RUNTIME_PATH_SIZE];
  int count;
#ifdef _WIN32
  HANDLE file;
  DWORD written;
  unsigned char marker = 0x4cU;
  count = snprintf(path, sizeof(path), "%s\\.laghu-ready-%lu-%lu-%llu",
                   cache_path, (unsigned long)GetCurrentProcessId(),
                   (unsigned long)GetCurrentThreadId(),
                   (unsigned long long)proxy_monotonic_ms());
  if (count <= 0 || (size_t)count >= sizeof(path)) return false;
  file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                     FILE_ATTRIBUTE_TEMPORARY, NULL);
  if (file == INVALID_HANDLE_VALUE) return false;
  if (!WriteFile(file, &marker, 1U, &written, NULL) || written != 1U ||
      !FlushFileBuffers(file)) {
    CloseHandle(file);
    (void)DeleteFileA(path);
    return false;
  }
  CloseHandle(file);
  return DeleteFileA(path) != 0;
#else
  int file;
  unsigned char marker = 0x4cU;
  count =
      snprintf(path, sizeof(path), "%s/.laghu-ready-%ld-%llu-%llu", cache_path,
               (long)getpid(), (unsigned long long)(uintptr_t)pthread_self(),
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
#endif
}

static bool proxy_queue_path_valid(const char *path) {
  laghu_runtime_queue queue;
  bool exists;
  bool valid = false;
  unsigned int attempt;
#ifdef _WIN32
  DWORD attributes = GetFileAttributesA(path);
  exists = attributes != INVALID_FILE_ATTRIBUTES;
  if (!exists && GetLastError() != ERROR_FILE_NOT_FOUND &&
      GetLastError() != ERROR_PATH_NOT_FOUND)
    return false;
#else
  exists = access(path, F_OK) == 0;
  if (!exists && errno != ENOENT) return false;
#endif
  if (!exists) {
    char parent[LAGHU_RUNTIME_PATH_SIZE];
    const char *separator = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    DWORD parent_attributes;
    if (backslash != NULL && (separator == NULL || backslash > separator))
      separator = backslash;
#endif
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
#ifdef _WIN32
    parent_attributes = GetFileAttributesA(parent);
    return parent_attributes != INVALID_FILE_ATTRIBUTES &&
           (parent_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
#else
    return access(parent, R_OK | X_OK) == 0;
#endif
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

#ifdef _WIN32
static BOOL WINAPI proxy_console_control(DWORD event) {
  if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT &&
      event != CTRL_CLOSE_EVENT && event != CTRL_SHUTDOWN_EVENT)
    return FALSE;
  InterlockedIncrement(&proxy_stop_requests);
  return TRUE;
}
#else
static void proxy_signal_handler(int signal_number) {
  (void)signal_number;
  if (proxy_stop_requests < 2) ++proxy_stop_requests;
}
#endif

static bool proxy_listener_ready(laghu_socket listener) {
  fd_set readable;
  struct timeval wait = {0, 200000};
  FD_ZERO(&readable);
  FD_SET(listener, &readable);
#ifdef _WIN32
  return select(0, &readable, NULL, NULL, &wait) > 0;
#else
  return select(listener + 1, &readable, NULL, NULL, &wait) > 0;
#endif
}

static void proxy_begin_drain(proxy_queue *queue) {
  proxy_queue_lock(queue);
  queue->state = PROXY_DRAINING;
  queue->stopping = true;
#ifdef _WIN32
  WakeAllConditionVariable(&queue->ready);
#else
  pthread_cond_broadcast(&queue->ready);
#endif
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

static void proxy_force_workers(proxy_queue *queue, proxy_worker *workers,
                                unsigned int worker_count) {
  unsigned int index;
  proxy_queue_lock(queue);
  queue->state = PROXY_FORCING;
  for (index = 0U; index < worker_count; ++index) {
    if (workers[index].active_client != LAGHU_INVALID_SOCKET)
      (void)shutdown(workers[index].active_client, LAGHU_SHUT_BOTH);
    if (workers[index].active_origin != LAGHU_INVALID_SOCKET)
      (void)shutdown(workers[index].active_origin, LAGHU_SHUT_BOTH);
  }
  proxy_queue_unlock(queue);
}

int laghu_proxy_run(const laghu_proxy_options *options) {
  proxy_queue queue;
  proxy_worker *workers;
  laghu_socket listener = LAGHU_INVALID_SOCKET;
  unsigned int index, started = 0U;
  int result = 1;
  bool lock_ready = false;
  if (options == NULL) return 1;
#ifndef _WIN32
  bool ready_condition = false, drained_condition = false;
#endif
#ifdef _WIN32
  WSADATA data;
  HANDLE *threads;
  bool sockets_ready = false;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
  sockets_ready = true;
#else
  pthread_t *threads;
#endif
  memset(&queue, 0, sizeof(queue));
  laghu_operational_registry_init(&queue.operational);
  queue.options = options;
  queue.capacity = options->connection_queue;
  queue.items = calloc(queue.capacity, sizeof(*queue.items));
  workers = calloc(options->workers, sizeof(*workers));
  threads = calloc(options->workers, sizeof(*threads));
  if (queue.items == NULL || workers == NULL || threads == NULL) goto cleanup;
#ifdef _WIN32
  InitializeCriticalSection(&queue.lock);
  lock_ready = true;
  InitializeConditionVariable(&queue.ready);
  InitializeConditionVariable(&queue.drained);
#else
  if (pthread_mutex_init(&queue.lock, NULL) != 0) goto cleanup;
  lock_ready = true;
  if (pthread_cond_init(&queue.ready, NULL) != 0) goto cleanup;
  ready_condition = true;
  if (pthread_cond_init(&queue.drained, NULL) != 0) goto cleanup;
  drained_condition = true;
#endif
  queue.state = PROXY_STARTING;
  queue.cache_readiness = -1;
  queue.optimizer_readiness = -1;
  queue.request_prefix = proxy_monotonic_ms() << 16U;
  {
    laghu_rum_options rum_options;
    char snapshot[LAGHU_RUNTIME_PATH_SIZE];
    char rum_error[160U];
    laghu_rum_options_init(&rum_options);
    if (options->service.rum_snapshot_path[0] != '\0')
      (void)snprintf(snapshot, sizeof(snapshot), "%s",
                     options->service.rum_snapshot_path);
    else if (snprintf(snapshot, sizeof(snapshot), "%s/rum.snapshot",
                      options->service.image_cache) <= 0)
      goto cleanup;
    rum_options.store_uri = options->service.rum_store;
    rum_options.snapshot_path = snapshot;
    rum_options.client_library = options->service.rum_client_library[0] == '\0'
                                     ? NULL
                                     : options->service.rum_client_library;
    rum_options.memory_limit = options->service.rum_memory_limit;
    rum_options.pending_limit = options->service.rum_pending_limit;
    rum_options.ttl_seconds = options->service.rum_ttl;
    rum_options.sync_interval_seconds = options->service.rum_sync_interval;
    rum_options.timeout_ms = options->service.rum_timeout_ms;
    rum_options.retry_limit = options->service.rum_retry_limit;
    rum_options.required = options->service.rum_store_required;
    queue.rum =
        laghu_rum_engine_create(&rum_options, rum_error, sizeof(rum_error));
    if (queue.rum == NULL && options->service.rum_store_required) goto cleanup;
    if (queue.rum == NULL) {
      proxy_log_event(&queue, "rum_store_fallback", "degraded");
      rum_options.store_uri = "local:";
      rum_options.client_library = NULL;
      rum_options.required = false;
      queue.rum =
          laghu_rum_engine_create(&rum_options, rum_error, sizeof(rum_error));
    }
    if (queue.rum == NULL) goto cleanup;
  }
  if (!proxy_cache_probe(options->service.image_cache)) {
    proxy_log_startup_failure(&queue, "cache_unavailable");
    goto cleanup;
  }
  if (!proxy_queue_path_valid(options->service.worker_queue) ||
      (options->service.font_providers != NULL &&
       !proxy_queue_path_valid(options->service.font_fetch_queue)) ||
      (options->service.javascript_queue[0] != '\0' &&
       !proxy_queue_path_valid(options->service.javascript_queue))) {
    proxy_log_startup_failure(&queue, "queue_unavailable");
    goto cleanup;
  }
  if (options->service.source_policy.mode != LAGHU_SOURCE_FILE_OFF &&
      !laghu_source_registry_publish(options->service.asset_upload_queue,
                                     &options->service.source_policy)) {
    proxy_log_startup_failure(&queue, "source_registry");
    goto cleanup;
  }
  if (!laghu_cache_backend_register_path(options->service.image_cache,
                                         &options->service.cache_limits)) {
    proxy_log_startup_failure(&queue, "cache_backend");
    goto cleanup;
  }
  if ((options->service.metrics || options->service.readiness) &&
      !laghu_operational_registry_open(
          &queue.operational, options->service.image_cache,
          LAGHU_OPERATIONAL_SURFACE_STANDALONE,
          LAGHU_OPERATIONAL_PROCESS_ADAPTER, true, (uint64_t)time(NULL))) {
    proxy_log_event(&queue, "observability", "unavailable");
  }
  if (options->origin_tls &&
      (queue.tls_context = proxy_tls_context(options)) == NULL) {
    proxy_log_startup_failure(&queue, "origin_tls");
    goto cleanup;
  }
  listener = proxy_listen(options->listen_host, options->listen_port);
  if (listener == LAGHU_INVALID_SOCKET) {
    proxy_log_startup_failure(&queue, "listen");
    goto cleanup;
  }
  proxy_stop_requests = 0;
#ifdef _WIN32
  (void)SetConsoleCtrlHandler(proxy_console_control, TRUE);
#else
  {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = proxy_signal_handler;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    action.sa_handler = SIG_IGN;
    (void)sigaction(SIGPIPE, &action, NULL);
  }
#endif
  for (index = 0U; index < options->workers; ++index) {
    workers[index].queue = &queue;
    workers[index].active_client = LAGHU_INVALID_SOCKET;
    workers[index].active_origin = LAGHU_INVALID_SOCKET;
#ifdef _WIN32
    threads[index] =
        CreateThread(NULL, 0U, proxy_worker_main, &workers[index], 0U, NULL);
    if (threads[index] == NULL) break;
#else
    if (pthread_create(&threads[index], NULL, proxy_worker_main,
                       &workers[index]) != 0)
      break;
#endif
    ++started;
  }
  if (started != options->workers) {
    proxy_log_event(&queue, "startup_failure", "forcing");
    proxy_stop_requests = 2;
  } else {
    queue.state = PROXY_RUNNING;
    proxy_log_event(&queue, "startup", "running");
    while (proxy_stop_requests == 0) {
      if (proxy_listener_ready(listener)) {
        proxy_connection connection;
        memset(&connection, 0, sizeof(connection));
        connection.peer_length = (laghu_socklen)sizeof(connection.peer);
        connection.socket =
            accept(listener, (struct sockaddr *)&connection.peer,
                   &connection.peer_length);
        if (connection.socket != LAGHU_INVALID_SOCKET &&
            !queue_push(&queue, &connection))
          proxy_reject_connection(&queue, connection.socket, "queue_saturated");
      }
    }
  }
  laghu_close(listener);
  listener = LAGHU_INVALID_SOCKET;
  proxy_begin_drain(&queue);
  proxy_log_event(&queue, "shutdown", "draining");
  {
    uint64_t deadline =
        proxy_monotonic_ms() + (uint64_t)options->drain_timeout * 1000U;
    while (proxy_active_count(&queue) != 0U && proxy_stop_requests < 2 &&
           proxy_monotonic_ms() < deadline)
      proxy_pause_ms(20U);
  }
  if (proxy_active_count(&queue) != 0U) {
    proxy_force_workers(&queue, workers, started);
    proxy_log_event(&queue, "shutdown", "forcing");
  }
  for (index = 0U; index < started; ++index) {
#ifdef _WIN32
    (void)WaitForSingleObject(threads[index], INFINITE);
    CloseHandle(threads[index]);
#else
    (void)pthread_join(threads[index], NULL);
#endif
  }
  queue.state = PROXY_STOPPED;
  proxy_log_event(&queue, "shutdown", "stopped");
  result = started == options->workers ? 0 : 1;
cleanup:
  laghu_operational_registry_close(&queue.operational);
  laghu_rum_engine_destroy(queue.rum);
  if (listener != LAGHU_INVALID_SOCKET) laghu_close(listener);
  SSL_CTX_free(queue.tls_context);
#ifdef _WIN32
  if (lock_ready) DeleteCriticalSection(&queue.lock);
#else
  if (drained_condition) pthread_cond_destroy(&queue.drained);
  if (ready_condition) pthread_cond_destroy(&queue.ready);
  if (lock_ready) pthread_mutex_destroy(&queue.lock);
#endif
  free(threads);
  free(workers);
  free(queue.items);
#ifdef _WIN32
  if (sockets_ready) WSACleanup();
#endif
  return result;
}
