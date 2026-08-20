// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <errno.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include "server_internal.h"

void proxy_timeout(laghu_socket socket, unsigned int seconds) {
  struct timeval value = {(time_t)seconds, 0};
  (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
  (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
}

bool proxy_send_all(laghu_socket socket, const void *data, size_t length) {
  const char *bytes = data;
  while (length != 0U) {
    int sent = send(socket, bytes, (int)(length > 65536U ? 65536U : length), 0);
    if (sent <= 0) return false;
    bytes += sent;
    length -= (size_t)sent;
  }
  return true;
}

bool proxy_socket_timed_out(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT; }

int proxy_origin_recv(laghu_socket socket, SSL *tls, void *data, size_t length) {
  if (tls == NULL) return recv(socket, data, (int)length, 0);
  return SSL_read(tls, data, (int)length);
}

bool proxy_origin_send_all(laghu_socket socket, SSL *tls, const void *data, size_t length) {
  const unsigned char *bytes = data;
  if (tls == NULL) return proxy_send_all(socket, data, length);
  while (length != 0U) {
    int sent = SSL_write(tls, bytes, (int)(length > 65536U ? 65536U : length));
    if (sent <= 0) return false;
    bytes += sent;
    length -= (size_t)sent;
  }
  return true;
}

SSL_CTX *proxy_tls_context(const laghu_proxy_options *options) {
  SSL_CTX *context;
  if (!options->origin_tls) return NULL;
  context = SSL_CTX_new(TLS_client_method());
  if (context == NULL) return NULL;
  if (!SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) || !SSL_CTX_set_default_verify_paths(context) ||
      (options->origin_ca_file[0] != '\0' && !SSL_CTX_load_verify_locations(context, options->origin_ca_file, NULL))) {
    SSL_CTX_free(context);
    return NULL;
  }
  SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
  SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
  return context;
}

SSL *proxy_tls_handshake(proxy_worker *worker, laghu_socket socket, const char *host, unsigned int timeout, bool *timed_out) {
  static const unsigned char alpn[] = {8U, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  SSL *tls = SSL_new(worker->queue->tls_context);
  X509_VERIFY_PARAM *parameters;
  bool ip_literal;
  bool complete = false;
  uint64_t deadline;
  int flags = fcntl(socket, F_GETFL, 0);
  *timed_out = false;
  if (tls == NULL) return NULL;
  ip_literal = inet_pton(AF_INET, host, (unsigned char[4]){0}) == 1 || inet_pton(AF_INET6, host, (unsigned char[16]){0}) == 1;
  parameters = SSL_get0_param(tls);
  if ((ip_literal && !X509_VERIFY_PARAM_set1_ip_asc(parameters, host)) ||
      (!ip_literal && (!SSL_set_tlsext_host_name(tls, host) || !SSL_set1_host(tls, host))) || SSL_set_alpn_protos(tls, alpn, sizeof(alpn)) != 0 ||
      !SSL_set_fd(tls, (int)socket)) {
    SSL_free(tls);
    return NULL;
  }
  if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0) {
    SSL_free(tls);
    return NULL;
  }
  deadline = proxy_monotonic_ms() + (uint64_t)timeout * 1000U;
  for (;;) {
    int result = SSL_connect(tls);
    int error;
    if (result == 1) {
      complete = true;
      break;
    }
    error = SSL_get_error(tls, result);
    if (proxy_monotonic_ms() >= deadline) {
      *timed_out = true;
      errno = ETIMEDOUT;
      break;
    }
    if ((error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) || proxy_is_forcing(worker->queue)) {
      break;
    }
    {
      struct pollfd ready = {(int)socket, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, 0};
      (void)poll(&ready, 1U, 200);
    }
  }
  (void)fcntl(socket, F_SETFL, flags);
  if (!complete) {
    SSL_free(tls);
    return NULL;
  }
  {
    const unsigned char *selected = NULL;
    unsigned int selected_length = 0U;
    SSL_get0_alpn_selected(tls, &selected, &selected_length);
    if (SSL_get_verify_result(tls) != X509_V_OK || (selected_length != 0U && (selected_length != 8U || memcmp(selected, "http/1.1", 8U) != 0))) {
      SSL_free(tls);
      return NULL;
    }
  }
  return tls;
}

bool proxy_read_headers(laghu_socket socket, char *buffer, SSL *tls, size_t *length, unsigned char **body_start, size_t *body_initial) {
  size_t used = 0U;
  while (used < LAGHU_PROXY_HEADER_BYTES) {
    int got = proxy_origin_recv(socket, tls, buffer + used, LAGHU_PROXY_HEADER_BYTES - used);
    char *end;
    if (got <= 0) return false;
    used += (size_t)got;
    buffer[used] = '\0';
    end = strstr(buffer, "\r\n\r\n");
    if (end != NULL) {
      size_t header_length = (size_t)(end - buffer) + 4U;
      *body_start = (unsigned char *)buffer + header_length;
      *body_initial = used - header_length;
      *length = header_length;
      return true;
    }
  }
  return false;
}

laghu_socket proxy_connect(proxy_worker *worker, const char *host, const char *port, unsigned int timeout) {
  struct addrinfo hints, *addresses = NULL, *address;
  laghu_socket descriptor = LAGHU_INVALID_SOCKET;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, port, &hints, &addresses) != 0) return descriptor;
  for (address = addresses; address != NULL; address = address->ai_next) {
    descriptor = (laghu_socket)socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (descriptor == LAGHU_INVALID_SOCKET) continue;
    proxy_worker_origin(worker, descriptor);
    {
      bool connected = false;
      int flags = fcntl(descriptor, F_GETFL, 0);
      int result;
      if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        proxy_worker_origin(worker, LAGHU_INVALID_SOCKET);
        laghu_close(descriptor);
        descriptor = LAGHU_INVALID_SOCKET;
        continue;
      }
      result = connect(descriptor, address->ai_addr, (laghu_socklen)address->ai_addrlen);
      if (result == 0) {
        connected = true;
      } else if (errno == EINPROGRESS) {
        uint64_t deadline = proxy_monotonic_ms() + (uint64_t)timeout * 1000U;
        int socket_error = 0;
        laghu_socklen error_length = (laghu_socklen)sizeof(socket_error);
        while (!proxy_is_forcing(worker->queue) && proxy_monotonic_ms() < deadline) {
          struct pollfd writable = {(int)descriptor, POLLOUT, 0};
          int selected;
          selected = poll(&writable, 1U, 200);
          if (selected > 0 && getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) == 0 && socket_error == 0) {
            connected = true;
            break;
          }
          if (selected < 0) break;
        }
      }
      (void)fcntl(descriptor, F_SETFL, flags);
      if (connected) {
        proxy_timeout(descriptor, timeout);
        break;
      }
    }
    proxy_worker_origin(worker, LAGHU_INVALID_SOCKET);
    laghu_close(descriptor);
    descriptor = LAGHU_INVALID_SOCKET;
  }
  freeaddrinfo(addresses);
  return descriptor;
}

