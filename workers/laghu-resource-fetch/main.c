// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define laghu_socket SOCKET
#define laghu_socklen int
#define LAGHU_INVALID_SOCKET INVALID_SOCKET
#define laghu_close closesocket
#define laghu_sleep_ms(value) Sleep(value)
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define laghu_socket int
#define laghu_socklen socklen_t
#define LAGHU_INVALID_SOCKET (-1)
#define laghu_close close
static void laghu_sleep_ms(unsigned int value) {
  struct timespec pause = {.tv_sec = (time_t)(value / 1000U),
                           .tv_nsec = (long)(value % 1000U) * 1000000L};
  (void)nanosleep(&pause, NULL);
}
#endif

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "laghu/runtime.h"

#define LAGHU_FETCH_HEADER_MAX 65536U
#define LAGHU_FETCH_REDIRECT_MAX 3U
#define LAGHU_FETCH_TIMEOUT_SECONDS 10U
#define LAGHU_FETCH_RETRY_SECONDS 60U
#define LAGHU_FETCH_BACKEND "laghu-resource-fetch-v1"

typedef struct {
  unsigned int status;
  char content_type[128];
  char location[LAGHU_RUNTIME_PATH_SIZE];
  unsigned int max_age;
  unsigned char *body;
  size_t length;
} laghu_fetch_response;

static volatile sig_atomic_t laghu_fetch_stop;

#ifdef _WIN32
static SERVICE_STATUS_HANDLE laghu_fetch_service_handle;
static SERVICE_STATUS laghu_fetch_service_status;
static const char *laghu_fetch_service_queue;
static const char *laghu_fetch_service_cache;
static const char *laghu_fetch_service_providers;
#endif

static void laghu_fetch_signal(int signal_number) {
  (void)signal_number;
  laghu_fetch_stop = 1;
}

static bool laghu_fetch_url(const char *url, char host[256],
                            char target[LAGHU_RUNTIME_PATH_SIZE]) {
  const char *authority;
  const char *slash;
  size_t host_length;
  if (url == NULL || strncmp(url, "https://", 8U) != 0) return false;
  authority = url + 8U;
  slash = strchr(authority, '/');
  if (slash == NULL) return false;
  host_length = (size_t)(slash - authority);
  if (host_length == 0U || host_length >= 256U ||
      host_length + strlen(slash) + 1U >= LAGHU_RUNTIME_PATH_SIZE ||
      memchr(authority, ':', host_length) != NULL ||
      memchr(authority, '@', host_length) != NULL || strchr(slash, '#') != NULL)
    return false;
  memcpy(host, authority, host_length);
  host[host_length] = '\0';
  memcpy(target, slash, strlen(slash) + 1U);
  return true;
}

static void laghu_fetch_timeout(laghu_socket socket) {
#ifdef _WIN32
  DWORD timeout = LAGHU_FETCH_TIMEOUT_SECONDS * 1000U;
  (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
                   sizeof(timeout));
  (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout,
                   sizeof(timeout));
#else
  struct timeval timeout = {LAGHU_FETCH_TIMEOUT_SECONDS, 0};
  (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

static laghu_socket laghu_fetch_connect(const char *host) {
  struct addrinfo hints, *addresses = NULL, *item;
  laghu_socket result = LAGHU_INVALID_SOCKET;
#if LAGHU_TEST_HOOKS
  const char *endpoint = getenv("LAGHU_TEST_FETCH_ENDPOINT");
  char test_host[256];
  char test_port[6];
  if (endpoint != NULL) {
    const char *colon = strrchr(endpoint, ':');
    size_t length = colon == NULL ? 0U : (size_t)(colon - endpoint);
    if (length == 0U || length >= sizeof(test_host) || strlen(colon + 1U) >= 6U)
      return result;
    memcpy(test_host, endpoint, length);
    test_host[length] = '\0';
    (void)snprintf(test_port, sizeof(test_port), "%s", colon + 1U);
    host = test_host;
  }
#endif
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host,
#if LAGHU_TEST_HOOKS
                  endpoint != NULL ? test_port : "443",
#else
                  "443",
#endif
                  &hints, &addresses) != 0)
    return result;
  for (item = addresses; item != NULL; item = item->ai_next)
    if (!laghu_source_address_public(item->ai_addr)
#if LAGHU_TEST_HOOKS
        && endpoint == NULL
#endif
    ) {
      freeaddrinfo(addresses);
      return result;
    }
  for (item = addresses; item != NULL; item = item->ai_next) {
    result = (laghu_socket)socket(item->ai_family, item->ai_socktype,
                                  item->ai_protocol);
    if (result == LAGHU_INVALID_SOCKET) continue;
    laghu_fetch_timeout(result);
    if (connect(result, item->ai_addr, (laghu_socklen)item->ai_addrlen) == 0)
      break;
    laghu_close(result);
    result = LAGHU_INVALID_SOCKET;
  }
  freeaddrinfo(addresses);
  return result;
}

