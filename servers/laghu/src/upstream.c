// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server_internal.h"

typedef struct {
  unsigned char data[LAGHU_PROXY_HEADER_BYTES];
  size_t length;
} proxy_gateway_parameters;

static void upstream_primary(const laghu_proxy_route *route, laghu_proxy_upstream_target *target) {
  memset(target, 0, sizeof(*target));
  (void)snprintf(target->host, sizeof(target->host), "%s", route->upstream_host);
  (void)snprintf(target->port, sizeof(target->port), "%s", route->upstream_port);
  (void)snprintf(target->authority, sizeof(target->authority), "%s", route->upstream_authority);
  target->tls = route->upstream_tls;
}

const char *proxy_upstream_protocol_name(laghu_proxy_upstream_protocol protocol) {
  switch (protocol) {
    case LAGHU_PROXY_UPSTREAM_HTTP:
      return "http";
    case LAGHU_PROXY_UPSTREAM_FASTCGI:
      return "fastcgi";
    case LAGHU_PROXY_UPSTREAM_UWSGI:
      return "uwsgi";
    case LAGHU_PROXY_UPSTREAM_SCGI:
      return "scgi";
  }
  return "unknown";
}

static size_t upstream_route_index(const laghu_proxy_options *options, const laghu_proxy_route *route) {
  if (options == NULL || route == NULL || route < options->routes || route >= options->routes + options->route_count) return LAGHU_PROXY_MAX_ROUTES;
  return (size_t)(route - options->routes);
}

static bool upstream_available(proxy_worker *worker, const laghu_proxy_options *options, const laghu_proxy_route *route, size_t target_index) {
  size_t route_index = upstream_route_index(options, route);
  bool available = true;
  if (route_index == LAGHU_PROXY_MAX_ROUTES || target_index > LAGHU_PROXY_MAX_FAILOVERS) return false;
  proxy_queue_lock(worker->queue);
  if (worker->queue->upstream_health[route_index][target_index].known && !worker->queue->upstream_health[route_index][target_index].healthy)
    available = false;
  proxy_queue_unlock(worker->queue);
  return available;
}

static void upstream_mark(proxy_worker *worker, const laghu_proxy_options *options, const laghu_proxy_route *route, size_t target_index,
                          bool healthy) {
  size_t route_index = upstream_route_index(options, route);
  if (route_index == LAGHU_PROXY_MAX_ROUTES || target_index > LAGHU_PROXY_MAX_FAILOVERS) return;
  proxy_queue_lock(worker->queue);
  worker->queue->upstream_health[route_index][target_index].known = true;
  worker->queue->upstream_health[route_index][target_index].healthy = healthy;
  proxy_queue_unlock(worker->queue);
}

bool proxy_route_acquire(proxy_worker *worker, const laghu_proxy_options *options, const laghu_proxy_route *route, proxy_origin_connection *origin,
                         laghu_proxy_upstream_target *selected, unsigned int *failovers, bool *timed_out) {
  size_t index;
  bool attempt_timed_out = false;
  if (worker == NULL || options == NULL || route == NULL || origin == NULL || selected == NULL || failovers == NULL || timed_out == NULL)
    return false;
  *failovers = 0U;
  *timed_out = false;
  for (index = 0U; index <= route->failover_count; ++index) {
    laghu_proxy_upstream_target target;
    if (!upstream_available(worker, options, route, index)) {
      if (index != route->failover_count) ++*failovers;
      continue;
    }
    if (index == 0U)
      upstream_primary(route, &target);
    else
      target = route->failovers[index - 1U];
    if (proxy_origin_acquire(worker, options, origin, target.host, target.port, target.authority, target.tls, &attempt_timed_out)) {
      *selected = target;
      upstream_mark(worker, options, route, index, true);
      return true;
    }
    upstream_mark(worker, options, route, index, false);
    *timed_out = *timed_out || attempt_timed_out;
    if (index != route->failover_count) ++*failovers;
  }
  return false;
}

