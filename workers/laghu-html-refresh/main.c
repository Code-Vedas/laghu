// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
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

#include "laghu/html_cache.h"
#include "laghu/queue.h"
#include "laghu/source.h"

#define LAGHU_HTML_REFRESH_BACKEND "laghu-html-refresh"
#define LAGHU_HTML_REFRESH_HEADER_MAX (64U * 1024U)
#define LAGHU_HTML_REFRESH_BODY_MAX (4U * 1024U * 1024U)
#define LAGHU_HTML_REFRESH_TIMEOUT_SECONDS 10U
#define LAGHU_HTML_REFRESH_QUEUE_SLOTS 16U

typedef struct {
  unsigned int status;
  char content_type[LAGHU_RUNTIME_TYPE_SIZE];
  char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  bool cacheable;
  unsigned char *body;
  size_t length;
} laghu_html_refresh_response;

static volatile sig_atomic_t laghu_html_refresh_stop;

static void laghu_html_refresh_signal(int signal_number) {
  (void)signal_number;
  laghu_html_refresh_stop = 1;
}

static void laghu_html_refresh_sleep_ms(unsigned int value) {
  struct timespec pause = {.tv_sec = (time_t)(value / 1000U), .tv_nsec = (long)(value % 1000U) * 1000000L};
  (void)nanosleep(&pause, NULL);
}

static bool laghu_html_refresh_has_control(const char *value) {
  const unsigned char *cursor = (const unsigned char *)value;
  if (value == NULL) return true;
  for (; *cursor != '\0'; ++cursor)
    if (*cursor < 0x20U || *cursor == 0x7fU) return true;
  return false;
}

static bool laghu_html_refresh_origin(const char *origin, char host[256]) {
  const char *authority;
  size_t length;
  if (origin == NULL || strncmp(origin, "https://", 8U) != 0) return false;
  authority = origin + 8U;
  length = strlen(authority);
  if (length == 0U || length >= 256U || strchr(authority, '/') != NULL || strchr(authority, ':') != NULL || strchr(authority, '@') != NULL ||
      strchr(authority, '?') != NULL || strchr(authority, '#') != NULL || laghu_html_refresh_has_control(authority))
    return false;
  memcpy(host, authority, length + 1U);
  return true;
}

static bool laghu_html_refresh_path(const char *path) {
  if (path == NULL || path[0] != '/' || path[1] == '/') return false;
  return !laghu_html_refresh_has_control(path) && strchr(path, '#') == NULL;
}

