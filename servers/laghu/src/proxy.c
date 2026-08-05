// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/proxy.h"

#include <ctype.h>
#include <errno.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "laghu/catalog.h"
#include "laghu/config.h"
#include "server_internal.h"

void proxy_queue_lock(proxy_queue *queue) {
#ifdef _WIN32
  EnterCriticalSection(&queue->lock);
#else
  pthread_mutex_lock(&queue->lock);
#endif
}

void proxy_queue_unlock(proxy_queue *queue) {
#ifdef _WIN32
  LeaveCriticalSection(&queue->lock);
#else
  pthread_mutex_unlock(&queue->lock);
#endif
}

void proxy_worker_origin(proxy_worker *worker, laghu_socket origin) {
  proxy_queue_lock(worker->queue);
  worker->active_origin = origin;
  proxy_queue_unlock(worker->queue);
}

bool proxy_is_forcing(proxy_queue *queue) {
  bool forcing;
  proxy_queue_lock(queue);
  forcing = queue->state == PROXY_FORCING;
  proxy_queue_unlock(queue);
  return forcing;
}

void proxy_handle(const proxy_connection *connection, proxy_worker *worker) {
  laghu_socket client = connection->socket;
  const laghu_proxy_options *options = worker->queue->options;
  proxy_request request;
  proxy_response response;
  unsigned char *initial;
  size_t initial_length, header_length;
  unsigned char *request_body = NULL, *origin_body = NULL, *decoded = NULL;
  size_t request_body_length = 0U, origin_body_length = 0U;
  laghu_socket origin = LAGHU_INVALID_SOCKET;
  SSL *origin_tls = NULL;
  char outbound[LAGHU_PROXY_HEADER_BYTES + 1U];
  size_t outbound_length = 0U, index;
  laghu_http_header request_headers[LAGHU_HTTP_MAX_REQUEST_HEADERS];
  laghu_http_header response_headers[LAGHU_HTTP_MAX_RESPONSE_HEADERS];
  laghu_http_request normalized_request;
  laghu_http_response normalized_response;
  laghu_http_environment environment;
  laghu_http_transaction transaction = {0};
  laghu_http_transaction_result prepared, finalized;
  proxy_access_log access;
  bool prepared_ok = false;
  bool tls_timed_out = false;
  proxy_header *request_host = NULL;
  memset(&request, 0, sizeof(request));
  memset(&response, 0, sizeof(response));
  memset(&prepared, 0, sizeof(prepared));
  memset(&finalized, 0, sizeof(finalized));
  proxy_access_init(&access, worker->queue);
#define PROXY_FAIL(code, reason, category)        \
  do {                                            \
    access.status = (code);                       \
    access.failure = (category);                  \
    proxy_error_response(client, (code), reason); \
  } while (0)
  proxy_timeout(client, options->io_timeout);
  if (!proxy_read_headers(client, request.storage, NULL, &header_length,
                          &initial, &initial_length) ||
      !proxy_parse_request(&request, header_length)) {
    PROXY_FAIL(400U, "Bad Request", "client_parse");
    goto done;
  }
  (void)snprintf(access.method, sizeof(access.method), "%s", request.method);
  {
    const char *query = strchr(request.target, '?');
    size_t length = query == NULL ? strlen(request.target)
                                  : (size_t)(query - request.target);
    if (length >= sizeof(access.path)) length = sizeof(access.path) - 1U;
    memcpy(access.path, request.target, length);
    access.path[length] = '\0';
  }
  access.input_bytes = header_length + request.content_length;
  request_host = proxy_find(request.headers, request.header_count, "Host");
  proxy_poll_flush_file(options);
  if (!strcmp(request.method, "CONNECT") || !strcmp(request.method, "TRACE")) {
    PROXY_FAIL(405U, "Method Not Allowed", "request_limit");
    goto done;
  }
  if (request.expect) {
    PROXY_FAIL(417U, "Expectation Failed", "request_limit");
    goto done;
  }
  if (request.upgrade) {
    PROXY_FAIL(400U, "Bad Request", "client_parse");
    goto done;
  }
  if (request.chunked) {
    PROXY_FAIL(501U, "Not Implemented", "request_limit");
    goto done;
  }
  if (request.content_length > LAGHU_PROXY_REQUEST_BODY_BYTES) {
    PROXY_FAIL(413U, "Payload Too Large", "request_limit");
    goto done;
  }
  if (request.content_length != 0U &&
      !proxy_read_body(client, NULL, initial, initial_length,
                       request.content_length, false, &request_body,
                       &request_body_length)) {
    PROXY_FAIL(400U, "Bad Request", "client_parse");
    goto done;
  }
  if (proxy_handle_administrative_routes(connection, worker, &request, &access))
    goto done;
  if (!strncmp(request.target, "/.laghu/", 8U)) {
    if (proxy_handle_beacon_routes(connection, worker, &request, request_body,
                                   request_body_length, &access))
      goto done;
    if (strcmp(request.method, "GET") != 0 &&
        strcmp(request.method, "HEAD") != 0) {
      PROXY_FAIL(405U, "Method Not Allowed", "request_limit");
      goto done;
    }
    memset(&normalized_request, 0, sizeof(normalized_request));
    memset(&normalized_response, 0, sizeof(normalized_response));
    memset(&environment, 0, sizeof(environment));
    {
      const char *scheme =
          proxy_effective_scheme(options, connection, &request);
      normalized_request = (laghu_http_request){
          LAGHU_HTTP_ABI_VERSION,
          sizeof(normalized_request),
          {(const unsigned char *)request.method, strlen(request.method)},
          {(const unsigned char *)scheme, strlen(scheme)},
          {NULL, 0U},
          {(const unsigned char *)request.target, strlen(request.target)},
          NULL,
          0U};
    }
    {
      proxy_header *host =
          proxy_find(request.headers, request.header_count, "Host");
      normalized_request.authority = (laghu_buffer){
          (const unsigned char *)(host != NULL ? host->value
                                               : options->listen_host),
          strlen(host != NULL ? host->value : options->listen_host)};
    }
    normalized_response = (laghu_http_response){LAGHU_HTTP_ABI_VERSION,
                                                sizeof(normalized_response),
                                                200U,
                                                NULL,
                                                0U,
                                                0U,
                                                false,
                                                true,
                                                false,
                                                {NULL, 0U}};
    environment = (laghu_http_environment){
        .version = LAGHU_HTTP_ABI_VERSION,
        .struct_size = sizeof(environment),
        .config = options->config,
        .cache_path = options->cache_path,
        .asset_offload =
            options->asset_offload_loaded ? &options->asset_offload : NULL,
        .rum = worker->queue->rum,
        .worker_queue_path = options->worker_queue_path,
        .queue = &worker->runtime_queue,
        .font_fetch_queue_path = options->font_providers_loaded
                                     ? options->font_fetch_queue_path
                                     : NULL,
        .font_fetch_queue =
            options->font_providers_loaded ? &worker->font_fetch_queue : NULL,
        .font_providers =
            options->font_providers_loaded ? &options->font_providers : NULL,
        .javascript_queue_path = options->javascript_queue_enabled
                                     ? options->javascript_queue_path
                                     : NULL,
        .javascript_queue = options->javascript_queue_enabled
                                ? &worker->javascript_queue
                                : NULL,
        .javascript_target = options->javascript_target,
        .javascript_observations = options->javascript_observations_loaded
                                       ? &options->javascript_observations
                                       : NULL,
        .javascript_defer = options->javascript_defer_loaded
                                ? &options->javascript_defer
                                : NULL,
        .now = (uint64_t)time(NULL)};
    laghu_http_transaction_init(&transaction);
    if (laghu_http_transaction_prepare(&transaction, &normalized_request,
                                       &normalized_response, &environment,
                                       &prepared) &&
        prepared.action == LAGHU_HTTP_ACTION_SERVE_CACHED) {
      response.status = 200U;
      (void)laghu_base_string_copy(response.reason, sizeof(response.reason),
                                   "OK");
      if (!proxy_send_result(client, &response, &prepared, prepared.selected))
        access.failure = "client_disconnect";
      access.cache_state = "immutable";
      access.output_bytes = prepared.selected.length;
    } else
      PROXY_FAIL(404U, "Not Found", "none");
    laghu_http_transaction_result_release(&prepared);
    goto done;
  }
  origin = proxy_connect(worker, options->origin_host, options->origin_port,
                         options->connect_timeout);
  if (origin == LAGHU_INVALID_SOCKET) {
    PROXY_FAIL(502U, "Bad Gateway", "origin_connect");
    goto done;
  }
  proxy_worker_origin(worker, origin);
  proxy_timeout(origin, options->io_timeout);
  if (options->origin_tls) {
    origin_tls = proxy_tls_handshake(worker, origin, options->origin_host,
                                     options->connect_timeout, &tls_timed_out);
    if (origin_tls == NULL) {
      PROXY_FAIL(502U, "Bad Gateway",
                 tls_timed_out ? "origin_timeout" : "origin_tls");
      goto done;
    }
  }
  outbound_length = (size_t)snprintf(
      outbound, sizeof(outbound),
      "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n", request.method,
      request.target, options->origin_authority);
  for (index = 0U; index < request.header_count; ++index) {
    int n;
    if (proxy_hop(request.headers[index].name) ||
        proxy_connection_nominates(request.headers, request.header_count,
                                   request.headers[index].name) ||
        proxy_forwarding_name(request.headers[index].name) ||
        proxy_name_equal(request.headers[index].name, "Host") ||
        proxy_name_equal(request.headers[index].name, "Content-Length"))
      continue;
    n = snprintf(outbound + outbound_length, sizeof(outbound) - outbound_length,
                 "%s: %s\r\n", request.headers[index].name,
                 request.headers[index].value);
    if (n <= 0 || (size_t)n >= sizeof(outbound) - outbound_length) {
      PROXY_FAIL(400U, "Bad Request", "request_limit");
      goto done;
    }
    outbound_length += (size_t)n;
  }
  if (request_host == NULL ||
      !proxy_append_forwarding(options, connection, &request,
                               request_host->value, outbound, sizeof(outbound),
                               &outbound_length)) {
    PROXY_FAIL(400U, "Bad Request", "client_parse");
    goto done;
  }
  if (request.content_length != 0U)
    outbound_length += (size_t)snprintf(
        outbound + outbound_length, sizeof(outbound) - outbound_length,
        "Content-Length: %zu\r\n", request.content_length);
  if (outbound_length + 2U >= sizeof(outbound)) {
    PROXY_FAIL(400U, "Bad Request", "request_limit");
    goto done;
  }
  memcpy(outbound + outbound_length, "\r\n", 2U);
  outbound_length += 2U;
  if (!proxy_origin_send_all(origin, origin_tls, outbound, outbound_length) ||
      (request_body_length &&
       !proxy_origin_send_all(origin, origin_tls, request_body,
                              request_body_length)) ||
      !proxy_read_headers(origin, response.storage, origin_tls, &header_length,
                          &initial, &initial_length) ||
      !proxy_parse_response(&response, header_length)) {
    PROXY_FAIL(502U, "Bad Gateway",
               proxy_socket_timed_out()
                   ? "origin_timeout"
                   : (origin_tls != NULL ? "origin_tls" : "origin_protocol"));
    goto done;
  }
  for (index = 0U; index < request.header_count; ++index)
    request_headers[index] =
        (laghu_http_header){{(unsigned char *)request.headers[index].name,
                             strlen(request.headers[index].name)},
                            {(unsigned char *)request.headers[index].value,
                             strlen(request.headers[index].value)}};
  for (index = 0U; index < response.header_count; ++index)
    response_headers[index] =
        (laghu_http_header){{(unsigned char *)response.headers[index].name,
                             strlen(response.headers[index].name)},
                            {(unsigned char *)response.headers[index].value,
                             strlen(response.headers[index].value)}};
  {
    const char *scheme = proxy_effective_scheme(options, connection, &request);
    normalized_request = (laghu_http_request){
        LAGHU_HTTP_ABI_VERSION,
        sizeof(normalized_request),
        {(unsigned char *)request.method, strlen(request.method)},
        {(unsigned char *)scheme, strlen(scheme)},
        {NULL, 0U},
        {(unsigned char *)request.target, strlen(request.target)},
        request_headers,
        request.header_count};
  }
  {
    proxy_header *host =
        proxy_find(request.headers, request.header_count, "Host");
    normalized_request.authority = (laghu_buffer){
        (unsigned char *)(host != NULL ? host->value : options->listen_host),
        strlen(host != NULL ? host->value : options->listen_host)};
  }
  normalized_response = (laghu_http_response){
      LAGHU_HTTP_ABI_VERSION,
      sizeof(normalized_response),
      response.status,
      response_headers,
      response.header_count,
      response.content_length,
      response.has_content_length,
      true,
      response.status == 206U ||
          proxy_find(response.headers, response.header_count,
                     "Content-Range") != NULL,
      {NULL, 0U}};
  environment = (laghu_http_environment){
      .version = LAGHU_HTTP_ABI_VERSION,
      .struct_size = sizeof(environment),
      .config = options->config,
      .cache_path = options->cache_path,
      .asset_offload =
          options->asset_offload_loaded ? &options->asset_offload : NULL,
      .rum = worker->queue->rum,
      .worker_queue_path = options->worker_queue_path,
      .queue = &worker->runtime_queue,
      .font_fetch_queue_path = options->font_providers_loaded
                                   ? options->font_fetch_queue_path
                                   : NULL,
      .font_fetch_queue =
          options->font_providers_loaded ? &worker->font_fetch_queue : NULL,
      .font_providers =
          options->font_providers_loaded ? &options->font_providers : NULL,
      .javascript_queue_path = options->javascript_queue_enabled
                                   ? options->javascript_queue_path
                                   : NULL,
      .javascript_queue =
          options->javascript_queue_enabled ? &worker->javascript_queue : NULL,
      .javascript_target = options->javascript_target,
      .javascript_observations = options->javascript_observations_loaded
                                     ? &options->javascript_observations
                                     : NULL,
      .javascript_defer =
          options->javascript_defer_loaded ? &options->javascript_defer : NULL,
      .now = (uint64_t)time(NULL)};
  if (!response.chunked) {
    bool bodyless = !strcmp(request.method, "HEAD") ||
                    (response.status >= 100U && response.status < 200U) ||
                    response.status == 204U || response.status == 304U;
    laghu_http_transaction_init(&transaction);
    prepared_ok = laghu_http_transaction_prepare(
        &transaction, &normalized_request, &normalized_response, &environment,
        &prepared);
    if (!prepared_ok) {
      PROXY_FAIL(502U, "Bad Gateway", "transform");
      goto done;
    }
    if (prepared.action == LAGHU_HTTP_ACTION_SERVE_CACHED) {
      if (!proxy_send_result(client, &response, &prepared, prepared.selected))
        access.failure = "client_disconnect";
      goto done;
    }
    if (prepared.action == LAGHU_HTTP_ACTION_BYPASS) {
      size_t expected = bodyless ? 0U : response.content_length;
      bool until_close = !bodyless && !response.has_content_length;
      if (!proxy_send_headers(client, &response, &prepared,
                              response.content_length,
                              response.has_content_length) ||
          !proxy_stream_body(origin, origin_tls, client, initial,
                             bodyless ? 0U : initial_length, expected,
                             until_close)) {
        access.failure = "client_disconnect";
        goto done;
      }
      goto done;
    }
  }
  {
    bool bodyless = !strcmp(request.method, "HEAD") ||
                    (response.status >= 100U && response.status < 200U) ||
                    response.status == 204U || response.status == 304U;
    if (!proxy_read_body(
            origin, origin_tls, initial, initial_length,
            bodyless
                ? 0U
                : (response.has_content_length ? response.content_length : 0U),
            !bodyless && !response.has_content_length, &origin_body,
            &origin_body_length)) {
      PROXY_FAIL(502U, "Bad Gateway",
                 proxy_socket_timed_out()
                     ? "origin_timeout"
                     : (origin_tls != NULL ? "origin_tls" : "origin_protocol"));
      goto done;
    }
  }
  if (response.chunked) {
    size_t decoded_length = 0U;
    decoded = malloc(LAGHU_PROXY_MAX_BODY);
    if (decoded == NULL ||
        !laghu_proxy_decode_chunked(
            (laghu_buffer){origin_body, origin_body_length}, decoded,
            LAGHU_PROXY_MAX_BODY, &decoded_length)) {
      PROXY_FAIL(502U, "Bad Gateway", "origin_protocol");
      goto done;
    }
    free(origin_body);
    origin_body = decoded;
    decoded = NULL;
    origin_body_length = decoded_length;
  }
  if (!prepared_ok) {
    normalized_response.declared_length = origin_body_length;
    normalized_response.has_declared_length = true;
    laghu_http_transaction_init(&transaction);
    prepared_ok = laghu_http_transaction_prepare(
        &transaction, &normalized_request, &normalized_response, &environment,
        &prepared);
    if (!prepared_ok) {
      PROXY_FAIL(502U, "Bad Gateway", "transform");
      goto done;
    }
  }
  if (prepared.action == LAGHU_HTTP_ACTION_SERVE_CACHED) {
    if (!proxy_send_result(client, &response, &prepared, prepared.selected))
      access.failure = "client_disconnect";
  } else if (prepared.action == LAGHU_HTTP_ACTION_BYPASS) {
    if (!proxy_send_result(client, &response, &prepared,
                           (laghu_buffer){origin_body, origin_body_length}))
      access.failure = "client_disconnect";
  } else if (!proxy_is_forcing(worker->queue) &&
             laghu_http_transaction_finalize(
                 &transaction, (laghu_buffer){origin_body, origin_body_length},
                 &finalized)) {
    laghu_operational_registry_budget(
        &worker->queue->operational, &transaction.budget,
        transaction.environment.config.transform_deadline_ms);
    laghu_operational_registry_lcp(
        &worker->queue->operational, finalized.lcp_decision,
        finalized.lcp_applied, finalized.lcp_profile_observations,
        finalized.lcp_profile_ready);
    if (!proxy_send_result(client, &response, &finalized, finalized.selected))
      access.failure = "client_disconnect";
  } else {
    if (prepared_ok)
      laghu_operational_registry_budget(
          &worker->queue->operational, &transaction.budget,
          transaction.environment.config.transform_deadline_ms);
    if (!proxy_send_result(client, &response, &finalized,
                           (laghu_buffer){origin_body, origin_body_length}))
      access.failure = "client_disconnect";
  }
done:
  if (access.status == 0U)
    access.status = response.status != 0U ? response.status : 200U;
  if (finalized.version == LAGHU_HTTP_ABI_VERSION) {
    access.decision = finalized.decision;
    access.job_published = finalized.job_published;
    access.output_bytes = finalized.selected.length;
    access.cache_state = "cold";
    access.javascript_defer_recommended =
        finalized.javascript_defer_recommended;
    access.javascript_defer_rollback_recommended =
        finalized.javascript_defer_rollback_recommended;
    (void)snprintf(access.javascript_defer_path,
                   sizeof(access.javascript_defer_path), "%s",
                   finalized.javascript_defer_path);
    (void)snprintf(access.javascript_defer_template,
                   sizeof(access.javascript_defer_template), "%s",
                   finalized.javascript_defer_template);
    access.javascript_defer_bucket = finalized.javascript_defer_bucket;
    access.javascript_defer_observations =
        finalized.javascript_defer_observations;
  } else if (prepared.version == LAGHU_HTTP_ABI_VERSION) {
    access.decision = prepared.decision;
    access.job_published = prepared.job_published;
    if (prepared.action == LAGHU_HTTP_ACTION_SERVE_CACHED) {
      access.cache_state = "warm";
      access.output_bytes = prepared.selected.length;
    } else if (prepared.action != LAGHU_HTTP_ACTION_BYPASS) {
      access.cache_state = "cold";
    }
  }
  access.original_response_bytes = origin_body_length;
  if (access.output_bytes == 0U && origin_body_length != 0U)
    access.output_bytes = origin_body_length;
  if (proxy_is_forcing(worker->queue)) access.failure = "shutdown";
  proxy_access_write(worker->queue, &access);
  laghu_http_transaction_result_release(&prepared);
  laghu_http_transaction_result_release(&finalized);
  free(request_body);
  free(origin_body);
  if (origin_tls != NULL) SSL_free(origin_tls);
  free(decoded);
  if (origin != LAGHU_INVALID_SOCKET) {
    proxy_worker_origin(worker, LAGHU_INVALID_SOCKET);
    laghu_close(origin);
  }
  laghu_close(client);
#undef PROXY_FAIL
}