void proxy_route_mark_unhealthy(proxy_worker *worker, const laghu_proxy_options *options, const laghu_proxy_route *route,
                                const laghu_proxy_upstream_target *target) {
  size_t index;
  laghu_proxy_upstream_target primary;
  if (worker == NULL || options == NULL || route == NULL || target == NULL) return;
  upstream_primary(route, &primary);
  if (primary.tls == target->tls && !strcmp(primary.host, target->host) && !strcmp(primary.port, target->port) &&
      !strcmp(primary.authority, target->authority)) {
    upstream_mark(worker, options, route, 0U, false);
    return;
  }
  for (index = 0U; index < route->failover_count; ++index)
    if (route->failovers[index].tls == target->tls && !strcmp(route->failovers[index].host, target->host) &&
        !strcmp(route->failovers[index].port, target->port) && !strcmp(route->failovers[index].authority, target->authority)) {
      upstream_mark(worker, options, route, index + 1U, false);
      return;
    }
}

static bool gateway_append(proxy_gateway_parameters *parameters, const void *value, size_t length) {
  if (parameters->length > sizeof(parameters->data) - length) return false;
  memcpy(parameters->data + parameters->length, value, length);
  parameters->length += length;
  return true;
}

static bool gateway_fastcgi_length(proxy_gateway_parameters *parameters, size_t value) {
  unsigned char encoded[4U];
  if (value < 128U) {
    encoded[0] = (unsigned char)value;
    return gateway_append(parameters, encoded, 1U);
  }
  if (value > UINT32_C(0x7fffffff)) return false;
  encoded[0] = (unsigned char)((value >> 24U) | 0x80U);
  encoded[1] = (unsigned char)(value >> 16U);
  encoded[2] = (unsigned char)(value >> 8U);
  encoded[3] = (unsigned char)value;
  return gateway_append(parameters, encoded, sizeof(encoded));
}

static bool gateway_fastcgi_pair(proxy_gateway_parameters *parameters, const char *name, const char *value) {
  size_t name_length = strlen(name);
  size_t value_length = strlen(value);
  return gateway_fastcgi_length(parameters, name_length) && gateway_fastcgi_length(parameters, value_length) &&
         gateway_append(parameters, name, name_length) && gateway_append(parameters, value, value_length);
}

static bool gateway_short_pair(proxy_gateway_parameters *parameters, const char *name, const char *value, bool uwsgi) {
  unsigned char lengths[4U];
  size_t name_length = strlen(name);
  size_t value_length = strlen(value);
  if (name_length > UINT16_MAX || value_length > UINT16_MAX) return false;
  if (uwsgi) {
    lengths[0] = (unsigned char)name_length;
    lengths[1] = (unsigned char)(name_length >> 8U);
    lengths[2] = (unsigned char)value_length;
    lengths[3] = (unsigned char)(value_length >> 8U);
  } else {
    lengths[0] = (unsigned char)(name_length >> 8U);
    lengths[1] = (unsigned char)name_length;
    lengths[2] = (unsigned char)(value_length >> 8U);
    lengths[3] = (unsigned char)value_length;
  }
  return gateway_append(parameters, lengths, sizeof(lengths)) && gateway_append(parameters, name, name_length) &&
         gateway_append(parameters, value, value_length);
}

static bool gateway_scgi_pair(proxy_gateway_parameters *parameters, const char *name, const char *value) {
  return gateway_append(parameters, name, strlen(name) + 1U) && gateway_append(parameters, value, strlen(value) + 1U);
}

static bool gateway_parameters(proxy_gateway_parameters *parameters, const proxy_request *request, const laghu_proxy_upstream_target *target,
                               laghu_proxy_upstream_protocol protocol) {
  const proxy_header *content_type;
  const char *query = strchr(request->target, '?');
  char uri[LAGHU_RUNTIME_PATH_SIZE];
  char content_length[32U];
  char port[8U];
  const char *names[] = {"REQUEST_METHOD", "REQUEST_URI", "QUERY_STRING", "SERVER_PROTOCOL", "SERVER_NAME", "SERVER_PORT", "CONTENT_LENGTH"};
  const char *values[7U];
  size_t index;
  if (query == NULL)
    (void)snprintf(uri, sizeof(uri), "%s", request->target);
  else {
    size_t length = (size_t)(query - request->target);
    if (length >= sizeof(uri)) return false;
    memcpy(uri, request->target, length);
    uri[length] = '\0';
  }
  (void)snprintf(content_length, sizeof(content_length), "%zu", request->content_length);
  (void)snprintf(port, sizeof(port), "%s", target->port);
  values[0] = request->method;
  values[1] = uri;
  values[2] = query == NULL ? "" : query + 1U;
  values[3] = request->version;
  values[4] = target->host;
  values[5] = port;
  values[6] = content_length;
  content_type = proxy_find((proxy_header *)request->headers, request->header_count, "Content-Type");
  for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
    bool appended = protocol == LAGHU_PROXY_UPSTREAM_FASTCGI ? gateway_fastcgi_pair(parameters, names[index], values[index])
                    : protocol == LAGHU_PROXY_UPSTREAM_UWSGI ? gateway_short_pair(parameters, names[index], values[index], true)
                                                             : gateway_scgi_pair(parameters, names[index], values[index]);
    if (!appended) return false;
  }
  if (content_type != NULL) {
    if (protocol == LAGHU_PROXY_UPSTREAM_FASTCGI && !gateway_fastcgi_pair(parameters, "CONTENT_TYPE", content_type->value)) return false;
    if (protocol == LAGHU_PROXY_UPSTREAM_UWSGI && !gateway_short_pair(parameters, "CONTENT_TYPE", content_type->value, true)) return false;
    if (protocol == LAGHU_PROXY_UPSTREAM_SCGI && !gateway_scgi_pair(parameters, "CONTENT_TYPE", content_type->value)) return false;
  }
  if (protocol == LAGHU_PROXY_UPSTREAM_SCGI && !gateway_scgi_pair(parameters, "SCGI", "1")) return false;
  return parameters->length <= UINT16_MAX;
}