static bool laghu_fetch_send(SSL *tls, const unsigned char *data,
                             size_t length) {
  while (length != 0U) {
    int sent = SSL_write(tls, data, (int)(length > 65536U ? 65536U : length));
    if (sent <= 0) return false;
    data += sent;
    length -= (size_t)sent;
  }
  return true;
}

static int laghu_fetch_casecmp(const char *left, const char *right,
                               size_t length) {
  size_t index;
  for (index = 0U; index < length; ++index) {
    int difference = tolower((unsigned char)left[index]) -
                     tolower((unsigned char)right[index]);
    if (difference != 0) return difference;
  }
  return 0;
}

static char *laghu_fetch_trim(char *value) {
  char *end;
  while (*value == ' ' || *value == '\t') ++value;
  end = value + strlen(value);
  while (end > value && (end[-1] == ' ' || end[-1] == '\t')) --end;
  *end = '\0';
  return value;
}

static bool laghu_fetch_read(SSL *tls, const laghu_font_provider *provider,
                             laghu_fetch_response *response) {
  unsigned char *data;
  size_t capacity = LAGHU_FETCH_HEADER_MAX + provider->max_css_bytes + 1U;
  size_t used = 0U;
  char *header_end;
  size_t header_length;
  char *line;
  bool chunked = false;
  size_t content_length = 0U;
  bool has_content_length = false;
  data = malloc(capacity);
  if (data == NULL) return false;
  while (used + 1U < capacity) {
    int got = SSL_read(tls, data + used, (int)(capacity - used - 1U));
    if (got <= 0) break;
    used += (size_t)got;
    data[used] = '\0';
  }
  header_end = strstr((char *)data, "\r\n\r\n");
  if (header_end == NULL ||
      (size_t)(header_end - (char *)data) > LAGHU_FETCH_HEADER_MAX) {
    free(data);
    return false;
  }
  header_length = (size_t)(header_end - (char *)data) + 4U;
  if (sscanf((char *)data, "HTTP/1.%*u %u", &response->status) != 1) {
    free(data);
    return false;
  }
  line = strstr((char *)data, "\r\n") + 2U;
  while (line < header_end) {
    char *next = strstr(line, "\r\n");
    char *colon;
    char *value;
    if (next == NULL || next > header_end) break;
    *next = '\0';
    colon = strchr(line, ':');
    if (colon == NULL) {
      free(data);
      return false;
    }
    *colon = '\0';
    value = laghu_fetch_trim(colon + 1U);
    if (strlen(line) == 12U &&
        laghu_fetch_casecmp(line, "Content-Type", 12U) == 0)
      (void)snprintf(response->content_type, sizeof(response->content_type),
                     "%s", value);
    else if (strlen(line) == 8U &&
             laghu_fetch_casecmp(line, "Location", 8U) == 0)
      (void)snprintf(response->location, sizeof(response->location), "%s",
                     value);
    else if (strlen(line) == 14U &&
             laghu_fetch_casecmp(line, "Content-Length", 14U) == 0) {
      char *end = NULL;
      unsigned long parsed = strtoul(value, &end, 10);
      if (end == value || *end != '\0' || parsed > provider->max_css_bytes) {
        free(data);
        return false;
      }
      content_length = (size_t)parsed;
      has_content_length = true;
    } else if (strlen(line) == 17U &&
               laghu_fetch_casecmp(line, "Transfer-Encoding", 17U) == 0 &&
               strstr(value, "chunked") != NULL) {
      chunked = true;
    } else if (strlen(line) == 16U &&
               laghu_fetch_casecmp(line, "Content-Encoding", 16U) == 0 &&
               strcmp(value, "identity") != 0) {
      free(data);
      return false;
    } else if (strlen(line) == 13U &&
               laghu_fetch_casecmp(line, "Cache-Control", 13U) == 0) {
      char *maximum = strstr(value, "max-age=");
      if (maximum != NULL) {
        unsigned long parsed = strtoul(maximum + 8U, NULL, 10);
        response->max_age = parsed > provider->ttl_seconds
                                ? provider->ttl_seconds
                                : (unsigned int)parsed;
      }
    }
    line = next + 2U;
  }
  if (chunked) {
    size_t input = header_length, output = 0U;
    unsigned char *decoded = malloc(provider->max_css_bytes + 1U);
    if (decoded == NULL) {
      free(data);
      return false;
    }
    while (input < used) {
      char *end = NULL;
      unsigned long size = strtoul((char *)data + input, &end, 16);
      if (end == (char *)data + input || end + 2U > (char *)data + used ||
          end[0] != '\r' || end[1] != '\n' ||
          size > provider->max_css_bytes - output) {
        free(decoded);
        free(data);
        return false;
      }
      input = (size_t)(end - (char *)data) + 2U;
      if (size == 0U) break;
      if (size > used - input || input + size + 2U > used ||
          data[input + size] != '\r' || data[input + size + 1U] != '\n') {
        free(decoded);
        free(data);
        return false;
      }
      memcpy(decoded + output, data + input, size);
      output += size;
      input += size + 2U;
    }
    decoded[output] = '\0';
    response->body = decoded;
    response->length = output;
  } else {
    size_t body_length = used - header_length;
    if ((has_content_length && body_length != content_length) ||
        body_length > provider->max_css_bytes) {
      free(data);
      return false;
    }
    response->body = malloc(body_length + 1U);
    if (response->body == NULL) {
      free(data);
      return false;
    }
    memcpy(response->body, data + header_length, body_length);
    response->body[body_length] = '\0';
    response->length = body_length;
  }
  free(data);
  return true;
}