static void laghu_html_refresh_timeout(int socket) {
  struct timeval timeout = {LAGHU_HTML_REFRESH_TIMEOUT_SECONDS, 0};
  (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static int laghu_html_refresh_connect(const char *host) {
  struct addrinfo hints, *addresses = NULL, *item;
  int result = -1;
#if LAGHU_TEST_HOOKS
  const char *endpoint = getenv("LAGHU_TEST_HTML_REFRESH_ENDPOINT");
  char test_host[256];
  char test_port[6];
  if (endpoint != NULL) {
    const char *colon = strrchr(endpoint, ':');
    size_t length = colon == NULL ? 0U : (size_t)(colon - endpoint);
    if (length == 0U || length >= sizeof(test_host) || strlen(colon + 1U) >= sizeof(test_port)) return -1;
    memcpy(test_host, endpoint, length);
    test_host[length] = '\0';
    memcpy(test_port, colon + 1U, strlen(colon + 1U) + 1U);
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
    return -1;
  for (item = addresses; item != NULL; item = item->ai_next)
    if (!laghu_source_address_public(item->ai_addr)
#if LAGHU_TEST_HOOKS
        && endpoint == NULL
#endif
    ) {
      freeaddrinfo(addresses);
      return -1;
    }
  for (item = addresses; item != NULL; item = item->ai_next) {
    result = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
    if (result < 0) continue;
    laghu_html_refresh_timeout(result);
    if (connect(result, item->ai_addr, (socklen_t)item->ai_addrlen) == 0) break;
    (void)close(result);
    result = -1;
  }
  freeaddrinfo(addresses);
  return result;
}

static bool laghu_html_refresh_write_all(SSL *tls, const unsigned char *data, size_t length) {
  while (length != 0U) {
    int written = SSL_write(tls, data, (int)(length > 65536U ? 65536U : length));
    if (written <= 0) return false;
    data += written;
    length -= (size_t)written;
  }
  return true;
}

static int laghu_html_refresh_casecmp(const char *left, const char *right, size_t length) {
  size_t index;
  for (index = 0U; index < length; ++index) {
    int difference = tolower((unsigned char)left[index]) - tolower((unsigned char)right[index]);
    if (difference != 0) return difference;
  }
  return 0;
}

static char *laghu_html_refresh_trim(char *value) {
  char *end;
  while (*value == ' ' || *value == '\t') ++value;
  end = value + strlen(value);
  while (end > value && (end[-1] == ' ' || end[-1] == '\t')) --end;
  *end = '\0';
  return value;
}

static bool laghu_html_refresh_html_type(const char *value) {
  return laghu_html_refresh_casecmp(value, "text/html", 9U) == 0 && (value[9] == '\0' || value[9] == ';' || value[9] == ' ' || value[9] == '\t');
}

static bool laghu_html_refresh_cache_control(const char *value) {
  const char *cursor = value;
  while (*cursor != '\0') {
    const char *end = strchr(cursor, ',');
    const char *equals;
    size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
    while (length != 0U && (*cursor == ' ' || *cursor == '\t')) {
      ++cursor;
      --length;
    }
    while (length != 0U && (cursor[length - 1U] == ' ' || cursor[length - 1U] == '\t')) --length;
    equals = memchr(cursor, '=', length);
    if (equals != NULL) {
      length = (size_t)(equals - cursor);
      while (length != 0U && (cursor[length - 1U] == ' ' || cursor[length - 1U] == '\t')) --length;
    }
    if ((length == 8U && laghu_html_refresh_casecmp(cursor, "no-store", 8U) == 0) ||
        (length == 8U && laghu_html_refresh_casecmp(cursor, "no-cache", 8U) == 0) ||
        (length == 7U && laghu_html_refresh_casecmp(cursor, "private", 7U) == 0))
      return false;
    if (end == NULL) break;
    cursor = end + 1U;
  }
  return true;
}

static bool laghu_html_refresh_chunked(const unsigned char *data, size_t length, unsigned char **body, size_t *body_length) {
  unsigned char *decoded = malloc(LAGHU_HTML_REFRESH_BODY_MAX + 1U);
  size_t input = 0U, output = 0U;
  if (decoded == NULL) return false;
  while (input < length) {
    char *end = NULL;
    unsigned long chunk = strtoul((const char *)data + input, &end, 16);
    if (end == (char *)data + input || end + 2U > (char *)data + length || end[0] != '\r' || end[1] != '\n' ||
        chunk > LAGHU_HTML_REFRESH_BODY_MAX - output) {
      free(decoded);
      return false;
    }
    input = (size_t)(end - (char *)data) + 2U;
    if (chunk == 0U) {
      if (input + 2U > length || data[input] != '\r' || data[input + 1U] != '\n') {
        free(decoded);
        return false;
      }
      input += 2U;
      if (input != length) {
        free(decoded);
        return false;
      }
      decoded[output] = '\0';
      *body = decoded;
      *body_length = output;
      return true;
    }
    if (chunk > length - input || input + chunk + 2U > length || data[input + chunk] != '\r' || data[input + chunk + 1U] != '\n') {
      free(decoded);
      return false;
    }
    memcpy(decoded + output, data + input, chunk);
    output += chunk;
    input += chunk + 2U;
  }
  free(decoded);
  return false;
}

static bool laghu_html_refresh_read(SSL *tls, laghu_html_refresh_response *response) {
  unsigned char *data;
  size_t capacity = LAGHU_HTML_REFRESH_HEADER_MAX + LAGHU_HTML_REFRESH_BODY_MAX + 1U;
  size_t used = 0U, header_length, content_length = 0U;
  bool has_content_length = false, chunked = false;
  char *header_end, *line;
  data = malloc(capacity);
  if (data == NULL) return false;
  while (used + 1U < capacity) {
    int read = SSL_read(tls, data + used, (int)(capacity - used - 1U));
    if (read <= 0) break;
    used += (size_t)read;
    data[used] = '\0';
  }
  if (used + 1U == capacity) {
    free(data);
    return false;
  }
  header_end = strstr((char *)data, "\r\n\r\n");
  if (header_end == NULL || (size_t)(header_end - (char *)data) > LAGHU_HTML_REFRESH_HEADER_MAX ||
      sscanf((char *)data, "HTTP/1.%*u %u", &response->status) != 1) {
    free(data);
    return false;
  }
  header_length = (size_t)(header_end - (char *)data) + 4U;
  line = strstr((char *)data, "\r\n") + 2U;
  response->cacheable = true;
  while (line < header_end) {
    char *next = strstr(line, "\r\n");
    char *colon, *value;
    if (next == NULL || next > header_end) break;
    *next = '\0';
    colon = strchr(line, ':');
    if (colon == NULL) {
      free(data);
      return false;
    }
    *colon = '\0';
    value = laghu_html_refresh_trim(colon + 1U);
    if (strlen(line) == 12U && laghu_html_refresh_casecmp(line, "Content-Type", 12U) == 0) {
      if (strlen(value) >= sizeof(response->content_type)) {
        free(data);
        return false;
      }
      memcpy(response->content_type, value, strlen(value) + 1U);
    } else if (strlen(line) == 4U && laghu_html_refresh_casecmp(line, "ETag", 4U) == 0) {
      if (!laghu_html_refresh_has_control(value) && strlen(value) < sizeof(response->validator))
        memcpy(response->validator, value, strlen(value) + 1U);
    } else if (strlen(line) == 14U && laghu_html_refresh_casecmp(line, "Content-Length", 14U) == 0) {
      char *end = NULL;
      unsigned long parsed = strtoul(value, &end, 10);
      if (end == value || *end != '\0' || parsed > LAGHU_HTML_REFRESH_BODY_MAX) {
        free(data);
        return false;
      }
      content_length = (size_t)parsed;
      has_content_length = true;
    } else if (strlen(line) == 17U && laghu_html_refresh_casecmp(line, "Transfer-Encoding", 17U) == 0) {
      if (laghu_html_refresh_casecmp(value, "chunked", 7U) != 0 || value[7] != '\0') {
        free(data);
        return false;
      }
      chunked = true;
    } else if (strlen(line) == 16U && laghu_html_refresh_casecmp(line, "Content-Encoding", 16U) == 0 && strcmp(value, "identity") != 0) {
      free(data);
      return false;
    } else if (strlen(line) == 13U && laghu_html_refresh_casecmp(line, "Cache-Control", 13U) == 0 && !laghu_html_refresh_cache_control(value)) {
      response->cacheable = false;
    } else if ((strlen(line) == 10U && laghu_html_refresh_casecmp(line, "Set-Cookie", 10U) == 0) ||
               (strlen(line) == 4U && laghu_html_refresh_casecmp(line, "Vary", 4U) == 0)) {
      response->cacheable = false;
    }
    line = next + 2U;
  }
  if (response->status == 304U) {
    if (used != header_length) {
      free(data);
      return false;
    }
    free(data);
    return true;
  }
  if (response->status != 200U || !response->cacheable || !laghu_html_refresh_html_type(response->content_type)) {
    free(data);
    return false;
  }
  if (chunked) {
    bool success = laghu_html_refresh_chunked(data + header_length, used - header_length, &response->body, &response->length);
    free(data);
    return success && response->length != 0U;
  }
  if (!has_content_length || content_length != used - header_length) {
    free(data);
    return false;
  }
  response->length = used - header_length;
  if (response->length == 0U || response->length > LAGHU_HTML_REFRESH_BODY_MAX) {
    free(data);
    return false;
  }
  response->body = malloc(response->length);
  if (response->body != NULL) memcpy(response->body, data + header_length, response->length);
  free(data);
  return response->body != NULL;
}

static bool laghu_html_refresh_fetch(SSL_CTX *context, const char *origin, const laghu_runtime_job *job, laghu_html_refresh_response *response) {
  char host[256];
  char request[LAGHU_RUNTIME_PATH_SIZE + LAGHU_RUNTIME_VALIDATOR_SIZE + 256U];
  X509_VERIFY_PARAM *parameters;
  SSL *tls = NULL;
  int socket = -1, length;
  bool success = false;
  bool use_validator = job->validator[0] != '\0' && !laghu_html_refresh_has_control(job->validator);
  memset(response, 0, sizeof(*response));
  if (!laghu_html_refresh_origin(origin, host) || !laghu_html_refresh_path(job->request_path)) return false;
  socket = laghu_html_refresh_connect(host);
  if (socket < 0 || (tls = SSL_new(context)) == NULL) goto done;
  parameters = SSL_get0_param(tls);
  X509_VERIFY_PARAM_set_hostflags(parameters, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
  if (!SSL_set_tlsext_host_name(tls, host) || !SSL_set1_host(tls, host) || !SSL_set_fd(tls, socket) || SSL_connect(tls) != 1 ||
      SSL_get_verify_result(tls) != X509_V_OK)
    goto done;
  length =
      snprintf(request, sizeof(request),
               "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: "
               "LaghuHtmlRefresh/1\r\nAccept: text/html,*/*;q=0.1\r\n"
               "Accept-Encoding: identity\r\n%s%s%sConnection: close\r\n\r\n",
               job->request_path, host, use_validator ? "If-None-Match: " : "", use_validator ? job->validator : "", use_validator ? "\r\n" : "");
  if (length <= 0 || (size_t)length >= sizeof(request) || !laghu_html_refresh_write_all(tls, (const unsigned char *)request, (size_t)length) ||
      !laghu_html_refresh_read(tls, response))
    goto done;
  success = true;
done:
  if (tls != NULL) {
    (void)SSL_shutdown(tls);
    SSL_free(tls);
  }
  if (socket >= 0) (void)close(socket);
  return success;
}

static bool laghu_html_refresh_process(SSL_CTX *context, const char *cache_path, const char *origin, const laghu_runtime_job *job) {
  char key[LAGHU_RUNTIME_KEY_SIZE];
  laghu_html_refresh_response response;
  laghu_html_cache_record record;
  bool success;
  if (job->kind != LAGHU_RUNTIME_JOB_HTML_REFRESH || job->payload.length != 0U || !laghu_html_cache_key(origin, job->request_path, key) ||
      strcmp(key, job->index_key) != 0)
    return false;
  if (!laghu_html_refresh_fetch(context, origin, job, &response)) return false;
  if (response.status == 304U)
    return response.cacheable && laghu_html_cache_renew(cache_path, origin, job->request_path, job->validator, (uint64_t)time(NULL), &record);
  success = laghu_html_cache_publish(cache_path, origin, job->request_path, response.validator, (laghu_buffer){response.body, response.length},
                                     (uint64_t)time(NULL), &record);
  free(response.body);
  return success;
}

static int laghu_html_refresh_serve(const char *queue_path, const char *cache_path, const char *origin, bool once) {
  laghu_runtime_queue queue;
  SSL_CTX *context;
  bool configured = false;
  int result = 0;
  char host[256];
  if (!laghu_html_refresh_origin(origin, host)) return 2;
  context = SSL_CTX_new(TLS_client_method());
  if (context == NULL || !SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION)
#if LAGHU_TEST_HOOKS
      || (getenv("LAGHU_TEST_HTML_REFRESH_CA") != NULL ? !SSL_CTX_load_verify_locations(context, getenv("LAGHU_TEST_HTML_REFRESH_CA"), NULL)
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
  (void)signal(SIGINT, laghu_html_refresh_signal);
  (void)signal(SIGTERM, laghu_html_refresh_signal);
  while (!laghu_html_refresh_stop) {
    laghu_runtime_job job;
    unsigned char payload[1];
    if (!laghu_runtime_queue_open(&queue, queue_path)) {
      if (once) {
        result = 1;
        break;
      }
      laghu_html_refresh_sleep_ms(100U);
      continue;
    }
    if (!configured && !laghu_runtime_queue_set_backend(&queue, 1U, LAGHU_HTML_REFRESH_BACKEND)) {
      result = 1;
      break;
    }
    configured = true;
    (void)laghu_runtime_queue_heartbeat(&queue, (uint64_t)time(NULL));
    if (laghu_runtime_queue_try_take(&queue, &job, payload, sizeof(payload))) {
      if (!laghu_html_refresh_process(context, cache_path, origin, &job)) result = once ? 1 : result;
      if (once) break;
    } else if (once) {
      break;
    } else {
      laghu_html_refresh_sleep_ms(100U);
    }
  }
  laghu_runtime_queue_close(&queue);
  SSL_CTX_free(context);
  return result;
}

int main(int argc, char **argv) {
  laghu_runtime_queue queue;
  bool initialize, once;
  if (argc != 5 || (strcmp(argv[1], "--init") != 0 && strcmp(argv[1], "--serve") != 0 && strcmp(argv[1], "--init-and-serve") != 0 &&
                    strcmp(argv[1], "--once") != 0)) {
    fputs(
        "Usage: laghu-html-refresh --init|--serve|--init-and-serve|--once "
        "QUEUE CACHE HTTPS_ORIGIN\n",
        stderr);
    return 2;
  }
  initialize = strcmp(argv[1], "--serve") != 0 && strcmp(argv[1], "--once") != 0;
  once = strcmp(argv[1], "--once") == 0;
  if (initialize) {
    char host[256];
    if (!laghu_html_refresh_origin(argv[4], host)) return 2;
    laghu_runtime_queue_init(&queue);
    if (!laghu_runtime_queue_create(&queue, argv[2], LAGHU_HTML_REFRESH_QUEUE_SLOTS, 1U)) return 1;
    laghu_runtime_queue_close(&queue);
    if (strcmp(argv[1], "--init") == 0) return 0;
  }
  return laghu_html_refresh_serve(argv[2], argv[3], argv[4], once);
}