static bool gateway_fastcgi_record(laghu_socket socket, unsigned char type, unsigned short request_id, const unsigned char *data, size_t length) {
  unsigned char header[8U];
  if (length > UINT16_MAX) return false;
  header[0] = 1U;
  header[1] = type;
  header[2] = (unsigned char)(request_id >> 8U);
  header[3] = (unsigned char)request_id;
  header[4] = (unsigned char)(length >> 8U);
  header[5] = (unsigned char)length;
  header[6] = 0U;
  header[7] = 0U;
  return proxy_origin_send_all(socket, NULL, header, sizeof(header)) && (length == 0U || proxy_origin_send_all(socket, NULL, data, length));
}

static bool gateway_send_fastcgi(laghu_socket socket, const proxy_gateway_parameters *parameters, const unsigned char *body, size_t body_length) {
  static const unsigned char begin[] = {0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U};
  size_t offset;
  if (!gateway_fastcgi_record(socket, 1U, 1U, begin, sizeof(begin))) return false;
  for (offset = 0U; offset < parameters->length; offset += UINT16_MAX) {
    size_t length = parameters->length - offset;
    if (length > UINT16_MAX) length = UINT16_MAX;
    if (!gateway_fastcgi_record(socket, 4U, 1U, parameters->data + offset, length)) return false;
  }
  if (!gateway_fastcgi_record(socket, 4U, 1U, NULL, 0U)) return false;
  for (offset = 0U; offset < body_length; offset += UINT16_MAX) {
    size_t length = body_length - offset;
    if (length > UINT16_MAX) length = UINT16_MAX;
    if (!gateway_fastcgi_record(socket, 5U, 1U, body + offset, length)) return false;
  }
  return gateway_fastcgi_record(socket, 5U, 1U, NULL, 0U);
}

static bool gateway_send_uwsgi(laghu_socket socket, const proxy_gateway_parameters *parameters, const unsigned char *body, size_t body_length) {
  unsigned char header[4U] = {0U, (unsigned char)parameters->length, (unsigned char)(parameters->length >> 8U), 0U};
  return proxy_origin_send_all(socket, NULL, header, sizeof(header)) && proxy_origin_send_all(socket, NULL, parameters->data, parameters->length) &&
         (body_length == 0U || proxy_origin_send_all(socket, NULL, body, body_length));
}

static bool gateway_send_scgi(laghu_socket socket, const proxy_gateway_parameters *parameters, const unsigned char *body, size_t body_length) {
  char prefix[32U];
  int length = snprintf(prefix, sizeof(prefix), "%zu:", parameters->length);
  return length > 0 && (size_t)length < sizeof(prefix) && proxy_origin_send_all(socket, NULL, prefix, (size_t)length) &&
         proxy_origin_send_all(socket, NULL, parameters->data, parameters->length) && proxy_origin_send_all(socket, NULL, ",", 1U) &&
         (body_length == 0U || proxy_origin_send_all(socket, NULL, body, body_length));
}

static const unsigned char *gateway_header_end(const unsigned char *input, size_t length) {
  size_t index;
  if (input == NULL || length < 4U) return NULL;
  for (index = 0U; index + 3U < length; ++index)
    if (input[index] == '\r' && input[index + 1U] == '\n' && input[index + 2U] == '\r' && input[index + 3U] == '\n') return input + index;
  return NULL;
}

