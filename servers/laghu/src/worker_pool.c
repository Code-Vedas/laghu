// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "server_internal.h"

bool queue_push(proxy_queue *queue, const proxy_connection *connection) {
  bool accepted = false;
#ifdef _WIN32
  EnterCriticalSection(&queue->lock);
#else
  pthread_mutex_lock(&queue->lock);
#endif
  if (!queue->stopping && queue->count < queue->capacity) {
    queue->items[(queue->head + queue->count) % queue->capacity] = *connection;
    ++queue->count;
    accepted = true;
#ifdef _WIN32
    WakeConditionVariable(&queue->ready);
#else
    pthread_cond_signal(&queue->ready);
#endif
  }
#ifdef _WIN32
  LeaveCriticalSection(&queue->lock);
#else
  pthread_mutex_unlock(&queue->lock);
#endif
  return accepted;
}

static bool queue_pop(proxy_queue *queue, proxy_connection *connection) {
  bool available;
#ifdef _WIN32
  EnterCriticalSection(&queue->lock);
  while (queue->count == 0U && !queue->stopping)
    SleepConditionVariableCS(&queue->ready, &queue->lock, INFINITE);
#else
  pthread_mutex_lock(&queue->lock);
  while (queue->count == 0U && !queue->stopping)
    pthread_cond_wait(&queue->ready, &queue->lock);
#endif
  available = !queue->stopping && queue->count != 0U;
  if (available) {
    *connection = queue->items[queue->head];
    queue->head = (queue->head + 1U) % queue->capacity;
    --queue->count;
  }
#ifdef _WIN32
  LeaveCriticalSection(&queue->lock);
#else
  pthread_mutex_unlock(&queue->lock);
#endif
  return available;
}

static void proxy_worker_begin(proxy_worker *worker, laghu_socket client) {
  proxy_queue_lock(worker->queue);
  worker->active_client = client;
  worker->active_origin = LAGHU_INVALID_SOCKET;
  ++worker->queue->active_count;
  proxy_queue_unlock(worker->queue);
}

static void proxy_worker_end(proxy_worker *worker) {
  proxy_queue_lock(worker->queue);
  worker->active_client = LAGHU_INVALID_SOCKET;
  worker->active_origin = LAGHU_INVALID_SOCKET;
  if (worker->queue->active_count != 0U) --worker->queue->active_count;
#ifdef _WIN32
  WakeAllConditionVariable(&worker->queue->drained);
#else
  pthread_cond_broadcast(&worker->queue->drained);
#endif
  proxy_queue_unlock(worker->queue);
}

#ifdef _WIN32
DWORD WINAPI proxy_worker_main(LPVOID argument)
#else
void *proxy_worker_main(void *argument)
#endif
{
  proxy_worker *worker = argument;
  proxy_connection connection;
  laghu_runtime_queue_init(&worker->runtime_queue);
  laghu_runtime_queue_init(&worker->font_fetch_queue);
  laghu_runtime_queue_init(&worker->javascript_queue);
  if (worker->queue->options->font_providers_loaded)
    (void)laghu_runtime_queue_open(
        &worker->font_fetch_queue,
        worker->queue->options->font_fetch_queue_path);
  if (worker->queue->options->javascript_queue_enabled)
    (void)laghu_runtime_queue_open(
        &worker->javascript_queue,
        worker->queue->options->javascript_queue_path);
  while (queue_pop(worker->queue, &connection)) {
    proxy_worker_begin(worker, connection.socket);
    proxy_handle(&connection, worker);
    proxy_worker_end(worker);
  }
  laghu_runtime_queue_close(&worker->runtime_queue);
  laghu_runtime_queue_close(&worker->font_fetch_queue);
  laghu_runtime_queue_close(&worker->javascript_queue);
#ifdef _WIN32
  return 0U;
#else
  return NULL;
#endif
}
