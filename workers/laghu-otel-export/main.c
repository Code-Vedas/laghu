// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "laghu/log.h"
#include "laghu/queue.h"
#include "laghu/source.h"
#include "laghu/worker.h"

#define LAGHU_OTEL_BACKEND "laghu-otel-export-v1"
#define LAGHU_OTEL_QUEUE_SLOTS 64U
#define LAGHU_OTEL_PAYLOAD 65536U
#define LAGHU_OTEL_TIMEOUT_SECONDS 5U
#define LAGHU_OTEL_RETRY_COUNT 2U

static volatile sig_atomic_t laghu_otel_stop;
static const char *laghu_otel_service = "443";

static void laghu_otel_signal(int value) { (void)value; laghu_otel_stop = 1; }

static void laghu_otel_pause(void) {
  struct timespec value = {.tv_sec = 0, .tv_nsec = 10000000L};
  (void)nanosleep(&value, NULL);
}

static bool laghu_otel_endpoint(const char *value, char host[256U], char path[1024U]) {
  const char *authority, *slash;
  size_t host_length;
  if (value == NULL || strncmp(value, "https://", 8U) != 0) return false;
  authority = value + 8U;
  slash = strchr(authority, '/');
  if (slash == NULL) return false;
  host_length = (size_t)(slash - authority);
  if (host_length == 0U || host_length >= 256U || strchr(authority, '@') != NULL || memchr(authority, ':', host_length) != NULL ||
      strlen(slash) >= 1024U || strchr(slash, '#') != NULL || strchr(slash, '?') != NULL)
    return false;
  memcpy(host, authority, host_length);
  host[host_length] = '\0';
  memcpy(path, slash, strlen(slash) + 1U);
  return true;
}

