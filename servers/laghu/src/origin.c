// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <errno.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "server_internal.h"

void proxy_timeout(laghu_socket socket, unsigned int seconds) {
  struct timeval value = {(time_t)seconds, 0};
  (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
  (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
}

static void proxy_timeout_ms(laghu_socket socket, uint64_t milliseconds) {
  struct timeval value;
  if (milliseconds == 0U) milliseconds = 1U;
  value.tv_sec = (time_t)(milliseconds / 1000U);
  value.tv_usec = (suseconds_t)((milliseconds % 1000U) * 1000U);
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

/* Downstream request reads retain the socket's idle limit, while this monotonic
 * deadline is never reset after a successful byte. Upstream response reads
 * deliberately keep their existing independent origin timeout behavior. */
int proxy_client_recv_until(laghu_socket socket, SSL *tls, void *data, size_t length, unsigned int idle_timeout, uint64_t deadline) {
  for (;;) {
    uint64_t now = proxy_monotonic_ms();
    uint64_t idle_ms = (uint64_t)idle_timeout * 1000U;
    uint64_t remaining;
    int got;
    if (now >= deadline) {
      errno = ETIMEDOUT;
      return -1;
    }
    remaining = deadline - now;
    if (remaining > idle_ms) remaining = idle_ms;
    proxy_timeout_ms(socket, remaining);
    got = tls == NULL ? recv(socket, data, (int)length, 0) : SSL_read(tls, data, (int)length);
    if (got >= 0) return got;
    if (tls == NULL) {
      if (errno == EINTR) continue;
      return -1;
    }
    {
      int error = SSL_get_error(tls, got);
      if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        struct pollfd ready = {(int)socket, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, 0};
        now = proxy_monotonic_ms();
        if (now >= deadline) {
          errno = ETIMEDOUT;
          return -1;
        }
        remaining = deadline - now;
        if (remaining > idle_ms) remaining = idle_ms;
        got = poll(&ready, 1U, (int)remaining);
        if (got > 0) continue;
        if (got < 0 && errno == EINTR) continue;
        if (got == 0) errno = EAGAIN;
      } else if (error == SSL_ERROR_SYSCALL && errno == EINTR) {
        continue;
      }
    }
    return -1;
  }
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

bool proxy_client_send_all(laghu_socket socket, SSL *tls, const void *data, size_t length) {
  return proxy_origin_send_all(socket, tls, data, length);
}

SSL_CTX *proxy_tls_context(const laghu_proxy_options *options) {
  SSL_CTX *context;
  if (options == NULL) return NULL;
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

static SSL_CTX *proxy_downstream_tls_context_files(const char *certificate, const char *private_key) {
  SSL_CTX *context;
  context = SSL_CTX_new(TLS_server_method());
  if (context == NULL) return NULL;
  if (!SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) || SSL_CTX_use_certificate_chain_file(context, certificate) != 1 ||
      SSL_CTX_use_PrivateKey_file(context, private_key, SSL_FILETYPE_PEM) != 1 || SSL_CTX_check_private_key(context) != 1) {
    SSL_CTX_free(context);
    return NULL;
  }
  SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
  return context;
}

static int proxy_downstream_sni(SSL *tls, int *alert, void *argument) {
  proxy_queue *queue = argument;
  const char *name;
  const laghu_proxy_options *options;
  size_t index;
  (void)alert;
  if (queue == NULL || (name = SSL_get_servername(tls, TLSEXT_NAMETYPE_host_name)) == NULL || name[0] == '\0') return SSL_TLSEXT_ERR_OK;
  options = queue->downstream_tls_options;
  for (index = 0U; options != NULL && index < options->site_count; ++index) {
    const laghu_proxy_site *site = &options->sites[index];
    if (site->downstream_tls_context != NULL && !strcasecmp(name, site->host)) {
      (void)SSL_set_SSL_CTX(tls, site->downstream_tls_context);
      break;
    }
  }
  return SSL_TLSEXT_ERR_OK;
}

SSL_CTX *proxy_downstream_tls_context(proxy_queue *queue, const laghu_proxy_options *options) {
  SSL_CTX *context;
  const char *certificate = options == NULL ? NULL : options->tls_certificate;
  const char *private_key = options == NULL ? NULL : options->tls_private_key;
  size_t index;
  if (options == NULL || !options->downstream_tls) return NULL;
  if (certificate[0] == '\0') {
    for (index = 0U; index < options->site_count; ++index) {
      if (options->sites[index].tls_certificate[0] != '\0') {
        certificate = options->sites[index].tls_certificate;
        private_key = options->sites[index].tls_private_key;
        break;
      }
    }
  }
  context = certificate[0] == '\0' || private_key[0] == '\0' ? NULL : proxy_downstream_tls_context_files(certificate, private_key);
  if (context == NULL) return NULL;
  for (index = 0U; index < options->site_count; ++index) {
    laghu_proxy_site *site = (laghu_proxy_site *)&options->sites[index];
    if (site->tls_certificate[0] == '\0') continue;
    site->downstream_tls_context = proxy_downstream_tls_context_files(site->tls_certificate, site->tls_private_key);
    if (site->downstream_tls_context == NULL) {
      SSL_CTX_free(context);
      while (index != 0U) {
        --index;
        SSL_CTX_free(options->sites[index].downstream_tls_context);
        ((laghu_proxy_site *)&options->sites[index])->downstream_tls_context = NULL;
      }
      return NULL;
    }
  }
  SSL_CTX_set_tlsext_servername_callback(context, proxy_downstream_sni);
  SSL_CTX_set_tlsext_servername_arg(context, queue);
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

SSL *proxy_downstream_tls_handshake(proxy_worker *worker, laghu_socket socket, unsigned int timeout, bool *timed_out) {
  SSL *tls = SSL_new(worker->queue->downstream_tls_context);
  bool complete = false;
  uint64_t deadline;
  int flags = fcntl(socket, F_GETFL, 0);
  *timed_out = false;
  if (tls == NULL || flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0 || !SSL_set_fd(tls, (int)socket)) {
    SSL_free(tls);
    return NULL;
  }
  deadline = proxy_monotonic_ms() + (uint64_t)timeout * 1000U;
  for (;;) {
    int result = SSL_accept(tls);
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
    if ((error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) || proxy_is_forcing(worker->queue)) break;
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

bool proxy_read_client_headers(laghu_socket socket, char *buffer, SSL *tls, unsigned int idle_timeout, unsigned int total_timeout, size_t *length,
                               unsigned char **body_start, size_t *body_initial) {
  uint64_t deadline = proxy_monotonic_ms() + (uint64_t)total_timeout * 1000U;
  size_t used = 0U;
  bool complete = false;
  while (used < LAGHU_PROXY_HEADER_BYTES) {
    int got = proxy_client_recv_until(socket, tls, buffer + used, LAGHU_PROXY_HEADER_BYTES - used, idle_timeout, deadline);
    char *end;
    if (got <= 0) break;
    used += (size_t)got;
    buffer[used] = '\0';
    end = strstr(buffer, "\r\n\r\n");
    if (end != NULL) {
      size_t header_length = (size_t)(end - buffer) + 4U;
      *body_start = (unsigned char *)buffer + header_length;
      *body_initial = used - header_length;
      *length = header_length;
      complete = true;
      break;
    }
  }
  proxy_timeout(socket, idle_timeout);
  return complete;
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

static bool proxy_origin_idle_alive(const proxy_origin_connection *origin) {
  laghu_socket socket = origin->socket;
  struct pollfd ready = {(int)socket, POLLIN, 0};
  unsigned char byte;
  int received;
  if (origin->tls != NULL && SSL_pending(origin->tls) > 0) return false;
  int selected = poll(&ready, 1U, 0);
  if (selected < 0) return false;
  if (selected == 0 || (ready.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) == 0) return true;
  if ((ready.revents & POLLNVAL) != 0) return false;
  received = recv(socket, &byte, 1U, MSG_PEEK | MSG_DONTWAIT);
  if (received > 0) return false;
  if (received == 0) return false;
  return errno == EAGAIN || errno == EWOULDBLOCK;
}

static proxy_origin_connection proxy_origin_pool_remove(proxy_queue *queue, unsigned int index) {
  proxy_origin_connection candidate = queue->origins[index];
  unsigned int last = --queue->origin_count;
  if (index != last) queue->origins[index] = queue->origins[last];
  memset(&queue->origins[last], 0, sizeof(queue->origins[last]));
  queue->origins[last].socket = LAGHU_INVALID_SOCKET;
  return candidate;
}

static unsigned int proxy_origin_pool_oldest(const proxy_queue *queue) {
  unsigned int oldest = 0U;
  unsigned int index;
  for (index = 1U; index < queue->origin_count; ++index)
    if (queue->origins[index].idle_since_ms < queue->origins[oldest].idle_since_ms) oldest = index;
  return oldest;
}

bool proxy_origin_acquire(proxy_worker *worker, const laghu_proxy_options *options, proxy_origin_connection *origin, const char *host,
                          const char *port, const char *authority, bool origin_tls, bool *timed_out) {
  proxy_queue *queue = worker->queue;
  uint64_t now = proxy_monotonic_ms();
  unsigned int index = 0U;
  memset(origin, 0, sizeof(*origin));
  origin->socket = LAGHU_INVALID_SOCKET;
  *timed_out = false;
  proxy_queue_lock(queue);
  while (index < queue->origin_count) {
    proxy_origin_connection candidate = queue->origins[index];
    bool matches = candidate.origin_tls == origin_tls && !strcmp(candidate.authority, authority);
    bool expired = now - candidate.idle_since_ms >= (uint64_t)options->origin_idle_timeout * 1000U;
    bool alive = !expired && proxy_origin_idle_alive(&candidate);
    if (matches && alive) {
      *origin = proxy_origin_pool_remove(queue, index);
      proxy_queue_unlock(queue);
      proxy_worker_origin(worker, origin->socket);
      return true;
    }
    if (!alive) {
      candidate = proxy_origin_pool_remove(queue, index);
      proxy_queue_unlock(queue);
      proxy_origin_dispose(&candidate);
      proxy_queue_lock(queue);
    } else {
      ++index;
    }
  }
  proxy_queue_unlock(queue);
  origin->socket = proxy_connect(worker, host, port, options->connect_timeout);
  if (origin->socket == LAGHU_INVALID_SOCKET) return false;
  if (origin_tls) {
    origin->tls = proxy_tls_handshake(worker, origin->socket, host, options->connect_timeout, timed_out);
    if (origin->tls == NULL) {
      proxy_origin_dispose(origin);
      proxy_worker_origin(worker, LAGHU_INVALID_SOCKET);
      return false;
    }
  }
  (void)snprintf(origin->authority, sizeof(origin->authority), "%s", authority);
  origin->origin_tls = origin_tls;
  proxy_timeout(origin->socket, options->io_timeout);
  return true;
}

void proxy_origin_release(proxy_worker *worker, const laghu_proxy_options *options, proxy_origin_connection *origin, bool reusable) {
  proxy_queue *queue = worker->queue;
  proxy_origin_connection evicted = {.socket = LAGHU_INVALID_SOCKET};
  proxy_worker_origin(worker, LAGHU_INVALID_SOCKET);
  if (!reusable || options->origin_pool_size == 0U || proxy_is_forcing(queue)) {
    proxy_origin_dispose(origin);
    return;
  }
  origin->idle_since_ms = proxy_monotonic_ms();
  proxy_queue_lock(queue);
  if (queue->origin_count < options->origin_pool_size) {
    queue->origins[queue->origin_count++] = *origin;
    origin->socket = LAGHU_INVALID_SOCKET;
    origin->tls = NULL;
  } else {
    unsigned int oldest = proxy_origin_pool_oldest(queue);
    /* Capacity is shared across authorities. Replace the oldest idle entry
     * deterministically instead of silently dropping the returned authority. */
    evicted = queue->origins[oldest];
    queue->origins[oldest] = *origin;
    origin->socket = LAGHU_INVALID_SOCKET;
    origin->tls = NULL;
  }
  proxy_queue_unlock(queue);
  proxy_origin_dispose(&evicted);
  proxy_origin_dispose(origin);
}

void proxy_origin_pool_close(proxy_queue *queue) {
  unsigned int index;
  proxy_queue_lock(queue);
  for (index = 0U; index < queue->origin_count; ++index) proxy_origin_dispose(&queue->origins[index]);
  queue->origin_count = 0U;
  proxy_queue_unlock(queue);
}