static void proxy_origin_dispose(proxy_origin_connection *origin) {
  if (origin->tls != NULL) SSL_free(origin->tls);
  if (origin->socket != LAGHU_INVALID_SOCKET) laghu_close(origin->socket);
  memset(origin, 0, sizeof(*origin));
  origin->socket = LAGHU_INVALID_SOCKET;
}

static bool proxy_origin_idle_alive(laghu_socket socket) {
  struct pollfd ready = {(int)socket, POLLIN, 0};
  unsigned char byte;
  int selected = poll(&ready, 1U, 0);
  if (selected < 0) return false;
  if (selected == 0 || (ready.revents & (POLLIN | POLLHUP | POLLERR)) == 0) return true;
  return recv(socket, &byte, 1U, MSG_PEEK | MSG_DONTWAIT) > 0 ? false : errno == EAGAIN || errno == EWOULDBLOCK;
}

bool proxy_origin_acquire(proxy_worker *worker, proxy_origin_connection *origin, bool *timed_out) {
  proxy_queue *queue = worker->queue;
  const laghu_proxy_options *options = queue->options;
  uint64_t now = proxy_monotonic_ms();
  unsigned int index = 0U;
  memset(origin, 0, sizeof(*origin));
  origin->socket = LAGHU_INVALID_SOCKET;
  *timed_out = false;
  proxy_queue_lock(queue);
  while (index < queue->origin_count) {
    proxy_origin_connection candidate = queue->origins[index];
    bool matches = candidate.origin_tls == options->origin_tls && !strcmp(candidate.authority, options->origin_authority);
    bool expired = now - candidate.idle_since_ms >= (uint64_t)options->origin_idle_timeout * 1000U;
    queue->origins[index] = queue->origins[queue->origin_count - 1U];
    --queue->origin_count;
    if (matches && !expired && proxy_origin_idle_alive(candidate.socket)) {
      *origin = candidate;
      proxy_queue_unlock(queue);
      proxy_worker_origin(worker, origin->socket);
      return true;
    }
    proxy_queue_unlock(queue);
    proxy_origin_dispose(&candidate);
    proxy_queue_lock(queue);
  }
  proxy_queue_unlock(queue);
  origin->socket = proxy_connect(worker, options->origin_host, options->origin_port, options->connect_timeout);
  if (origin->socket == LAGHU_INVALID_SOCKET) return false;
  if (options->origin_tls) {
    origin->tls = proxy_tls_handshake(worker, origin->socket, options->origin_host, options->connect_timeout, timed_out);
    if (origin->tls == NULL) {
      proxy_origin_dispose(origin);
      proxy_worker_origin(worker, LAGHU_INVALID_SOCKET);
      return false;
    }
  }
  (void)snprintf(origin->authority, sizeof(origin->authority), "%s", options->origin_authority);
  origin->origin_tls = options->origin_tls;
  proxy_timeout(origin->socket, options->io_timeout);
  return true;
}

void proxy_origin_release(proxy_worker *worker, proxy_origin_connection *origin, bool reusable) {
  proxy_queue *queue = worker->queue;
  proxy_worker_origin(worker, LAGHU_INVALID_SOCKET);
  if (!reusable || queue->options->origin_pool_size == 0U || proxy_is_forcing(queue)) {
    proxy_origin_dispose(origin);
    return;
  }
  origin->idle_since_ms = proxy_monotonic_ms();
  proxy_queue_lock(queue);
  if (queue->origin_count < queue->options->origin_pool_size) {
    queue->origins[queue->origin_count++] = *origin;
    origin->socket = LAGHU_INVALID_SOCKET;
    origin->tls = NULL;
  }
  proxy_queue_unlock(queue);
  proxy_origin_dispose(origin);
}

void proxy_origin_pool_close(proxy_queue *queue) {
  unsigned int index;
  proxy_queue_lock(queue);
  for (index = 0U; index < queue->origin_count; ++index) proxy_origin_dispose(&queue->origins[index]);
  queue->origin_count = 0U;
  proxy_queue_unlock(queue);
}
