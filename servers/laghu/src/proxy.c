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
#include "laghu/html_cache.h"
#include "server_internal.h"

void proxy_queue_lock(proxy_queue *queue) { pthread_mutex_lock(&queue->lock); }

void proxy_queue_unlock(proxy_queue *queue) { pthread_mutex_unlock(&queue->lock); }

static uint32_t proxy_runtime_queue_capabilities(proxy_worker *worker) {
  laghu_runtime_queue_snapshot snapshot;
  laghu_runtime_queue *queue = proxy_runtime_queue(worker);
  uint64_t now = (uint64_t)time(NULL);
  if (queue == NULL || !laghu_runtime_queue_snapshot_get(queue, &snapshot) || snapshot.capabilities == 0U ||
      snapshot.worker_heartbeat == 0U || snapshot.worker_heartbeat > now || now - snapshot.worker_heartbeat > 45U)
    return 0U;
  return snapshot.capabilities;
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

static bool proxy_html_cache_request_eligible(const laghu_proxy_options *options, const proxy_request *request) {
  return options != NULL && request != NULL && options->config.mode == LAGHU_MODE_ON &&
         options->config.html_cache_ttl != LAGHU_HTML_CACHE_TTL_UNSET && options->config.html_cache_origin[0] != '\0' &&
         strcmp(request->method, "GET") == 0 && request->target[0] == '/' && strncmp(request->target, "/.laghu/", sizeof("/.laghu/") - 1U) != 0 &&
         proxy_find((proxy_header *)request->headers, request->header_count, "Authorization") == NULL &&
         proxy_find((proxy_header *)request->headers, request->header_count, "Cookie") == NULL;
}

static bool proxy_html_cache_mark_hit(laghu_http_transaction_result *result) {
  laghu_http_header_operation *operation = NULL;
  char *value;
  size_t index;
  if (result == NULL) return false;
  for (index = 0U; index < result->header_operation_count; ++index) {
    if (proxy_name_equal(result->header_operations[index].name, "X-Laghu-Cache")) {
      operation = &result->header_operations[index];
      break;
    }
  }
  if (operation == NULL) {
    if (result->header_operation_count == LAGHU_HTTP_MAX_HEADER_OPERATIONS) return false;
    operation = &result->header_operations[result->header_operation_count++];
    memset(operation, 0, sizeof(*operation));
    (void)snprintf(operation->name, sizeof(operation->name), "%s", "X-Laghu-Cache");
  }
  value = malloc(sizeof("hit"));
  if (value == NULL) return false;
  memcpy(value, "hit", sizeof("hit"));
  free(operation->value);
  operation->kind = LAGHU_HTTP_HEADER_SET;
  operation->value = value;
  operation->early_hint = false;
  return true;
}

static bool proxy_materialize_cached_result(laghu_http_transaction_result *result) {
  unsigned char *body;
  if (result == NULL || result->action != LAGHU_HTTP_ACTION_SERVE_CACHED || result->selected.data != NULL) return true;
  if (!result->cached_file || result->selected.length == 0U) return false;
  body = malloc(result->selected.length);
  if (body == NULL || !laghu_runtime_cache_read(&result->cached_entry, body, result->selected.length)) {
    free(body);
    return false;
  }
  result->owned_body = body;
  result->selected = (laghu_buffer){body, result->selected.length};
  return true;
}

static bool proxy_html_cache_token_equal(const char *value, size_t length, const char *expected) {
  size_t index;
  if (strlen(expected) != length) return false;
  for (index = 0U; index < length; ++index)
    if (tolower((unsigned char)value[index]) != tolower((unsigned char)expected[index])) return false;
  return true;
}

static bool proxy_html_cache_control_prohibits(const char *value) {
  const char *cursor = value;
  while (cursor != NULL && *cursor != '\0') {
    const char *end = strchr(cursor, ',');
    const char *token_end = end == NULL ? cursor + strlen(cursor) : end;
    const char *equals;
    while (cursor < token_end && (*cursor == ' ' || *cursor == '\t')) ++cursor;
    equals = memchr(cursor, '=', (size_t)(token_end - cursor));
    if (equals != NULL) token_end = equals;
    while (token_end > cursor && (token_end[-1] == ' ' || token_end[-1] == '\t')) --token_end;
    if (proxy_html_cache_token_equal(cursor, (size_t)(token_end - cursor), "private")) return true;
    if (proxy_html_cache_token_equal(cursor, (size_t)(token_end - cursor), "no-store")) return true;
    if (proxy_html_cache_token_equal(cursor, (size_t)(token_end - cursor), "no-cache")) return true;
    cursor = end == NULL ? NULL : end + 1U;
  }
  return false;
}

static bool proxy_html_cache_content_type(const char *value) {
  size_t length;
  if (value == NULL) return false;
  length = strlen(value);
  while (length != 0U && (*value == ' ' || *value == '\t')) {
    ++value;
    --length;
  }
  return length >= sizeof("text/html") - 1U && proxy_html_cache_token_equal(value, sizeof("text/html") - 1U, "text/html") &&
         (length == sizeof("text/html") - 1U || value[sizeof("text/html") - 1U] == ';' || value[sizeof("text/html") - 1U] == ' ' ||
          value[sizeof("text/html") - 1U] == '\t');
}

static bool proxy_html_cache_response_eligible(const laghu_proxy_options *options, const proxy_request *request, const proxy_response *response,
                                               size_t body_length) {
  size_t index;
  proxy_header *content_type;
  if (!proxy_html_cache_request_eligible(options, request) || response == NULL || response->status != 200U || body_length == 0U ||
      proxy_find((proxy_header *)response->headers, response->header_count, "Set-Cookie") != NULL ||
      proxy_find((proxy_header *)response->headers, response->header_count, "Vary") != NULL ||
      proxy_find((proxy_header *)response->headers, response->header_count, "Content-Encoding") != NULL) {
    return false;
  }
  content_type = proxy_find((proxy_header *)response->headers, response->header_count, "Content-Type");
  if (content_type == NULL || !proxy_html_cache_content_type(content_type->value)) {
    return false;
  }
  for (index = 0U; index < response->header_count; ++index)
    if (proxy_name_equal(response->headers[index].name, "Cache-Control") && proxy_html_cache_control_prohibits(response->headers[index].value))
      return false;
  return true;
}

static void proxy_html_cache_enqueue_refresh(proxy_worker *worker, const char *request_path, const laghu_html_cache_record *record) {
  laghu_runtime_job job = {0};
  proxy_queue *queue;
  uint64_t now;
  unsigned int index, candidate = 0U;
  if (worker == NULL || request_path == NULL || record == NULL || strlen(request_path) >= sizeof(job.request_path) ||
      !laghu_html_cache_key(worker->queue->options->config.html_cache_origin, request_path, job.index_key)) {
    return;
  }
  job.kind = LAGHU_RUNTIME_JOB_HTML_REFRESH;
  (void)snprintf(job.request_path, sizeof(job.request_path), "%s", request_path);
  memcpy(job.policy_key, job.index_key, sizeof(job.policy_key));
  (void)snprintf(job.validator, sizeof(job.validator), "%s", record->entry.validator);
  now = (uint64_t)time(NULL);
  queue = worker->queue;
  proxy_queue_lock(queue);
  if (!queue->html_refresh_queue_ready) {
    proxy_queue_unlock(queue);
    return;
  }
  for (index = 0U; index < LAGHU_PROXY_HTML_REFRESH_DEDUP; ++index) {
    if (queue->html_refresh_until[index] > now && strcmp(queue->html_refresh_keys[index], job.index_key) == 0) {
      proxy_queue_unlock(queue);
      return;
    }
    if (queue->html_refresh_until[index] < queue->html_refresh_until[candidate]) candidate = index;
  }
  if (laghu_runtime_queue_try_publish(&queue->html_refresh_queue, &job)) {
    memcpy(queue->html_refresh_keys[candidate], job.index_key, sizeof(job.index_key));
    queue->html_refresh_until[candidate] = now + 1U;
  }
  proxy_queue_unlock(queue);
}

void proxy_handle(const proxy_connection *connection, proxy_worker *worker) {
  laghu_socket client = connection->socket;
  const laghu_proxy_options *options = worker->queue->options;
  proxy_request request;
  proxy_response response;
  unsigned char *initial;
  size_t initial_length, header_length;
  unsigned char *request_body = NULL, *origin_body = NULL, *decoded = NULL, *cached_body = NULL;
  size_t request_body_length = 0U, origin_body_length = 0U;
  laghu_socket origin = LAGHU_INVALID_SOCKET;
  SSL *origin_tls = NULL;
  proxy_origin_connection upstream = {.socket = LAGHU_INVALID_SOCKET};
  bool origin_reusable = false;
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
  bool html_cache_response = false;
  bool tls_timed_out = false;
  proxy_header *request_host = NULL;
  char cached_content_type[LAGHU_RUNTIME_TYPE_SIZE];
  char cached_validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
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
  if (!proxy_read_headers(client, request.storage, NULL, &header_length, &initial, &initial_length) ||
      !proxy_parse_request(&request, header_length)) {
    PROXY_FAIL(400U, "Bad Request", "client_parse");
    goto done;
  }
  (void)snprintf(access.method, sizeof(access.method), "%s", request.method);
  {
    const char *query = strchr(request.target, '?');
    size_t length = query == NULL ? strlen(request.target) : (size_t)(query - request.target);
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
      !proxy_read_body(client, NULL, initial, initial_length, request.content_length, false, &request_body, &request_body_length)) {
    PROXY_FAIL(400U, "Bad Request", "client_parse");
    goto done;
  }
  if (proxy_handle_administrative_routes(connection, worker, &request, &access)) goto done;
  if (!strncmp(request.target, "/.laghu/", 8U)) {
    if (proxy_handle_beacon_routes(connection, worker, &request, request_body, request_body_length, &access)) goto done;
    if (strcmp(request.method, "GET") != 0 && strcmp(request.method, "HEAD") != 0) {
      PROXY_FAIL(405U, "Method Not Allowed", "request_limit");
      goto done;
    }
    memset(&normalized_request, 0, sizeof(normalized_request));
    memset(&normalized_response, 0, sizeof(normalized_response));
    memset(&environment, 0, sizeof(environment));
    {
      const char *scheme = proxy_effective_scheme(options, connection, &request);
      normalized_request = (laghu_http_request){LAGHU_HTTP_ABI_VERSION,
                                                sizeof(normalized_request),
                                                {(const unsigned char *)request.method, strlen(request.method)},
                                                {(const unsigned char *)scheme, strlen(scheme)},
                                                {NULL, 0U},
                                                {(const unsigned char *)request.target, strlen(request.target)},
                                                NULL,
                                                0U};
    }
    {
      proxy_header *host = proxy_find(request.headers, request.header_count, "Host");
      normalized_request.authority = (laghu_buffer){(const unsigned char *)(host != NULL ? host->value : options->listen_host),
                                                    strlen(host != NULL ? host->value : options->listen_host)};
    }
    normalized_response =
        (laghu_http_response){LAGHU_HTTP_ABI_VERSION, sizeof(normalized_response), 200U, NULL, 0U, 0U, false, true, false, {NULL, 0U}};
    environment = (laghu_http_environment){
        .version = LAGHU_HTTP_ABI_VERSION,
        .struct_size = sizeof(environment),
        .config = options->config,
        .cache_path = options->service.image_cache,
        .asset_offload = options->service.asset_offload,
        .rum = worker->queue->rum,
        .queue = proxy_runtime_queue(worker),
        .queue_capabilities = proxy_runtime_queue_capabilities(worker),
        .font_fetch_queue = options->service.font_providers != NULL ? proxy_font_fetch_queue(worker) : NULL,
        .font_providers = options->service.font_providers,
        .javascript_queue = options->service.javascript_queue[0] != '\0' ? proxy_javascript_queue(worker) : NULL,
        .chrome_analysis_queue = options->service.chrome_analysis_queue[0] != '\0' ? proxy_chrome_analysis_queue(worker) : NULL,
        .otel_trace_queue = options->service.otel_trace_queue[0] != '\0' ? proxy_otel_trace_queue(worker) : NULL,
        .otel_sampling_rate = options->service.otel_sampling_rate,
        .chrome_analysis_timeout_ms = options->service.chrome_analysis_timeout_ms,
        .javascript_target = options->service.javascript_target,
        .javascript_observations = options->service.javascript_observations,
        .javascript_defer = options->service.javascript_defer,
        .layout_reservations = options->service.layout_reservations,
        .now = (uint64_t)time(NULL)};
    laghu_http_transaction_init(&transaction);
    if (laghu_http_transaction_prepare(&transaction, &normalized_request, &normalized_response, &environment, &prepared) &&
        prepared.action == LAGHU_HTTP_ACTION_SERVE_CACHED && proxy_materialize_cached_result(&prepared)) {
      response.status = 200U;
      (void)laghu_base_string_copy(response.reason, sizeof(response.reason), "OK");
      if (!proxy_send_result(client, &response, &prepared, prepared.selected)) access.failure = "client_disconnect";
      access.cache_state = "immutable";
      access.output_bytes = prepared.selected.length;
    } else
      PROXY_FAIL(404U, "Not Found", "none");
    laghu_http_transaction_result_release(&prepared);
    goto done;
  }
  if (proxy_html_cache_request_eligible(options, &request)) {
    laghu_html_cache_record record;
    if (laghu_html_cache_lookup(options->service.image_cache, options->config.html_cache_origin, request.target, (uint64_t)time(NULL),
                                options->config.html_cache_ttl, options->config.html_cache_stale_ttl, &record) &&
        record.state != LAGHU_HTML_CACHE_MISS && record.entry.length != 0U && record.entry.length <= LAGHU_PROXY_MAX_BODY &&
        (cached_body = malloc(record.entry.length)) != NULL && laghu_runtime_cache_read(&record.entry, cached_body, record.entry.length)) {
      memset(&normalized_request, 0, sizeof(normalized_request));
      memset(&normalized_response, 0, sizeof(normalized_response));
      memset(&environment, 0, sizeof(environment));
      memset(&response, 0, sizeof(response));
      (void)snprintf(cached_content_type, sizeof(cached_content_type), "%s", record.entry.content_type);
      (void)snprintf(cached_validator, sizeof(cached_validator), "%s", record.entry.validator);
      response.status = 200U;
      (void)laghu_base_string_copy(response.reason, sizeof(response.reason), "OK");
      response.headers[response.header_count++] = (proxy_header){"Content-Type", cached_content_type};
      if (cached_validator[0] != '\0') response.headers[response.header_count++] = (proxy_header){"ETag", cached_validator};
      response.content_length = record.entry.length;
      response.has_content_length = true;
      for (index = 0U; index < request.header_count; ++index)
        request_headers[index] = (laghu_http_header){{(unsigned char *)request.headers[index].name, strlen(request.headers[index].name)},
                                                     {(unsigned char *)request.headers[index].value, strlen(request.headers[index].value)}};
      for (index = 0U; index < response.header_count; ++index)
        response_headers[index] = (laghu_http_header){{(unsigned char *)response.headers[index].name, strlen(response.headers[index].name)},
                                                      {(unsigned char *)response.headers[index].value, strlen(response.headers[index].value)}};
      {
        const char *scheme = proxy_effective_scheme(options, connection, &request);
        proxy_header *host = proxy_find(request.headers, request.header_count, "Host");
        normalized_request = (laghu_http_request){
            LAGHU_HTTP_ABI_VERSION,
            sizeof(normalized_request),
            {(unsigned char *)request.method, strlen(request.method)},
            {(unsigned char *)scheme, strlen(scheme)},
            {(unsigned char *)(host != NULL ? host->value : options->listen_host), strlen(host != NULL ? host->value : options->listen_host)},
            {(unsigned char *)request.target, strlen(request.target)},
            request_headers,
            request.header_count};
      }
      normalized_response = (laghu_http_response){LAGHU_HTTP_ABI_VERSION,
                                                  sizeof(normalized_response),
                                                  response.status,
                                                  response_headers,
                                                  response.header_count,
                                                  record.entry.length,
                                                  true,
                                                  true,
                                                  false,
                                                  {NULL, 0U}};
      environment = (laghu_http_environment){
          .version = LAGHU_HTTP_ABI_VERSION,
          .struct_size = sizeof(environment),
          .config = options->config,
          .cache_path = options->service.image_cache,
          .asset_offload = options->service.asset_offload,
          .rum = worker->queue->rum,
          .queue = proxy_runtime_queue(worker),
          .queue_capabilities = proxy_runtime_queue_capabilities(worker),
          .font_fetch_queue = options->service.font_providers != NULL ? proxy_font_fetch_queue(worker) : NULL,
          .font_providers = options->service.font_providers,
          .javascript_queue = options->service.javascript_queue[0] != '\0' ? proxy_javascript_queue(worker) : NULL,
          .chrome_analysis_queue = options->service.chrome_analysis_queue[0] != '\0' ? proxy_chrome_analysis_queue(worker) : NULL,
          .otel_trace_queue = options->service.otel_trace_queue[0] != '\0' ? proxy_otel_trace_queue(worker) : NULL,
          .otel_sampling_rate = options->service.otel_sampling_rate,
          .chrome_analysis_timeout_ms = options->service.chrome_analysis_timeout_ms,
          .javascript_target = options->service.javascript_target,
          .javascript_observations = options->service.javascript_observations,
          .javascript_defer = options->service.javascript_defer,
          .layout_reservations = options->service.layout_reservations,
          .now = (uint64_t)time(NULL)};
      laghu_http_transaction_init(&transaction);
      prepared_ok = laghu_http_transaction_prepare(&transaction, &normalized_request, &normalized_response, &environment, &prepared);
      if (prepared_ok && record.state == LAGHU_HTML_CACHE_STALE) proxy_html_cache_enqueue_refresh(worker, request.target, &record);
      if (prepared_ok && prepared.action == LAGHU_HTTP_ACTION_BYPASS) {
        (void)proxy_html_cache_mark_hit(&prepared);
        if (!proxy_send_result(client, &response, &prepared, (laghu_buffer){cached_body, record.entry.length})) access.failure = "client_disconnect";
        access.cache_state = record.state == LAGHU_HTML_CACHE_STALE ? "html_stale" : "html_fresh";
        origin_body_length = record.entry.length;
        free(cached_body);
        cached_body = NULL;
        goto done;
      }
      if (prepared_ok && !proxy_is_forcing(worker->queue) &&
          laghu_http_transaction_finalize(&transaction, (laghu_buffer){cached_body, record.entry.length}, &finalized)) {
        laghu_operational_registry_budget(&worker->queue->operational, &transaction.budget, transaction.environment.config.transform_deadline_ms);
        laghu_operational_registry_lcp(&worker->queue->operational, finalized.lcp_decision, finalized.lcp_applied, finalized.lcp_profile_observations,
                                       finalized.lcp_profile_ready);
        finalized.not_modified = laghu_http_request_matches_result_etag(&normalized_request, &finalized);
        (void)proxy_html_cache_mark_hit(&finalized);
        if ((!finalized.not_modified && !proxy_send_early_hints(client, request.version, &finalized)) ||
            !proxy_send_result(client, &response, &finalized, finalized.selected))
          access.failure = "client_disconnect";
        access.cache_state = record.state == LAGHU_HTML_CACHE_STALE ? "html_stale" : "html_fresh";
        origin_body_length = record.entry.length;
        free(cached_body);
        cached_body = NULL;
        goto done;
      }
      laghu_http_transaction_result_release(&prepared);
      laghu_http_transaction_result_release(&finalized);
      memset(&prepared, 0, sizeof(prepared));
      memset(&finalized, 0, sizeof(finalized));
      prepared_ok = false;
    }
    free(cached_body);
    cached_body = NULL;
    memset(&response, 0, sizeof(response));
  }
  if (!proxy_origin_acquire(worker, &upstream, &tls_timed_out)) {
    PROXY_FAIL(502U, "Bad Gateway", tls_timed_out ? "origin_timeout" : (options->origin_tls ? "origin_tls" : "origin_connect"));
    goto done;
  }
  origin = upstream.socket;
  origin_tls = upstream.tls;
  outbound_length = (size_t)snprintf(outbound, sizeof(outbound), "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n", request.method,
                                     request.target, options->origin_authority);
  for (index = 0U; index < request.header_count; ++index) {
    int n;
    if (proxy_hop(request.headers[index].name) || proxy_connection_nominates(request.headers, request.header_count, request.headers[index].name) ||
        proxy_forwarding_name(request.headers[index].name) || proxy_name_equal(request.headers[index].name, "Host") ||
        proxy_name_equal(request.headers[index].name, "Content-Length"))
      continue;
    n = snprintf(outbound + outbound_length, sizeof(outbound) - outbound_length, "%s: %s\r\n", request.headers[index].name,
                 request.headers[index].value);
    if (n <= 0 || (size_t)n >= sizeof(outbound) - outbound_length) {
      PROXY_FAIL(400U, "Bad Request", "request_limit");
      goto done;
    }
    outbound_length += (size_t)n;
  }
  if (request_host == NULL ||
      !proxy_append_forwarding(options, connection, &request, request_host->value, outbound, sizeof(outbound), &outbound_length)) {
    PROXY_FAIL(400U, "Bad Request", "client_parse");
    goto done;
  }
  if (request.content_length != 0U)
    outbound_length +=
        (size_t)snprintf(outbound + outbound_length, sizeof(outbound) - outbound_length, "Content-Length: %zu\r\n", request.content_length);
  if (outbound_length + 2U >= sizeof(outbound)) {
    PROXY_FAIL(400U, "Bad Request", "request_limit");
    goto done;
  }
  memcpy(outbound + outbound_length, "\r\n", 2U);
  outbound_length += 2U;
  if (!proxy_origin_send_all(origin, origin_tls, outbound, outbound_length) ||
      (request_body_length && !proxy_origin_send_all(origin, origin_tls, request_body, request_body_length)) ||
      !proxy_read_headers(origin, response.storage, origin_tls, &header_length, &initial, &initial_length) ||
      !proxy_parse_response(&response, header_length)) {
    PROXY_FAIL(502U, "Bad Gateway", proxy_socket_timed_out() ? "origin_timeout" : (origin_tls != NULL ? "origin_tls" : "origin_protocol"));
    goto done;
  }
  for (index = 0U; index < request.header_count; ++index)
    request_headers[index] = (laghu_http_header){{(unsigned char *)request.headers[index].name, strlen(request.headers[index].name)},
                                                 {(unsigned char *)request.headers[index].value, strlen(request.headers[index].value)}};
  for (index = 0U; index < response.header_count; ++index)
    response_headers[index] = (laghu_http_header){{(unsigned char *)response.headers[index].name, strlen(response.headers[index].name)},
                                                  {(unsigned char *)response.headers[index].value, strlen(response.headers[index].value)}};
  {
    const char *scheme = proxy_effective_scheme(options, connection, &request);
    normalized_request = (laghu_http_request){LAGHU_HTTP_ABI_VERSION,
                                              sizeof(normalized_request),
                                              {(unsigned char *)request.method, strlen(request.method)},
                                              {(unsigned char *)scheme, strlen(scheme)},
                                              {NULL, 0U},
                                              {(unsigned char *)request.target, strlen(request.target)},
                                              request_headers,
                                              request.header_count};
  }
  {
    proxy_header *host = proxy_find(request.headers, request.header_count, "Host");
    normalized_request.authority = (laghu_buffer){(unsigned char *)(host != NULL ? host->value : options->listen_host),
                                                  strlen(host != NULL ? host->value : options->listen_host)};
  }
  normalized_response = (laghu_http_response){LAGHU_HTTP_ABI_VERSION,
                                              sizeof(normalized_response),
                                              response.status,
                                              response_headers,
                                              response.header_count,
                                              response.content_length,
                                              response.has_content_length,
                                              true,
                                              response.status == 206U || proxy_find(response.headers, response.header_count, "Content-Range") != NULL,
                                              {NULL, 0U}};
  environment = (laghu_http_environment){
      .version = LAGHU_HTTP_ABI_VERSION,
      .struct_size = sizeof(environment),
      .config = options->config,
      .cache_path = options->service.image_cache,
      .asset_offload = options->service.asset_offload,
      .rum = worker->queue->rum,
      .queue = proxy_runtime_queue(worker),
      .queue_capabilities = proxy_runtime_queue_capabilities(worker),
      .font_fetch_queue = options->service.font_providers != NULL ? proxy_font_fetch_queue(worker) : NULL,
      .font_providers = options->service.font_providers,
      .javascript_queue = options->service.javascript_queue[0] != '\0' ? proxy_javascript_queue(worker) : NULL,
      .chrome_analysis_queue = options->service.chrome_analysis_queue[0] != '\0' ? proxy_chrome_analysis_queue(worker) : NULL,
      .otel_trace_queue = options->service.otel_trace_queue[0] != '\0' ? proxy_otel_trace_queue(worker) : NULL,
      .otel_sampling_rate = options->service.otel_sampling_rate,
      .chrome_analysis_timeout_ms = options->service.chrome_analysis_timeout_ms,
      .javascript_target = options->service.javascript_target,
      .javascript_observations = options->service.javascript_observations,
      .javascript_defer = options->service.javascript_defer,
      .layout_reservations = options->service.layout_reservations,
      .now = (uint64_t)time(NULL)};
  if (!response.chunked) {
    bool bodyless =
        !strcmp(request.method, "HEAD") || (response.status >= 100U && response.status < 200U) || response.status == 204U || response.status == 304U;
    laghu_http_transaction_init(&transaction);
    prepared_ok = laghu_http_transaction_prepare(&transaction, &normalized_request, &normalized_response, &environment, &prepared);
    if (!prepared_ok) {
      PROXY_FAIL(502U, "Bad Gateway", "transform");
      goto done;
    }
    if (prepared.action == LAGHU_HTTP_ACTION_SERVE_CACHED) {
      if (!proxy_materialize_cached_result(&prepared)) {
        PROXY_FAIL(502U, "Bad Gateway", "cache_read");
        goto done;
      }
      if (!proxy_send_early_hints(client, request.version, &prepared) || !proxy_send_result(client, &response, &prepared, prepared.selected))
        access.failure = "client_disconnect";
      goto done;
    }
    if (prepared.action == LAGHU_HTTP_ACTION_BYPASS) {
      size_t expected = bodyless ? 0U : response.content_length;
      bool until_close = !bodyless && !response.has_content_length;
      if (!proxy_send_early_hints(client, request.version, &prepared) ||
          !proxy_send_headers(client, &response, &prepared, response.content_length, response.has_content_length) ||
          !proxy_stream_body(origin, origin_tls, client, initial, bodyless ? 0U : initial_length, expected, until_close)) {
        access.failure = "client_disconnect";
        goto done;
      }
      goto done;
    }
  }
  {
    bool bodyless =
        !strcmp(request.method, "HEAD") || (response.status >= 100U && response.status < 200U) || response.status == 204U || response.status == 304U;
    if (!proxy_read_body(origin, origin_tls, initial, initial_length, bodyless ? 0U : (response.has_content_length ? response.content_length : 0U),
                         !bodyless && !response.has_content_length, &origin_body, &origin_body_length)) {
      PROXY_FAIL(502U, "Bad Gateway", proxy_socket_timed_out() ? "origin_timeout" : (origin_tls != NULL ? "origin_tls" : "origin_protocol"));
      goto done;
    }
    origin_reusable = !strcmp(response.version, "HTTP/1.1") && !proxy_connection_nominates(response.headers, response.header_count, "close") &&
                      (bodyless ? initial_length == 0U : response.has_content_length || response.chunked);
  }
  if (response.chunked) {
    size_t decoded_length = 0U;
    decoded = malloc(LAGHU_PROXY_MAX_BODY);
    if (decoded == NULL ||
        !laghu_proxy_decode_chunked((laghu_buffer){origin_body, origin_body_length}, decoded, LAGHU_PROXY_MAX_BODY, &decoded_length)) {
      PROXY_FAIL(502U, "Bad Gateway", "origin_protocol");
      goto done;
    }
    free(origin_body);
    origin_body = decoded;
    decoded = NULL;
    origin_body_length = decoded_length;
  }
  html_cache_response = proxy_html_cache_response_eligible(options, &request, &response, origin_body_length);
  if (!prepared_ok) {
    normalized_response.declared_length = origin_body_length;
    normalized_response.has_declared_length = true;
    laghu_http_transaction_init(&transaction);
    prepared_ok = laghu_http_transaction_prepare(&transaction, &normalized_request, &normalized_response, &environment, &prepared);
    if (!prepared_ok) {
      PROXY_FAIL(502U, "Bad Gateway", "transform");
      goto done;
    }
  }
  if (prepared.action == LAGHU_HTTP_ACTION_SERVE_CACHED) {
    if (!proxy_materialize_cached_result(&prepared)) {
      PROXY_FAIL(502U, "Bad Gateway", "cache_read");
      goto done;
    }
    prepared.not_modified = laghu_http_request_matches_result_etag(&normalized_request, &prepared);
    if (!proxy_send_early_hints(client, request.version, &prepared) || !proxy_send_result(client, &response, &prepared, prepared.selected))
      access.failure = "client_disconnect";
  } else if (prepared.action == LAGHU_HTTP_ACTION_BYPASS) {
    if (!proxy_send_result(client, &response, &prepared, (laghu_buffer){origin_body, origin_body_length})) access.failure = "client_disconnect";
  } else if (!proxy_is_forcing(worker->queue) &&
             laghu_http_transaction_finalize(&transaction, (laghu_buffer){origin_body, origin_body_length}, &finalized)) {
    laghu_operational_registry_budget(&worker->queue->operational, &transaction.budget, transaction.environment.config.transform_deadline_ms);
    laghu_operational_registry_lcp(&worker->queue->operational, finalized.lcp_decision, finalized.lcp_applied, finalized.lcp_profile_observations,
                                   finalized.lcp_profile_ready);
    finalized.not_modified = laghu_http_request_matches_result_etag(&normalized_request, &finalized);
    if (html_cache_response) {
      proxy_header *validator = proxy_find(response.headers, response.header_count, "ETag");
      laghu_html_cache_record record;
      (void)laghu_html_cache_publish(options->service.image_cache, options->config.html_cache_origin, request.target,
                                     validator == NULL ? "" : validator->value, finalized.selected, (uint64_t)time(NULL), &record);
    }
    if ((!finalized.not_modified && !proxy_send_early_hints(client, request.version, &finalized)) ||
        !proxy_send_result(client, &response, &finalized, finalized.selected))
      access.failure = "client_disconnect";
  } else {
    if (prepared_ok)
      laghu_operational_registry_budget(&worker->queue->operational, &transaction.budget, transaction.environment.config.transform_deadline_ms);
    if (!proxy_send_result(client, &response, &finalized, (laghu_buffer){origin_body, origin_body_length})) access.failure = "client_disconnect";
  }
done:
  if (transaction.version == LAGHU_HTTP_ABI_VERSION && transaction.trace.trace_id[0] != '\0') {
    (void)snprintf(access.trace_id, sizeof(access.trace_id), "%s", transaction.trace.trace_id);
    (void)snprintf(access.span_id, sizeof(access.span_id), "%s", transaction.trace.span_id);
  }
  if (access.status == 0U)
    access.status = (finalized.not_modified || prepared.not_modified) ? 304U : (response.status != 0U ? response.status : 200U);
  if (finalized.version == LAGHU_HTTP_ABI_VERSION) {
    access.decision = finalized.decision;
    access.job_published = finalized.job_published;
    access.output_bytes = finalized.selected.length;
    access.cache_state = "cold";
    access.javascript_defer_recommended = finalized.javascript_defer_recommended;
    access.javascript_defer_rollback_recommended = finalized.javascript_defer_rollback_recommended;
    (void)snprintf(access.javascript_defer_path, sizeof(access.javascript_defer_path), "%s", finalized.javascript_defer_path);
    (void)snprintf(access.javascript_defer_template, sizeof(access.javascript_defer_template), "%s", finalized.javascript_defer_template);
    access.javascript_defer_bucket = finalized.javascript_defer_bucket;
    access.javascript_defer_observations = finalized.javascript_defer_observations;
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
  if (access.output_bytes == 0U && origin_body_length != 0U) access.output_bytes = origin_body_length;
  if (proxy_is_forcing(worker->queue)) access.failure = "shutdown";
  proxy_access_write(worker->queue, &access);
  laghu_http_transaction_result_release(&prepared);
  laghu_http_transaction_result_release(&finalized);
  free(request_body);
  free(origin_body);
  free(decoded);
  if (origin != LAGHU_INVALID_SOCKET) proxy_origin_release(worker, &upstream, origin_reusable);
  laghu_close(client);
#undef PROXY_FAIL
}