static int laghu_otel_connect(const char *host) {
  struct addrinfo hints, *items = NULL, *item;
  int result = -1;
  struct timeval timeout = {LAGHU_OTEL_TIMEOUT_SECONDS, 0};
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, laghu_otel_service, &hints, &items) != 0) return -1;
  #if !LAGHU_TEST_HOOKS
  for (item = items; item != NULL; item = item->ai_next)
    if (!laghu_source_address_public(item->ai_addr)) { freeaddrinfo(items); return -1; }
  #endif
  for (item = items; item != NULL; item = item->ai_next) {
    result = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
    if (result < 0) continue;
    (void)setsockopt(result, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(result, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (connect(result, item->ai_addr, (socklen_t)item->ai_addrlen) == 0) break;
    (void)close(result);
    result = -1;
  }
  freeaddrinfo(items);
  return result;
}

static bool laghu_otel_write(SSL *tls, const unsigned char *data, size_t length) {
  while (length != 0U) {
    int sent = SSL_write(tls, data, (int)(length > 65536U ? 65536U : length));
    if (sent <= 0) return false;
    data += (size_t)sent;
    length -= (size_t)sent;
  }
  return true;
}

static bool laghu_otel_authorization(const char **authorization) {
  const char *value = getenv("LAGHU_OTEL_AUTHORIZATION");
  if (value != NULL && (strlen(value) > 1024U || strchr(value, '\r') != NULL || strchr(value, '\n') != NULL)) return false;
  *authorization = value;
  return true;
}

static void laghu_otel_log_lifecycle(const char *state, const char *failure) {
  char line[LAGHU_LOG_LINE_SIZE];
  laghu_log_lifecycle record = {.common = {(time_t)time(NULL), "worker", "otel-export", NULL, NULL}, .state = state, .failure = failure};
  if (laghu_log_render_lifecycle(&record, line, sizeof(line))) fprintf(stderr, "%s\n", line);
}

static bool laghu_otel_export(SSL_CTX *context, const char *host, const char *path, const char *authorization, laghu_buffer payload) {
  int socket_value;
  SSL *tls;
  X509_VERIFY_PARAM *parameters;
  char request[2048U];
  int length;
  unsigned char response[64U];
  bool success = false;
  socket_value = laghu_otel_connect(host);
  if (socket_value < 0 || (tls = SSL_new(context)) == NULL) {
    if (socket_value >= 0) (void)close(socket_value);
    return false;
  }
  parameters = SSL_get0_param(tls);
  if (!SSL_set_tlsext_host_name(tls, host) || !SSL_set1_host(tls, host) || !X509_VERIFY_PARAM_set1_host(parameters, host, 0U) ||
      !SSL_set_fd(tls, socket_value) ||
      SSL_connect(tls) != 1 || SSL_get_verify_result(tls) != X509_V_OK)
    goto done;
  length = snprintf(request, sizeof(request),
                    "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n%s%s%sContent-Length: %zu\r\nConnection: close\r\n\r\n",
                    path, host, authorization == NULL ? "" : "Authorization: ", authorization == NULL ? "" : authorization,
                    authorization == NULL ? "" : "\r\n", payload.length);
  if (length <= 0 || (size_t)length >= sizeof(request) || !laghu_otel_write(tls, (const unsigned char *)request, (size_t)length) ||
      !laghu_otel_write(tls, payload.data, payload.length) || SSL_read(tls, response, sizeof(response)) < 12)
    goto done;
  success = memcmp(response, "HTTP/1.1 2", 10U) == 0 || memcmp(response, "HTTP/1.0 2", 10U) == 0;
done:
  (void)SSL_shutdown(tls);
  SSL_free(tls);
  (void)close(socket_value);
  return success;
}

int main(int argc, char **argv) {
  laghu_runtime_queue queue;
  laghu_worker_lifecycle lifecycle;
  SSL_CTX *tls;
  char host[256U], path[1024U];
  unsigned char payload[LAGHU_OTEL_PAYLOAD];
  laghu_runtime_job job;
  const char *authorization;
  const char *cache_path;
  if (argc < 5 || argc > 7 || strcmp(argv[1], "--queue") != 0 || strcmp(argv[3], "--endpoint") != 0 ||
      !laghu_otel_endpoint(argv[4], host, path))
    return 2;
  if (!laghu_otel_authorization(&authorization)) return 2;
  cache_path = getenv("LAGHU_OTEL_CACHE_PATH");
  if (cache_path == NULL || cache_path[0] == '\0') return 2;
#if LAGHU_TEST_HOOKS
  {
    const char *test_endpoint = getenv("LAGHU_TEST_OTEL_ENDPOINT");
    const char *test_port = getenv("LAGHU_TEST_OTEL_PORT");
    if (test_endpoint != NULL && !laghu_otel_endpoint(test_endpoint, host, path)) return 2;
    if (test_port != NULL && strspn(test_port, "0123456789") == strlen(test_port) && test_port[0] != '\0') laghu_otel_service = test_port;
  }
#endif
  tls = SSL_CTX_new(TLS_client_method());
  if (tls == NULL || SSL_CTX_set_min_proto_version(tls, TLS1_2_VERSION) != 1 || SSL_CTX_set_default_verify_paths(tls) != 1) return 1;
  if (argc == 7 && (strcmp(argv[5], "--ca-file") != 0 || SSL_CTX_load_verify_locations(tls, argv[6], NULL) != 1)) {
    SSL_CTX_free(tls);
    return 2;
  }
  SSL_CTX_set_verify(tls, SSL_VERIFY_PEER, NULL);
  laghu_runtime_queue_init(&queue);
  laghu_worker_lifecycle_init(&lifecycle);
  if (!laghu_runtime_queue_open(&queue, argv[2]) && !laghu_runtime_queue_create(&queue, argv[2], LAGHU_OTEL_QUEUE_SLOTS, LAGHU_OTEL_PAYLOAD)) {
    SSL_CTX_free(tls); return 1;
  }
  (void)laghu_runtime_queue_set_backend(&queue, 0U, LAGHU_OTEL_BACKEND);
  if (!laghu_worker_lifecycle_start(&lifecycle, cache_path, LAGHU_OPERATIONAL_PROCESS_OTEL_EXPORT, &queue, true, (uint64_t)time(NULL))) {
    laghu_runtime_queue_close(&queue);
    SSL_CTX_free(tls);
    return 1;
  }
  laghu_otel_log_lifecycle("started", "none");
  signal(SIGINT, laghu_otel_signal);
  signal(SIGTERM, laghu_otel_signal);
  while (!laghu_otel_stop) {
    if (laghu_runtime_queue_try_take(&queue, &job, payload, sizeof(payload))) {
      uint64_t started = laghu_worker_lifecycle_clock();
      bool success = false;
      unsigned int attempt;
      for (attempt = 0U; attempt < LAGHU_OTEL_RETRY_COUNT && !success && job.kind == LAGHU_RUNTIME_JOB_TRACE_EXPORT;
           ++attempt) {
        success = laghu_otel_export(tls, host, path, authorization, job.payload);
        if (!success && attempt + 1U < LAGHU_OTEL_RETRY_COUNT) laghu_otel_pause();
      }
      char line[LAGHU_LOG_LINE_SIZE];
      laghu_log_job record = {.common = {(time_t)time(NULL), "worker", "otel-export", job.trace.trace_id, job.trace.span_id},
                              .job_kind = "trace_export", .outcome = success ? "success" : "failed", .input_bytes = job.payload.length,
                              .output_bytes = job.payload.length,
                              .duration_ms = 0U, .failure = success ? "none" : "network"};
      if (laghu_log_render_job(&record, line, sizeof(line))) fprintf(stderr, "%s\n", line);
      laghu_worker_lifecycle_job(&lifecycle, success, laghu_worker_lifecycle_clock() - started,
                                 LAGHU_OPERATIONAL_FAILURE_TRANSPORT);
    } else {
      laghu_worker_lifecycle_heartbeat(&lifecycle, (uint64_t)time(NULL), true);
      laghu_otel_pause();
    }
  }
  laghu_otel_log_lifecycle("stopped", "none");
  laghu_worker_lifecycle_stop(&lifecycle, (uint64_t)time(NULL));
  laghu_runtime_queue_close(&queue);
  SSL_CTX_free(tls);
  return 0;
}