static bool gateway_response_from_cgi(const unsigned char *input, size_t length, proxy_response *response) {
  const unsigned char *separator;
  const unsigned char *cursor;
  char status[4U] = "200";
  char reason[128U] = "OK";
  size_t used;
  if (input == NULL || response == NULL || length < 4U || (separator = gateway_header_end(input, length)) == NULL) return false;
  used = (size_t)snprintf(response->storage, sizeof(response->storage), "HTTP/1.1 %s %s\r\n", status, reason);
  if (used >= sizeof(response->storage)) return false;
  cursor = input;
  while (cursor < separator) {
    const unsigned char *end = memchr(cursor, '\r', (size_t)((input + length) - cursor));
    size_t line_length;
    if (end == NULL || end + 1U >= input + length || end[1] != '\n') return false;
    line_length = (size_t)(end - cursor);
    if (line_length >= sizeof(response->storage) - used) return false;
    if (line_length >= 7U && !strncasecmp((const char *)cursor, "Status:", 7U)) {
      const char *value = (const char *)cursor + 7U;
      char *end_status = NULL;
      unsigned long parsed;
      char status_line[160U];
      size_t value_length = line_length - 7U;
      if (value_length >= sizeof(status_line)) return false;
      memcpy(status_line, value, value_length);
      status_line[value_length] = '\0';
      value = status_line;
      while (*value == ' ') ++value;
      parsed = strtoul(value, &end_status, 10);
      if (end_status == value || parsed < 100U || parsed > 599U) return false;
      (void)snprintf(status, sizeof(status), "%03lu", parsed);
      while (*end_status == ' ') ++end_status;
      if (*end_status != '\0' && strlen(end_status) < sizeof(reason)) (void)snprintf(reason, sizeof(reason), "%s", end_status);
      used = (size_t)snprintf(response->storage, sizeof(response->storage), "HTTP/1.1 %s %s\r\n", status, reason);
    } else {
      memcpy(response->storage + used, cursor, line_length);
      used += line_length;
      response->storage[used++] = '\r';
      response->storage[used++] = '\n';
      response->storage[used] = '\0';
    }
    cursor = end + 2U;
  }
  if (used + 2U >= sizeof(response->storage)) return false;
  response->storage[used++] = '\r';
  response->storage[used++] = '\n';
  response->storage[used] = '\0';
  return proxy_parse_response(response, used);
}

static bool gateway_read_cgi(laghu_socket socket, proxy_response *response, unsigned char **body, size_t *body_length) {
  char headers[LAGHU_PROXY_HEADER_BYTES + 1U];
  unsigned char *initial;
  size_t header_length;
  size_t initial_length;
  bool until_close;
  if (!proxy_read_headers(socket, headers, NULL, &header_length, &initial, &initial_length) ||
      !gateway_response_from_cgi((const unsigned char *)headers, header_length, response))
    return false;
  until_close = !response->has_content_length;
  return proxy_read_body(socket, NULL, initial, initial_length, response->has_content_length ? response->content_length : 0U, until_close, body,
                         body_length);
}

static bool gateway_read_exact(laghu_socket socket, void *output, size_t length) {
  unsigned char *cursor = output;
  while (length != 0U) {
    int read_count = proxy_origin_recv(socket, NULL, cursor, length > 65536U ? 65536U : length);
    if (read_count <= 0) return false;
    cursor += read_count;
    length -= (size_t)read_count;
  }
  return true;
}

static bool gateway_append_bytes(unsigned char **buffer, size_t *length, size_t *capacity, const unsigned char *data, size_t data_length) {
  size_t needed;
  unsigned char *replacement;
  if (data_length > LAGHU_PROXY_MAX_BODY - *length) return false;
  needed = *length + data_length;
  if (needed > *capacity) {
    size_t grown = *capacity == 0U ? 4096U : *capacity;
    while (grown < needed) {
      if (grown > LAGHU_PROXY_MAX_BODY / 2U) {
        grown = LAGHU_PROXY_MAX_BODY;
        break;
      }
      grown *= 2U;
    }
    replacement = realloc(*buffer, grown);
    if (replacement == NULL) return false;
    *buffer = replacement;
    *capacity = grown;
  }
  memcpy(*buffer + *length, data, data_length);
  *length += data_length;
  return true;
}