static bool laghu_fetch_once(SSL_CTX *context,
                             const laghu_font_provider *provider,
                             const char *url, laghu_fetch_response *response) {
  char host[256], target[LAGHU_RUNTIME_PATH_SIZE];
  char request[LAGHU_RUNTIME_PATH_SIZE + 512U];
  laghu_socket socket = LAGHU_INVALID_SOCKET;
  SSL *tls = NULL;
  X509_VERIFY_PARAM *parameters;
  int length;
  bool success = false;
  memset(response, 0, sizeof(*response));
  if (!laghu_fetch_url(url, host, target) ||
      !laghu_font_provider_url_allowed(provider, url, false, false)) {
    fprintf(stderr, "laghu-resource-fetch: provider=%s stage=url-policy\n",
            provider->id);
    goto done;
  }
  if ((socket = laghu_fetch_connect(host)) == LAGHU_INVALID_SOCKET) {
    fprintf(stderr, "laghu-resource-fetch: provider=%s host=%s stage=connect\n",
            provider->id, host);
    goto done;
  }
  if ((tls = SSL_new(context)) == NULL) {
    fprintf(stderr, "laghu-resource-fetch: provider=%s host=%s stage=tls-new\n",
            provider->id, host);
    goto done;
  }
  parameters = SSL_get0_param(tls);
  X509_VERIFY_PARAM_set_hostflags(parameters,
                                  X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
  if (!SSL_set_tlsext_host_name(tls, host) || !SSL_set1_host(tls, host) ||
      !SSL_set_fd(tls, (int)socket) || SSL_connect(tls) != 1 ||
      SSL_get_verify_result(tls) != X509_V_OK) {
    fprintf(stderr,
            "laghu-resource-fetch: provider=%s host=%s stage=tls error=%lu "
            "verify=%ld\n",
            provider->id, host, ERR_peek_last_error(),
            SSL_get_verify_result(tls));
    goto done;
  }
  length = snprintf(request, sizeof(request),
                    "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 "
                    "LaghuFontFetch/1\r\nAccept: text/css,*/*;q=0.1\r\n"
                    "Accept-Encoding: identity\r\nConnection: close\r\n\r\n",
                    target, host);
  if (length <= 0 || (size_t)length >= sizeof(request) ||
      !laghu_fetch_send(tls, (const unsigned char *)request, (size_t)length) ||
      !laghu_fetch_read(tls, provider, response)) {
    fprintf(stderr,
            "laghu-resource-fetch: provider=%s host=%s stage=response\n",
            provider->id, host);
    goto done;
  }
  success = true;
done:
  if (tls != NULL) {
    (void)SSL_shutdown(tls);
    SSL_free(tls);
  }
  if (socket != LAGHU_INVALID_SOCKET) laghu_close(socket);
  return success;
}

static bool laghu_fetch_stylesheet(SSL_CTX *context,
                                   const laghu_font_provider *provider,
                                   const char *initial,
                                   laghu_fetch_response *response) {
  char url[LAGHU_RUNTIME_PATH_SIZE];
  unsigned int redirects;
  (void)snprintf(url, sizeof(url), "%s", initial);
  for (redirects = 0U; redirects <= LAGHU_FETCH_REDIRECT_MAX; ++redirects) {
    if (!laghu_fetch_once(context, provider, url, response)) return false;
    if (response->status < 300U || response->status >= 400U) break;
    if (response->location[0] == '\0' ||
        !laghu_font_provider_url_allowed(provider, response->location, true,
                                         false) ||
        redirects == LAGHU_FETCH_REDIRECT_MAX) {
      free(response->body);
      return false;
    }
    (void)snprintf(url, sizeof(url), "%s", response->location);
    free(response->body);
    memset(response, 0, sizeof(*response));
  }
  return response->status == 200U &&
         laghu_fetch_casecmp(response->content_type, "text/css", 8U) == 0 &&
         (response->content_type[8] == '\0' ||
          response->content_type[8] == ';' ||
          response->content_type[8] == ' ' ||
          response->content_type[8] == '\t') &&
         laghu_font_css_validate(
             provider, (laghu_buffer){response->body, response->length});
}

static bool laghu_fetch_publish_failure(const char *cache_path,
                                        const laghu_runtime_job *job,
                                        uint64_t now) {
  laghu_font_stylesheet_record record = {0};
  (void)snprintf(record.provider_id, sizeof(record.provider_id), "%s",
                 job->provider_id);
  memcpy(record.provider_digest, job->provider_digest,
         sizeof(record.provider_digest));
  (void)snprintf(record.normalized_url, sizeof(record.normalized_url), "%s",
                 job->request_path);
  record.retry_after = now + LAGHU_FETCH_RETRY_SECONDS;
  record.terminally_excluded = true;
  return laghu_font_stylesheet_publish(cache_path, &record);
}

static bool laghu_fetch_process(SSL_CTX *context,
                                const laghu_font_provider_set *providers,
                                const char *cache_path,
                                const laghu_runtime_job *job) {
  const laghu_font_provider *provider =
      laghu_font_provider_by_id(providers, job->provider_id);
  laghu_fetch_response response;
  laghu_runtime_cache_entry entry;
  laghu_font_stylesheet_record record = {0};
  char variant_key[LAGHU_RUNTIME_KEY_SIZE];
  uint64_t now = (uint64_t)time(NULL);
  if (provider == NULL || strcmp(provider->digest, job->provider_digest) != 0 ||
      !laghu_font_provider_url_allowed(provider, job->request_path, false,
                                       false) ||
      !laghu_fetch_stylesheet(context, provider, job->request_path, &response))
    return laghu_fetch_publish_failure(cache_path, job, now);
  if (!laghu_sha256_hex((laghu_buffer){response.body, response.length},
                        variant_key) ||
      !laghu_runtime_cache_publish(
          cache_path, variant_key, variant_key, variant_key, "text/css",
          LAGHU_FETCH_BACKEND, (laghu_buffer){response.body, response.length},
          &entry)) {
    free(response.body);
    return laghu_fetch_publish_failure(cache_path, job, now);
  }
  (void)snprintf(record.provider_id, sizeof(record.provider_id), "%s",
                 provider->id);
  memcpy(record.provider_digest, provider->digest,
         sizeof(record.provider_digest));
  (void)snprintf(record.normalized_url, sizeof(record.normalized_url), "%s",
                 job->request_path);
  memcpy(record.variant_key, variant_key, sizeof(record.variant_key));
  record.css_length = response.length;
  record.fetched_at = now;
  record.ttl_seconds =
      response.max_age == 0U ? provider->ttl_seconds : response.max_age;
  record.ready = true;
  free(response.body);
  return laghu_font_stylesheet_publish(cache_path, &record);
}

static int laghu_fetch_serve(const char *queue_path, const char *cache_path,
                             const char *provider_path) {
  laghu_runtime_queue queue;
  laghu_font_provider_set providers;
  SSL_CTX *context;
  char error[256];
  unsigned char payload[1];
#ifdef _WIN32
  WSADATA sockets;
  if (WSAStartup(MAKEWORD(2, 2), &sockets) != 0) return 1;
#endif
  if (!laghu_font_providers_load(provider_path, &providers, error,
                                 sizeof(error))) {
    fprintf(stderr, "laghu-resource-fetch: %s\n", error);
    return 1;
  }
  context = SSL_CTX_new(TLS_client_method());
  if (context == NULL || !SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION)
#if LAGHU_TEST_HOOKS
      || (getenv("LAGHU_TEST_FETCH_CA") != NULL
              ? !SSL_CTX_load_verify_locations(
                    context, getenv("LAGHU_TEST_FETCH_CA"), NULL)
              : !SSL_CTX_set_default_verify_paths(context))
#else
      || !SSL_CTX_set_default_verify_paths(context)
#endif
  ) {
    SSL_CTX_free(context);
    return 1;
  }
  SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
  SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
  laghu_runtime_queue_init(&queue);
  if (!laghu_runtime_queue_open(&queue, queue_path)) {
    SSL_CTX_free(context);
    return 1;
  }
  {
    unsigned int attempt;
    for (attempt = 0U; attempt < 100U; ++attempt) {
      if (laghu_runtime_queue_set_backend(&queue, 1U, LAGHU_FETCH_BACKEND))
        break;
      laghu_sleep_ms(1U);
    }
    if (attempt == 100U) {
      laghu_runtime_queue_close(&queue);
      SSL_CTX_free(context);
      return 1;
    }
  }
#ifndef _WIN32
  (void)signal(SIGINT, laghu_fetch_signal);
  (void)signal(SIGTERM, laghu_fetch_signal);
#endif
  while (!laghu_fetch_stop) {
    laghu_runtime_job job;
    (void)laghu_runtime_queue_heartbeat(&queue, (uint64_t)time(NULL));
    if (laghu_runtime_queue_try_take(&queue, &job, payload, sizeof(payload))) {
      if (job.kind == LAGHU_RUNTIME_JOB_FONT_CSS)
        (void)laghu_fetch_process(context, &providers, cache_path, &job);
    } else {
      laghu_sleep_ms(100U);
    }
  }
  laghu_runtime_queue_close(&queue);
  SSL_CTX_free(context);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}

