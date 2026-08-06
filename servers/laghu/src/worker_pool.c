// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "server_internal.h"

bool queue_push(proxy_queue *queue, const proxy_connection *connection) {
  bool accepted = false;
  pthread_mutex_lock(&queue->lock);
  if (!queue->stopping && queue->count < queue->capacity) {
    queue->items[(queue->head + queue->count) % queue->capacity] = *connection;
    ++queue->count;
    accepted = true;
    pthread_cond_signal(&queue->ready);
  }
  pthread_mutex_unlock(&queue->lock);
  return accepted;
}

static bool queue_pop(proxy_queue *queue, proxy_connection *connection) {
  bool available;
  pthread_mutex_lock(&queue->lock);
  while (queue->count == 0U && !queue->stopping)
    pthread_cond_wait(&queue->ready, &queue->lock);
  available = !queue->stopping && queue->count != 0U;
  if (available) {
    *connection = queue->items[queue->head];
    queue->head = (queue->head + 1U) % queue->capacity;
    --queue->count;
  }
  pthread_mutex_unlock(&queue->lock);
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
  pthread_cond_broadcast(&worker->queue->drained);
  proxy_queue_unlock(worker->queue);
}

static laghu_runtime_queue *proxy_worker_queue_if_ready(
    proxy_worker *worker, laghu_runtime_queue *runtime_queue, bool *ready) {
  laghu_runtime_queue *result = NULL;
  proxy_queue_lock(worker->queue);
  if (*ready) result = runtime_queue;
  proxy_queue_unlock(worker->queue);
  return result;
}

laghu_runtime_queue *proxy_runtime_queue(proxy_worker *worker) {
  return proxy_worker_queue_if_ready(worker, &worker->queue->runtime_queue,
                                     &worker->queue->runtime_queue_ready);
}

laghu_runtime_queue *proxy_font_fetch_queue(proxy_worker *worker) {
  return proxy_worker_queue_if_ready(worker, &worker->queue->font_fetch_queue,
                                     &worker->queue->font_fetch_queue_ready);
}

laghu_runtime_queue *proxy_javascript_queue(proxy_worker *worker) {
  return proxy_worker_queue_if_ready(worker, &worker->queue->javascript_queue,
                                     &worker->queue->javascript_queue_ready);
}

void *proxy_worker_main(void *argument) {
  proxy_worker *worker = argument;
  proxy_connection connection;
  while (queue_pop(worker->queue, &connection)) {
    proxy_worker_begin(worker, connection.socket);
    proxy_handle(&connection, worker);
    proxy_worker_end(worker);
  }
  return NULL;
}