static bool gateway_read_fastcgi(laghu_socket socket, proxy_response *response, unsigned char **body, size_t *body_length) {
  unsigned char *stdout_data = NULL;
  size_t stdout_length = 0U, stdout_capacity = 0U;
  bool done = false;
  while (!done) {
    unsigned char header[8U];
    size_t content_length;
    unsigned char *content = NULL;
    if (!gateway_read_exact(socket, header, sizeof(header)) || header[0] != 1U || header[2] != 0U || header[3] != 1U) goto failed;
    content_length = ((size_t)header[4] << 8U) | header[5];
    if (content_length != 0U && (content = malloc(content_length)) == NULL) goto failed;
    if (content_length != 0U && !gateway_read_exact(socket, content, content_length)) {
      free(content);
      goto failed;
    }
    if (header[6] != 0U) {
      unsigned char padding[255U];
      if (!gateway_read_exact(socket, padding, header[6])) {
        free(content);
        goto failed;
      }
    }
    if (header[1] == 6U && !gateway_append_bytes(&stdout_data, &stdout_length, &stdout_capacity, content, content_length)) {
      free(content);
      goto failed;
    }
    if (header[1] == 3U) done = true;
    free(content);
  }
  if (!gateway_response_from_cgi(stdout_data, stdout_length, response)) goto failed;
  {
    const unsigned char *separator = gateway_header_end(stdout_data, stdout_length);
    if (separator == NULL) goto failed;
    size_t offset = (size_t)(separator - stdout_data) + 4U;
    size_t length = stdout_length - offset;
    *body = malloc(length == 0U ? 1U : length);
    if (*body == NULL) goto failed;
    if (length != 0U) memcpy(*body, stdout_data + offset, length);
    *body_length = length;
  }
  free(stdout_data);
  return true;
failed:
  free(stdout_data);
  return false;
}

bool proxy_gateway_fetch(proxy_worker *worker, const laghu_proxy_options *options, const laghu_proxy_upstream_target *target,
                         laghu_proxy_upstream_protocol protocol, const proxy_request *request, const unsigned char *request_body,
                         size_t request_body_length, proxy_response *response, unsigned char **body, size_t *body_length, bool *timed_out) {
  proxy_gateway_parameters parameters = {0};
  laghu_socket socket = LAGHU_INVALID_SOCKET;
  bool result = false;
  if (worker == NULL || target == NULL || request == NULL || response == NULL || body == NULL || body_length == NULL || timed_out == NULL ||
      target->tls)
    return false;
  *timed_out = false;
  *body = NULL;
  *body_length = 0U;
  if (!gateway_parameters(&parameters, request, target, protocol)) return false;
  socket = proxy_connect(worker, target->host, target->port, options->connect_timeout);
  if (socket == LAGHU_INVALID_SOCKET) {
    *timed_out = proxy_socket_timed_out();
    return false;
  }
  proxy_timeout(socket, options->io_timeout);
  if ((protocol == LAGHU_PROXY_UPSTREAM_FASTCGI && !gateway_send_fastcgi(socket, &parameters, request_body, request_body_length)) ||
      (protocol == LAGHU_PROXY_UPSTREAM_UWSGI && !gateway_send_uwsgi(socket, &parameters, request_body, request_body_length)) ||
      (protocol == LAGHU_PROXY_UPSTREAM_SCGI && !gateway_send_scgi(socket, &parameters, request_body, request_body_length)))
    goto done;
  result = protocol == LAGHU_PROXY_UPSTREAM_FASTCGI ? gateway_read_fastcgi(socket, response, body, body_length)
                                                    : gateway_read_cgi(socket, response, body, body_length);
done:
  *timed_out = !result && proxy_socket_timed_out();
  laghu_close(socket);
  proxy_worker_origin(worker, LAGHU_INVALID_SOCKET);
  if (!result) {
    free(*body);
    *body = NULL;
    *body_length = 0U;
  }
  return result;
}