#ifdef _WIN32
static void WINAPI laghu_fetch_service_control(DWORD control) {
  if (control != SERVICE_CONTROL_STOP && control != SERVICE_CONTROL_SHUTDOWN)
    return;
  laghu_fetch_service_status.dwCurrentState = SERVICE_STOP_PENDING;
  laghu_fetch_service_status.dwCheckPoint = 1U;
  laghu_fetch_service_status.dwWaitHint = 15000U;
  (void)SetServiceStatus(laghu_fetch_service_handle,
                         &laghu_fetch_service_status);
  laghu_fetch_stop = 1;
}

static void WINAPI laghu_fetch_service_main(DWORD argc, LPSTR *argv) {
  int result;
  (void)argc;
  (void)argv;
  memset(&laghu_fetch_service_status, 0, sizeof(laghu_fetch_service_status));
  laghu_fetch_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  laghu_fetch_service_status.dwCurrentState = SERVICE_START_PENDING;
  laghu_fetch_service_status.dwControlsAccepted =
      SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  laghu_fetch_service_handle = RegisterServiceCtrlHandlerA(
      "laghu-resource-fetch", laghu_fetch_service_control);
  if (laghu_fetch_service_handle == NULL) return;
  laghu_fetch_service_status.dwCurrentState = SERVICE_RUNNING;
  (void)SetServiceStatus(laghu_fetch_service_handle,
                         &laghu_fetch_service_status);
  result =
      laghu_fetch_serve(laghu_fetch_service_queue, laghu_fetch_service_cache,
                        laghu_fetch_service_providers);
  laghu_fetch_service_status.dwCurrentState = SERVICE_STOPPED;
  laghu_fetch_service_status.dwWin32ExitCode =
      result == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
  laghu_fetch_service_status.dwServiceSpecificExitCode =
      result == 0 ? 0U : (DWORD)result;
  (void)SetServiceStatus(laghu_fetch_service_handle,
                         &laghu_fetch_service_status);
}
#endif