bool proxy_route_gateway_fetch(proxy_worker *worker, const laghu_proxy_options *options, const laghu_proxy_route *route, const proxy_request *request,
                               const unsigned char *request_body, size_t request_body_length, proxy_response *response, unsigned char **body,
                               size_t *body_length, laghu_proxy_upstream_target *selected, unsigned int *failovers, bool *timed_out) {
  size_t index;
  bool attempt_timed_out = false;
  bool retryable;
  if (worker == NULL || options == NULL || route == NULL || selected == NULL || failovers == NULL || timed_out == NULL) return false;
  *failovers = 0U;
  *timed_out = false;
  retryable = !strcmp(request->method, "GET") || !strcmp(request->method, "HEAD") || !strcmp(request->method, "OPTIONS");
  for (index = 0U; index <= route->failover_count; ++index) {
    if (!upstream_available(worker, options, route, index)) {
      if (!retryable || index == route->failover_count) break;
      ++*failovers;
      continue;
    }
    if (index == 0U)
      upstream_primary(route, selected);
    else
      *selected = route->failovers[index - 1U];
    if (proxy_gateway_fetch(worker, options, selected, route->upstream_protocol, request, request_body, request_body_length, response, body,
                            body_length, &attempt_timed_out)) {
      upstream_mark(worker, options, route, index, true);
      return true;
    }
    upstream_mark(worker, options, route, index, false);
    *timed_out = *timed_out || attempt_timed_out;
    if (!retryable || index == route->failover_count) break;
    ++*failovers;
  }
  return false;
}

bool proxy_options_has_tls_upstream(const laghu_proxy_options *options) {
  size_t index;
  if (options == NULL) return false;
  if (options->origin_tls) return true;
  for (index = 0U; index < options->route_count; ++index) {
    size_t target;
    if (options->routes[index].upstream_tls) return true;
    for (target = 0U; target < options->routes[index].failover_count; ++target)
      if (options->routes[index].failovers[target].tls) return true;
  }
  return false;
}

static bool upstream_probe(proxy_queue *queue, const laghu_proxy_upstream_target *target, const laghu_proxy_route *route) {
  proxy_worker worker = {.queue = queue, .active_client = LAGHU_INVALID_SOCKET, .active_origin = LAGHU_INVALID_SOCKET};
  laghu_socket socket;
  SSL *tls = NULL;
  bool timed_out = false;
  bool healthy = false;
  socket = proxy_connect(&worker, target->host, target->port, 1U);
  if (socket == LAGHU_INVALID_SOCKET) return false;
  proxy_timeout(socket, 1U);
  if (target->tls && (tls = proxy_tls_handshake(&worker, socket, target->host, 1U, &timed_out)) == NULL) goto done;
  if (route->upstream_protocol != LAGHU_PROXY_UPSTREAM_HTTP) {
    healthy = true;
  } else {
    char request[1024U];
    char response[LAGHU_PROXY_HEADER_BYTES + 1U];
    unsigned char *initial;
    size_t header_length, initial_length;
    int length =
        snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", route->health_path, target->authority);
    if (length > 0 && (size_t)length < sizeof(request) && proxy_origin_send_all(socket, tls, request, (size_t)length) &&
        proxy_read_headers(socket, response, tls, &header_length, &initial, &initial_length)) {
      proxy_response parsed = {0};
      memcpy(parsed.storage, response, header_length + 1U);
      healthy = proxy_parse_response(&parsed, header_length) && parsed.status >= 200U && parsed.status < 400U;
    }
  }
done:
  SSL_free(tls);
  laghu_close(socket);
  proxy_worker_origin(&worker, LAGHU_INVALID_SOCKET);
  return healthy;
}

void proxy_maintain_upstream_health(proxy_queue *queue) {
  const laghu_proxy_options *options;
  uint64_t now;
  size_t route_index;
  if (queue == NULL) return;
  options = proxy_options_acquire(queue);
  now = proxy_monotonic_ms();
  for (route_index = 0U; route_index < options->route_count; ++route_index) {
    const laghu_proxy_route *route = &options->routes[route_index];
    size_t target_index;
    if (route->health_path[0] == '\0') continue;
    for (target_index = 0U; target_index <= route->failover_count; ++target_index) {
      proxy_upstream_health *state = &queue->upstream_health[route_index][target_index];
      laghu_proxy_upstream_target target;
      bool due;
      proxy_queue_lock(queue);
      due = now >= state->next_probe_ms;
      if (due) state->next_probe_ms = now + (uint64_t)route->health_interval * 1000U;
      proxy_queue_unlock(queue);
      if (!due) continue;
      if (target_index == 0U)
        upstream_primary(route, &target);
      else
        target = route->failovers[target_index - 1U];
      {
        bool healthy = upstream_probe(queue, &target, route);
        proxy_queue_lock(queue);
        state->known = true;
        state->healthy = healthy;
        proxy_queue_unlock(queue);
      }
    }
  }
  proxy_options_release(queue);
}