int main(int argc, char **argv) {
  laghu_runtime_queue queue;
  bool initialize;
  if (argc != 5 ||
      (strcmp(argv[1], "--init") != 0 && strcmp(argv[1], "--serve") != 0 &&
       strcmp(argv[1], "--init-and-serve") != 0
#ifdef _WIN32
       && strcmp(argv[1], "--service") != 0
#endif
       )) {
    fputs(
        "Usage: laghu-resource-fetch --init|--serve|--init-and-serve "
        "QUEUE CACHE PROVIDERS\n",
        stderr);
    return 2;
  }
#ifdef _WIN32
  if (strcmp(argv[1], "--service") == 0) {
    SERVICE_TABLE_ENTRYA table[] = {
        {"laghu-resource-fetch", laghu_fetch_service_main}, {NULL, NULL}};
    laghu_fetch_service_queue = argv[2];
    laghu_fetch_service_cache = argv[3];
    laghu_fetch_service_providers = argv[4];
    return StartServiceCtrlDispatcherA(table) ? 0 : 1;
  }
#endif
  initialize = strcmp(argv[1], "--serve") != 0;
  if (initialize) {
    laghu_font_provider_set providers;
    char error[256];
    if (!laghu_font_providers_load(argv[4], &providers, error, sizeof(error))) {
      fprintf(stderr, "laghu-resource-fetch: %s\n", error);
      return 1;
    }
    laghu_runtime_queue_init(&queue);
    if (!laghu_runtime_queue_create(&queue, argv[2], 16U, 1U)) return 1;
    laghu_runtime_queue_close(&queue);
    if (strcmp(argv[1], "--init") == 0) return 0;
  }
  return laghu_fetch_serve(argv[2], argv[3], argv[4]);
}
