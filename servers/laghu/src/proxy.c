// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/proxy.h"

#include <ctype.h>
#include <errno.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET laghu_socket;
typedef int laghu_socklen;
#define LAGHU_INVALID_SOCKET INVALID_SOCKET
#define laghu_close closesocket
#define LAGHU_SHUT_WRITE SD_SEND
#define LAGHU_SHUT_BOTH SD_BOTH
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
typedef int laghu_socket;
typedef socklen_t laghu_socklen;
#define LAGHU_INVALID_SOCKET (-1)
#define laghu_close close
#define LAGHU_SHUT_WRITE SHUT_WR
#define LAGHU_SHUT_BOTH SHUT_RDWR
#endif

#define LAGHU_PROXY_MAX_BODY LAGHU_IMAGE_MAX_INPUT_BYTES
#define LAGHU_PROXY_BEACON_BODY 16384U

typedef struct {
  char *name;
  char *value;
} proxy_header;

typedef struct {
  char storage[LAGHU_PROXY_HEADER_BYTES + 1U];
  char method[16];
  char target[LAGHU_RUNTIME_PATH_SIZE];
  char version[9];
  proxy_header headers[LAGHU_HTTP_MAX_REQUEST_HEADERS];
  size_t header_count;
  size_t content_length;
  bool has_content_length;
  bool chunked;
  bool expect;
  bool upgrade;
} proxy_request;

typedef struct {
  char storage[LAGHU_PROXY_HEADER_BYTES + 1U];
  char reason[128];
  unsigned int status;
  proxy_header headers[LAGHU_HTTP_MAX_RESPONSE_HEADERS];
  size_t header_count;
  size_t content_length;
  bool has_content_length;
  bool chunked;
} proxy_response;

typedef struct {
  laghu_socket socket;
  struct sockaddr_storage peer;
  laghu_socklen peer_length;
} proxy_connection;

typedef enum {
  PROXY_STARTING = 0,
  PROXY_RUNNING,
  PROXY_DRAINING,
  PROXY_FORCING,
  PROXY_STOPPED
} proxy_lifecycle_state;

struct proxy_worker;

typedef struct {
  const laghu_proxy_options *options;
  proxy_connection *items;
  unsigned int capacity;
  unsigned int head;
  unsigned int count;
  uint64_t beacon_second;
  uint64_t request_prefix;
  uint64_t request_counter;
  int cache_readiness;
  int optimizer_readiness;
  unsigned int beacon_count;
  unsigned int active_count;
  proxy_lifecycle_state state;
  bool stopping;
  SSL_CTX *tls_context;
  laghu_rum_engine *rum;
  laghu_operational_registry operational;
#ifdef _WIN32
  CRITICAL_SECTION lock;
  CONDITION_VARIABLE ready;
  CONDITION_VARIABLE drained;
#else
  pthread_mutex_t lock;
  pthread_cond_t ready;
  pthread_cond_t drained;
#endif
} proxy_queue;

typedef struct proxy_worker {
  proxy_queue *queue;
  laghu_runtime_queue runtime_queue;
  laghu_runtime_queue font_fetch_queue;
  laghu_runtime_queue javascript_queue;
  laghu_socket active_client;
  laghu_socket active_origin;
} proxy_worker;

typedef struct {
  uint64_t request_id;
  uint64_t started_ms;
  char method[16];
  char path[LAGHU_RUNTIME_PATH_SIZE];
  unsigned int status;
  laghu_decision decision;
  size_t input_bytes;
  size_t original_response_bytes;
  size_t output_bytes;
  const char *cache_state;
  const char *failure;
  bool job_published;
  bool javascript_defer_recommended;
  bool javascript_defer_rollback_recommended;
  char javascript_defer_path[LAGHU_RUNTIME_PATH_SIZE];
  char javascript_defer_template[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int javascript_defer_bucket;
  uint64_t javascript_defer_observations;
} proxy_access_log;

typedef struct {
  laghu_operational_snapshot snapshot;
  char output[LAGHU_OPERATIONAL_RENDER_SIZE];
} proxy_operational_response;

#ifdef _WIN32
static volatile LONG proxy_stop_requests;
static SERVICE_STATUS_HANDLE proxy_service_handle;
static SERVICE_STATUS proxy_service_status;
static const laghu_proxy_options *proxy_service_options;
#else
static volatile sig_atomic_t proxy_stop_requests;
#endif

static const char laghu_beacon_script[] =
    "addEventListener('load',()=>{document.querySelectorAll('img[src]').forEach"
    "(i=>{const r=i.getBoundingClientRect();if(r.width<1||r.height<1)return;"
    "fetch('/.laghu/beacon/images',{method:'POST',headers:{'Content-Type':"
    "'application/json'},body:JSON.stringify({url:new URL(i.currentSrc||i.src,"
    "location.href).pathname,width:Math.round(r.width),height:Math.round(r."
    "height),viewport_width:innerWidth,dpr_hundredths:Math.min(400,Math.max("
    "100,Math.round(devicePixelRatio*100))),above_fold:r.top<innerHeight,"
    "mobile:innerWidth<768}),keepalive:true})})});";

static uint64_t proxy_monotonic_ms(void);
static bool proxy_cache_probe(const char *cache_path);
static void proxy_worker_origin(proxy_worker *worker, laghu_socket origin);
static bool proxy_is_forcing(proxy_queue *queue);
static void proxy_access_init(proxy_access_log *access, proxy_queue *queue);
static void proxy_access_write(proxy_queue *queue,
                               const proxy_access_log *access);

static bool proxy_uint(const char *value, unsigned int minimum,
                       unsigned int maximum, unsigned int *output) {
  unsigned long parsed;
  char *end = NULL;
  if (value == NULL || *value == '\0') return false;
  errno = 0;
  parsed = strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < minimum ||
      parsed > maximum)
    return false;
  *output = (unsigned int)parsed;
  return true;
}

static bool proxy_size(const char *value, unsigned int minimum,
                       unsigned int maximum, unsigned int *output) {
  unsigned long long parsed, multiplier = 1U;
  char *end = NULL;
  if (value == NULL || *value == '\0') return false;
  errno = 0;
  parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value) return false;
  if (*end != '\0') {
    if (end[1] != '\0') return false;
    if (*end == 'k' || *end == 'K')
      multiplier = 1024U;
    else if (*end == 'm' || *end == 'M')
      multiplier = 1024U * 1024U;
    else
      return false;
  }
  if (parsed > maximum / multiplier) return false;
  parsed *= multiplier;
  if (parsed < minimum || parsed > maximum) return false;
  *output = (unsigned int)parsed;
  return true;
}

static bool proxy_copy(char *output, size_t capacity, const char *value) {
  size_t length = value == NULL ? 0U : strlen(value);
  if (length == 0U || length >= capacity) return false;
  memcpy(output, value, length + 1U);
  return true;
}

static bool proxy_endpoint(const char *value, char *host, size_t host_capacity,
                           char port[6]) {
  const char *separator;
  size_t host_length;
  unsigned int parsed_port;
  if (value == NULL || *value == '\0') return false;
  if (*value == '[') {
    const char *end = strchr(value, ']');
    if (end == NULL || end[1] != ':') return false;
    separator = end + 1;
    host_length = (size_t)(end - value - 1);
    ++value;
  } else {
    separator = strrchr(value, ':');
    if (separator == NULL || strchr(value, ':') != separator) return false;
    host_length = (size_t)(separator - value);
  }
  if (host_length == 0U || host_length >= host_capacity ||
      !proxy_uint(separator + 1, 1U, 65535U, &parsed_port))
    return false;
  {
    size_t index;
    for (index = 0U; index < host_length; ++index)
      if ((unsigned char)value[index] <= 32U || value[index] == '/' ||
          value[index] == '\\')
        return false;
  }
  memcpy(host, value, host_length);
  host[host_length] = '\0';
  (void)snprintf(port, 6U, "%u", parsed_port);
  return true;
}

static bool proxy_origin(const char *value, laghu_proxy_options *options) {
  const char *authority;
  const char *separator;
  size_t length;
  unsigned int port;
  if (value != NULL && strncmp(value, "http://", 7U) == 0) {
    options->origin_tls = false;
    authority = value + 7U;
    port = 80U;
  } else if (value != NULL && strncmp(value, "https://", 8U) == 0) {
    options->origin_tls = true;
    authority = value + 8U;
    port = 443U;
  } else {
    return false;
  }
  if (*authority == '\0' || strpbrk(authority, "/?#@") != NULL) return false;
  length = strlen(authority);
  if (length >= sizeof(options->origin_authority)) return false;
  memcpy(options->origin_authority, authority, length + 1U);
  if (*authority == '[') {
    const char *end = strchr(authority, ']');
    size_t host_length;
    if (end == NULL || (end[1] != '\0' && end[1] != ':')) return false;
    host_length = (size_t)(end - authority - 1);
    if (host_length == 0U || host_length >= sizeof(options->origin_host))
      return false;
    memcpy(options->origin_host, authority + 1, host_length);
    options->origin_host[host_length] = '\0';
    if (end[1] == ':' && !proxy_uint(end + 2, 1U, 65535U, &port)) return false;
  } else if ((separator = strrchr(authority, ':')) != NULL) {
    size_t host_length = (size_t)(separator - authority);
    if (host_length == 0U || host_length >= sizeof(options->origin_host) ||
        !proxy_uint(separator + 1, 1U, 65535U, &port))
      return false;
    memcpy(options->origin_host, authority, host_length);
    options->origin_host[host_length] = '\0';
  } else if (!proxy_copy(options->origin_host, sizeof(options->origin_host),
                         authority)) {
    return false;
  }
  {
    size_t index;
    for (index = 0U; options->origin_host[index] != '\0'; ++index)
      if ((unsigned char)options->origin_host[index] <= 32U ||
          options->origin_host[index] == '\\')
        return false;
  }
  (void)snprintf(options->origin_port, sizeof(options->origin_port), "%u",
                 port);
  return true;
}

static bool proxy_parse_cidr(const char *value, laghu_proxy_cidr *cidr) {
  char address[INET6_ADDRSTRLEN];
  const char *slash = value == NULL ? NULL : strrchr(value, '/');
  unsigned int maximum, prefix;
  size_t length;
  unsigned int index;
  if (slash == NULL || slash == value) return false;
  length = (size_t)(slash - value);
  if (length >= sizeof(address) || !proxy_uint(slash + 1, 0U, 128U, &prefix))
    return false;
  memcpy(address, value, length);
  address[length] = '\0';
  memset(cidr, 0, sizeof(*cidr));
  if (inet_pton(AF_INET, address, cidr->address) == 1) {
    cidr->family = AF_INET;
    maximum = 32U;
  } else if (inet_pton(AF_INET6, address, cidr->address) == 1) {
    cidr->family = AF_INET6;
    maximum = 128U;
  } else {
    return false;
  }
  if (prefix > maximum) return false;
  cidr->prefix = prefix;
  for (index = prefix; index < maximum; ++index)
    if ((cidr->address[index / 8U] &
         (unsigned char)(1U << (7U - index % 8U))) != 0U)
      return false;
  return true;
}

static bool proxy_cidr_equal(const laghu_proxy_cidr *left,
                             const laghu_proxy_cidr *right) {
  size_t length = left->family == AF_INET ? 4U : 16U;
  return left->family == right->family && left->prefix == right->prefix &&
         memcmp(left->address, right->address, length) == 0;
}

void laghu_proxy_options_init(laghu_proxy_options *options) {
  laghu_config child;
  memset(options, 0, sizeof(*options));
  laghu_config_init(&child);
  child.mode = LAGHU_MODE_ON;
  child.preset = LAGHU_PRESET_BALANCED;
  laghu_config_merge(&options->config, NULL, &child);
  options->workers = LAGHU_PROXY_DEFAULT_WORKERS;
  options->connection_queue = LAGHU_PROXY_DEFAULT_QUEUE;
  options->connect_timeout = LAGHU_PROXY_DEFAULT_CONNECT_TIMEOUT;
  options->io_timeout = LAGHU_PROXY_DEFAULT_IO_TIMEOUT;
  options->drain_timeout = LAGHU_PROXY_DEFAULT_DRAIN_TIMEOUT;
  (void)proxy_copy(options->javascript_target,
                   sizeof(options->javascript_target),
                   "defaults and supports es6-module and not dead");
  (void)proxy_copy(options->rum_store, sizeof(options->rum_store), "local:");
  options->rum_timeout_ms = LAGHU_RUM_DEFAULT_TIMEOUT_MS;
  options->rum_ttl = LAGHU_IMAGE_METADATA_TTL_DEFAULT;
  options->rum_retry_limit = LAGHU_RUM_DEFAULT_RETRY_LIMIT;
  options->rum_sync_interval = LAGHU_RUM_DEFAULT_SYNC_SECONDS;
  options->rum_memory_limit = LAGHU_RUM_DEFAULT_MEMORY_BYTES;
  options->rum_pending_limit = LAGHU_RUM_DEFAULT_PENDING_BYTES;
  laghu_cache_limits_init(&options->cache_limits);
  laghu_source_policy_init(&options->source_policy);
}

static laghu_proxy_parse_result proxy_error(char *error, size_t capacity,
                                            const char *message) {
  if (capacity != 0U) (void)snprintf(error, capacity, "%s", message);
  return LAGHU_PROXY_PARSE_ERROR;
}

laghu_proxy_parse_result laghu_proxy_parse_options(int argc, char **argv,
                                                   laghu_proxy_options *options,
                                                   char *error,
                                                   size_t error_size) {
  bool listen_seen = false, origin_seen = false, cache_seen = false;
  bool legacy_cache_seen = false, backend_cache_seen = false;
  bool cache_size_seen = false, cache_inode_seen = false;
  bool cache_clean_seen = false, cache_metadata_seen = false;
  bool queue_seen = false, selector_seen = false;
  bool font_queue_seen = false, font_config_seen = false;
  bool javascript_queue_seen = false, javascript_target_seen = false;
  bool javascript_inline_limit_seen = false;
  bool javascript_outline_threshold_seen = false;
  bool transform_memory_seen = false, transform_deadline_seen = false;
  bool variants_per_source_seen = false;
  bool instrumentation_sample_rate_seen = false;
  bool javascript_defer_suggestions_seen = false;
  bool javascript_observation_config_seen = false;
  bool javascript_defer_config_seen = false;
  bool asset_offload_seen = false, asset_queue_seen = false;
  bool quality_seen = false, workers_seen = false;
  bool connection_queue_seen = false, connect_timeout_seen = false;
  bool io_timeout_seen = false, drain_timeout_seen = false;
  bool ca_seen = false, forwarded_seen = false;
  bool respect_vary_seen = false, respect_proto_seen = false;
  bool query_overrides_seen = false;
  bool rum_store_seen = false, rum_snapshot_seen = false;
  bool rum_library_seen = false, rum_timeout_seen = false, rum_ttl_seen = false;
  bool rum_retry_seen = false, rum_sync_seen = false;
  bool rum_memory_seen = false, rum_pending_seen = false;
  bool purge_method_seen = false, purge_query_seen = false;
  bool purge_token_seen = false, flush_file_seen = false;
  bool statistics_seen = false, metrics_seen = false, readiness_seen = false;
  bool readiness_policy_seen = false;
  bool load_from_file_seen = false;
  int index;
  if (options == NULL || argc < 1)
    return proxy_error(error, error_size, "invalid arguments");
  for (index = 1; index < argc; ++index) {
    const char *name = argv[index];
    const char *value = index + 1 < argc ? argv[index + 1] : NULL;
#define NEED_VALUE()                                                    \
  do {                                                                  \
    if (value == NULL || value[0] == '-')                               \
      return proxy_error(error, error_size, "option requires a value"); \
    ++index;                                                            \
  } while (0)
    if (strcmp(name, "--help") == 0) return LAGHU_PROXY_PARSE_HELP;
    if (strcmp(name, "--version") == 0) return LAGHU_PROXY_PARSE_VERSION;
    if (strcmp(name, "--listen") == 0) {
      NEED_VALUE();
      if (listen_seen ||
          !proxy_endpoint(value, options->listen_host,
                          sizeof(options->listen_host), options->listen_port))
        return proxy_error(error, error_size, "invalid or duplicate --listen");
      listen_seen = true;
    } else if (strcmp(name, "--origin") == 0) {
      NEED_VALUE();
      if (origin_seen || !proxy_origin(value, options))
        return proxy_error(error, error_size, "invalid or duplicate --origin");
      origin_seen = true;
    } else if (strcmp(name, "--cache") == 0) {
      NEED_VALUE();
      if (cache_seen || backend_cache_seen ||
          !proxy_copy(options->cache_path, sizeof(options->cache_path), value))
        return proxy_error(error, error_size, "invalid or duplicate --cache");
      cache_seen = true;
      legacy_cache_seen = true;
    } else if (strcmp(name, "--file-cache-backend") == 0) {
      char path[LAGHU_RUNTIME_PATH_SIZE];
      NEED_VALUE();
      if (cache_seen || legacy_cache_seen ||
          !laghu_cache_backend_uri_parse(value, path, sizeof(path)) ||
          !proxy_copy(options->cache_backend_uri,
                      sizeof(options->cache_backend_uri), value) ||
          !proxy_copy(options->cache_path, sizeof(options->cache_path), path))
        return proxy_error(error, error_size,
                           "invalid or duplicate --file-cache-backend");
      cache_seen = true;
      backend_cache_seen = true;
    } else if (strcmp(name, "--file-cache-size") == 0) {
      NEED_VALUE();
      if (cache_size_seen ||
          !laghu_cache_size_parse(value, 1024U * 1024U, UINT64_C(1) << 60U,
                                  &options->cache_limits.size_limit))
        return proxy_error(error, error_size, "invalid --file-cache-size");
      cache_size_seen = true;
    } else if (strcmp(name, "--transform-memory-limit") == 0) {
      uint64_t parsed;
      NEED_VALUE();
      if (transform_memory_seen ||
          !laghu_cache_size_parse(value, LAGHU_TRANSFORM_MEMORY_LIMIT_MIN,
                                  LAGHU_TRANSFORM_MEMORY_LIMIT_MAX, &parsed))
        return proxy_error(error, error_size,
                           "invalid --transform-memory-limit");
      options->config.transform_memory_limit = (unsigned int)parsed;
      transform_memory_seen = true;
    } else if (strcmp(name, "--transform-deadline-ms") == 0) {
      NEED_VALUE();
      if (transform_deadline_seen ||
          !proxy_uint(value, LAGHU_TRANSFORM_DEADLINE_MS_MIN,
                      LAGHU_TRANSFORM_DEADLINE_MS_MAX,
                      &options->config.transform_deadline_ms))
        return proxy_error(error, error_size,
                           "invalid --transform-deadline-ms");
      transform_deadline_seen = true;
    } else if (strcmp(name, "--variants-per-source") == 0) {
      NEED_VALUE();
      if (variants_per_source_seen ||
          !proxy_uint(value, LAGHU_VARIANTS_PER_SOURCE_MIN,
                      LAGHU_VARIANTS_PER_SOURCE_MAX,
                      &options->config.variants_per_source))
        return proxy_error(error, error_size, "invalid --variants-per-source");
      variants_per_source_seen = true;
    } else if (strcmp(name, "--file-cache-inode-limit") == 0) {
      NEED_VALUE();
      if (cache_inode_seen ||
          !laghu_cache_count_parse(value, 16U, 100000000U,
                                   &options->cache_limits.inode_limit))
        return proxy_error(error, error_size,
                           "invalid --file-cache-inode-limit");
      cache_inode_seen = true;
    } else if (strcmp(name, "--file-cache-clean-interval") == 0) {
      NEED_VALUE();
      if (cache_clean_seen ||
          !laghu_cache_duration_parse(value, 1U, 86400U,
                                      &options->cache_limits.clean_interval))
        return proxy_error(error, error_size,
                           "invalid --file-cache-clean-interval");
      cache_clean_seen = true;
    } else if (strcmp(name, "--file-cache-metadata-size") == 0) {
      uint64_t parsed;
      NEED_VALUE();
      if (cache_metadata_seen ||
          !laghu_cache_size_parse(value, 16384U, 1024U * 1024U * 1024U,
                                  &parsed) ||
          parsed > SIZE_MAX)
        return proxy_error(error, error_size,
                           "invalid --file-cache-metadata-size");
      options->cache_limits.metadata_size = (size_t)parsed;
      cache_metadata_seen = true;
    } else if (strcmp(name, "--purge-method") == 0) {
      NEED_VALUE();
      if (purge_method_seen || strcmp(value, "PURGE") != 0)
        return proxy_error(error, error_size,
                           "--purge-method accepts PURGE once");
      options->purge_method = true;
      purge_method_seen = true;
    } else if (strcmp(name, "--purge-query") == 0 ||
               strcmp(name, "--statistics") == 0 ||
               strcmp(name, "--metrics") == 0 ||
               strcmp(name, "--readiness") == 0) {
      bool *target =
          strcmp(name, "--purge-query") == 0
              ? &options->purge_query
              : (strcmp(name, "--statistics") == 0
                     ? &options->statistics
                     : (strcmp(name, "--metrics") == 0 ? &options->metrics
                                                       : &options->readiness));
      bool *seen =
          strcmp(name, "--purge-query") == 0
              ? &purge_query_seen
              : (strcmp(name, "--statistics") == 0
                     ? &statistics_seen
                     : (strcmp(name, "--metrics") == 0 ? &metrics_seen
                                                       : &readiness_seen));
      NEED_VALUE();
      if (*seen || (strcmp(value, "on") != 0 && strcmp(value, "off") != 0))
        return proxy_error(error, error_size,
                           "invalid duplicate on|off option");
      *target = strcmp(value, "on") == 0;
      *seen = true;
    } else if (strcmp(name, "--readiness-policy") == 0) {
      NEED_VALUE();
      if (readiness_policy_seen ||
          (strcmp(value, "degraded") != 0 && strcmp(value, "strict") != 0))
        return proxy_error(error, error_size,
                           "invalid --readiness-policy degraded|strict");
      options->readiness_strict = strcmp(value, "strict") == 0;
      readiness_policy_seen = true;
    } else if (strcmp(name, "--purge-token-file") == 0 ||
               strcmp(name, "--cache-flush-file") == 0) {
      char *target = strcmp(name, "--purge-token-file") == 0
                         ? options->purge_token_file
                         : options->cache_flush_file;
      bool *seen = strcmp(name, "--purge-token-file") == 0 ? &purge_token_seen
                                                           : &flush_file_seen;
      NEED_VALUE();
      if (*seen || !proxy_copy(target, LAGHU_RUNTIME_PATH_SIZE, value) ||
#ifdef _WIN32
          !(isalpha((unsigned char)value[0]) && value[1] == ':' &&
            (value[2] == '\\' || value[2] == '/'))
#else
          value[0] != '/'
#endif
      )
        return proxy_error(error, error_size,
                           "invalid duplicate absolute file");
      *seen = true;
    } else if (strcmp(name, "--purge-allow") == 0) {
      laghu_proxy_cidr cidr;
      size_t cidr_index;
      NEED_VALUE();
      if (options->purge_allow_count >= LAGHU_PROXY_MAX_TRUSTED_PROXIES ||
          !proxy_parse_cidr(value, &cidr))
        return proxy_error(error, error_size, "invalid --purge-allow CIDR");
      for (cidr_index = 0U; cidr_index < options->purge_allow_count;
           ++cidr_index)
        if (proxy_cidr_equal(&options->purge_allow[cidr_index], &cidr))
          return proxy_error(error, error_size, "duplicate --purge-allow");
      options->purge_allow[options->purge_allow_count++] = cidr;
    } else if (strcmp(name, "--worker-queue") == 0) {
      NEED_VALUE();
      if (queue_seen || !proxy_copy(options->worker_queue_path,
                                    sizeof(options->worker_queue_path), value))
        return proxy_error(error, error_size,
                           "invalid or duplicate --worker-queue");
      queue_seen = true;
    } else if (strcmp(name, "--rum-store") == 0) {
      NEED_VALUE();
      if (rum_store_seen || !laghu_rum_store_validate(value, NULL, 0U) ||
          !proxy_copy(options->rum_store, sizeof(options->rum_store), value))
        return proxy_error(error, error_size,
                           "invalid or duplicate --rum-store");
      rum_store_seen = true;
    } else if (strcmp(name, "--rum-store-local-snapshot") == 0) {
      NEED_VALUE();
      if (rum_snapshot_seen ||
          !proxy_copy(options->rum_snapshot_path,
                      sizeof(options->rum_snapshot_path), value))
        return proxy_error(error, error_size,
                           "invalid --rum-store-local-snapshot");
      rum_snapshot_seen = true;
    } else if (strcmp(name, "--rum-store-client-library") == 0) {
      NEED_VALUE();
      if (rum_library_seen ||
          !proxy_copy(options->rum_client_library,
                      sizeof(options->rum_client_library), value))
        return proxy_error(error, error_size,
                           "invalid --rum-store-client-library");
      rum_library_seen = true;
    } else if (strcmp(name, "--rum-store-required") == 0) {
      if (options->rum_store_required)
        return proxy_error(error, error_size, "duplicate --rum-store-required");
      options->rum_store_required = true;
    } else if (strcmp(name, "--rum-store-timeout") == 0) {
      NEED_VALUE();
      if (rum_timeout_seen ||
          !proxy_uint(value, 10U, 10000U, &options->rum_timeout_ms))
        return proxy_error(error, error_size, "invalid --rum-store-timeout");
      rum_timeout_seen = true;
    } else if (strcmp(name, "--rum-store-ttl") == 0) {
      NEED_VALUE();
      if (rum_ttl_seen ||
          !proxy_uint(value, 3600U, 2592000U, &options->rum_ttl))
        return proxy_error(error, error_size, "invalid --rum-store-ttl");
      rum_ttl_seen = true;
    } else if (strcmp(name, "--rum-store-retry-limit") == 0) {
      NEED_VALUE();
      if (rum_retry_seen ||
          !proxy_uint(value, 0U, 10U, &options->rum_retry_limit))
        return proxy_error(error, error_size,
                           "invalid --rum-store-retry-limit");
      rum_retry_seen = true;
    } else if (strcmp(name, "--rum-store-sync-interval") == 0) {
      NEED_VALUE();
      if (rum_sync_seen ||
          !proxy_uint(value, 1U, 300U, &options->rum_sync_interval))
        return proxy_error(error, error_size,
                           "invalid --rum-store-sync-interval");
      rum_sync_seen = true;
    } else if (strcmp(name, "--rum-store-memory-limit") == 0) {
      unsigned int parsed;
      NEED_VALUE();
      if (rum_memory_seen || !proxy_size(value, 16384U, 1073741824U, &parsed))
        return proxy_error(error, error_size,
                           "invalid --rum-store-memory-limit");
      options->rum_memory_limit = parsed;
      rum_memory_seen = true;
    } else if (strcmp(name, "--rum-store-pending-limit") == 0) {
      unsigned int parsed;
      NEED_VALUE();
      if (rum_pending_seen || !proxy_size(value, 16384U, 1073741824U, &parsed))
        return proxy_error(error, error_size,
                           "invalid --rum-store-pending-limit");
      options->rum_pending_limit = parsed;
      rum_pending_seen = true;
    } else if (strcmp(name, "--font-fetch-queue") == 0) {
      NEED_VALUE();
      if (font_queue_seen ||
          !proxy_copy(options->font_fetch_queue_path,
                      sizeof(options->font_fetch_queue_path), value))
        return proxy_error(error, error_size,
                           "invalid or duplicate --font-fetch-queue");
      font_queue_seen = true;
    } else if (strcmp(name, "--font-provider-config") == 0) {
      NEED_VALUE();
      if (font_config_seen ||
          !proxy_copy(options->font_provider_config_path,
                      sizeof(options->font_provider_config_path), value) ||
          !laghu_font_providers_load(value, &options->font_providers, error,
                                     error_size))
        return proxy_error(error, error_size,
                           "invalid or duplicate --font-provider-config");
      options->font_providers_loaded = true;
      font_config_seen = true;
    } else if (strcmp(name, "--javascript-queue") == 0) {
      NEED_VALUE();
      if (javascript_queue_seen ||
          !proxy_copy(options->javascript_queue_path,
                      sizeof(options->javascript_queue_path), value))
        return proxy_error(error, error_size,
                           "invalid or duplicate --javascript-queue");
      options->javascript_queue_enabled = true;
      javascript_queue_seen = true;
    } else if (strcmp(name, "--asset-offload-config") == 0) {
      NEED_VALUE();
      if (asset_offload_seen ||
          !proxy_copy(options->asset_offload_config_path,
                      sizeof(options->asset_offload_config_path), value) ||
          !laghu_asset_config_load(value, &options->asset_offload, error,
                                   error_size))
        return proxy_error(error, error_size,
                           "invalid or duplicate --asset-offload-config");
      options->asset_offload_loaded = true;
      asset_offload_seen = true;
    } else if (strcmp(name, "--asset-upload-queue") == 0) {
      NEED_VALUE();
      if (asset_queue_seen ||
          !proxy_copy(options->asset_upload_queue_path,
                      sizeof(options->asset_upload_queue_path), value))
        return proxy_error(error, error_size,
                           "invalid or duplicate --asset-upload-queue");
      asset_queue_seen = true;
    } else if (strcmp(name, "--load-from-file") == 0) {
      NEED_VALUE();
      if (load_from_file_seen ||
          !laghu_source_mode_parse(value, false, &options->source_policy.mode))
        return proxy_error(error, error_size,
                           "invalid or duplicate --load-from-file");
      load_from_file_seen = true;
    } else if (strcmp(name, "--file-source-map") == 0) {
      const char *separator;
      char prefix[LAGHU_RUNTIME_PATH_SIZE];
      size_t prefix_length;
      NEED_VALUE();
      separator = strchr(value, '=');
      prefix_length = separator == NULL ? 0U : (size_t)(separator - value);
      if (prefix_length == 0U || prefix_length >= sizeof(prefix) ||
          separator[1] == '\0')
        return proxy_error(error, error_size, "invalid --file-source-map");
      memcpy(prefix, value, prefix_length);
      prefix[prefix_length] = '\0';
      if (!laghu_source_mapping_add(&options->source_policy, prefix,
                                    separator + 1U))
        return proxy_error(
            error, error_size,
            "invalid, duplicate, or excessive --file-source-map");
    } else if (strcmp(name, "--javascript-target") == 0) {
      char normalized[LAGHU_JAVASCRIPT_TARGET_SIZE];
      NEED_VALUE();
      if (javascript_target_seen ||
          !laghu_javascript_target_normalize(value, normalized) ||
          !proxy_copy(options->javascript_target,
                      sizeof(options->javascript_target), normalized))
        return proxy_error(error, error_size,
                           "invalid or duplicate --javascript-target");
      javascript_target_seen = true;
    } else if (strcmp(name, "--javascript-observation-config") == 0) {
      NEED_VALUE();
      if (javascript_observation_config_seen ||
          !proxy_copy(options->javascript_observation_config_path,
                      sizeof(options->javascript_observation_config_path),
                      value) ||
          !laghu_javascript_observations_load(
              value, &options->javascript_observations, error, error_size))
        return proxy_error(
            error, error_size,
            "invalid or duplicate --javascript-observation-config");
      options->javascript_observations_loaded = true;
      javascript_observation_config_seen = true;
    } else if (strcmp(name, "--javascript-defer-config") == 0) {
      NEED_VALUE();
      if (javascript_defer_config_seen ||
          !proxy_copy(options->javascript_defer_config_path,
                      sizeof(options->javascript_defer_config_path), value) ||
          !laghu_javascript_defer_load(value, &options->javascript_defer, error,
                                       error_size))
        return proxy_error(error, error_size,
                           "invalid or duplicate --javascript-defer-config");
      options->javascript_defer_loaded = true;
      javascript_defer_config_seen = true;
    } else if (strcmp(name, "--javascript-inline-limit") == 0) {
      char *end = NULL;
      unsigned long limit;
      NEED_VALUE();
      limit = strtoul(value, &end, 10);
      if (javascript_inline_limit_seen || end == value || *end != '\0' ||
          limit > 65536U)
        return proxy_error(error, error_size,
                           "invalid or duplicate --javascript-inline-limit");
      options->config.javascript_inline_limit = (unsigned int)limit;
      javascript_inline_limit_seen = true;
    } else if (strcmp(name, "--javascript-outline-threshold") == 0) {
      char *end = NULL;
      unsigned long threshold;
      NEED_VALUE();
      threshold = strtoul(value, &end, 10);
      if (javascript_outline_threshold_seen || end == value || *end != '\0' ||
          threshold < 1024U || threshold > 1048576U)
        return proxy_error(
            error, error_size,
            "invalid or duplicate --javascript-outline-threshold");
      options->config.javascript_outline_threshold = (unsigned int)threshold;
      javascript_outline_threshold_seen = true;
    } else if (strcmp(name, "--cache-mime-types") == 0) {
      NEED_VALUE();
      if (options->config.cache_mime_types[0] != '\0' ||
          strlen(value) >= sizeof(options->config.cache_mime_types))
        return proxy_error(error, error_size,
                           "invalid or duplicate --cache-mime-types");
      (void)snprintf(options->config.cache_mime_types,
                     sizeof(options->config.cache_mime_types), "%s", value);
    } else if (strcmp(name, "--preset") == 0) {
      laghu_preset preset;
      NEED_VALUE();
      if (selector_seen || !laghu_parse_preset(value, &preset))
        return proxy_error(error, error_size,
                           "invalid or conflicting --preset");
      options->config.preset = preset;
      options->config.rewrite_level = LAGHU_REWRITE_LEVEL_UNSET;
      selector_seen = true;
    } else if (strcmp(name, "--rewrite-level") == 0) {
      laghu_rewrite_level level;
      NEED_VALUE();
      if (selector_seen || !laghu_parse_rewrite_level(value, &level))
        return proxy_error(error, error_size,
                           "invalid or conflicting --rewrite-level");
      options->config.preset = LAGHU_PRESET_UNSET;
      options->config.rewrite_level = level;
      selector_seen = true;
    } else if (strcmp(name, "--enable-filter") == 0 ||
               strcmp(name, "--disable-filter") == 0 ||
               strcmp(name, "--forbid-filter") == 0) {
      uint32_t filter;
      uint32_t declared;
      NEED_VALUE();
      declared = options->config.enabled_filters |
                 options->config.disabled_filters |
                 options->config.forbidden_filters;
      if (!laghu_parse_filter(value, &filter))
        return proxy_error(error, error_size, "unknown filter name");
      if ((declared & filter) != 0U)
        return proxy_error(error, error_size,
                           "duplicate or conflicting filter control");
      if (strcmp(name, "--enable-filter") == 0)
        options->config.enabled_filters |= filter;
      else if (strcmp(name, "--disable-filter") == 0)
        options->config.disabled_filters |= filter;
      else
        options->config.forbidden_filters |= filter;
    } else if (strcmp(name, "--allow-api") == 0) {
      if (options->config.allow_api == LAGHU_MODE_ON)
        return proxy_error(error, error_size, "duplicate --allow-api");
      options->config.allow_api = LAGHU_MODE_ON;
    } else if (strcmp(name, "--allow-resources") == 0 ||
               strcmp(name, "--disallow") == 0) {
      NEED_VALUE();
      if (!laghu_resource_rule_add(
              &options->config, strcmp(name, "--allow-resources") == 0, value))
        return proxy_error(
            error, error_size,
            "invalid, duplicate, conflicting, or excessive resource rule");
    } else if (strcmp(name, "--respect-vary") == 0 ||
               strcmp(name, "--respect-x-forwarded-proto") == 0 ||
               strcmp(name, "--query-filter-overrides") == 0) {
      bool *seen = strcmp(name, "--respect-vary") == 0 ? &respect_vary_seen
                   : strcmp(name, "--respect-x-forwarded-proto") == 0
                       ? &respect_proto_seen
                       : &query_overrides_seen;
      laghu_mode *target = strcmp(name, "--respect-vary") == 0
                               ? &options->config.respect_vary
                           : strcmp(name, "--respect-x-forwarded-proto") == 0
                               ? &options->config.respect_x_forwarded_proto
                               : &options->config.query_filter_overrides;
      NEED_VALUE();
      if (*seen || (strcmp(value, "on") != 0 && strcmp(value, "off") != 0))
        return proxy_error(error, error_size,
                           "request policy toggle expects on or off once");
      *target = strcmp(value, "on") == 0 ? LAGHU_MODE_ON : LAGHU_MODE_OFF;
      *seen = true;
    } else if (strcmp(name, "--image-beacon") == 0) {
      if (options->config.image_beacon == LAGHU_MODE_ON)
        return proxy_error(error, error_size, "duplicate --image-beacon");
      options->config.image_beacon = LAGHU_MODE_ON;
    } else if (strcmp(name, "--critical-css-beacon") == 0) {
      if (options->config.critical_css_beacon == LAGHU_MODE_ON)
        return proxy_error(error, error_size,
                           "duplicate --critical-css-beacon");
      options->config.critical_css_beacon = LAGHU_MODE_ON;
    } else if (strcmp(name, "--instrumentation-beacon") == 0) {
      if (options->config.instrumentation_beacon == LAGHU_MODE_ON)
        return proxy_error(error, error_size,
                           "duplicate --instrumentation-beacon");
      options->config.instrumentation_beacon = LAGHU_MODE_ON;
    } else if (strcmp(name, "--instrumentation-sample-rate") == 0) {
      NEED_VALUE();
      if (instrumentation_sample_rate_seen ||
          !proxy_uint(value, 0U, 100U,
                      &options->config.instrumentation_sample_rate))
        return proxy_error(error, error_size,
                           "invalid --instrumentation-sample-rate");
      instrumentation_sample_rate_seen = true;
    } else if (strcmp(name, "--javascript-defer-suggestions") == 0) {
      NEED_VALUE();
      if (javascript_defer_suggestions_seen ||
          (strcmp(value, "on") != 0 && strcmp(value, "off") != 0))
        return proxy_error(error, error_size,
                           "invalid --javascript-defer-suggestions");
      options->config.javascript_defer_suggestions =
          strcmp(value, "on") == 0 ? LAGHU_MODE_ON : LAGHU_MODE_OFF;
      javascript_defer_suggestions_seen = true;
    } else if (strcmp(name, "--include-js-source-maps") == 0) {
      if (options->config.include_js_source_maps == LAGHU_MODE_ON)
        return proxy_error(error, error_size,
                           "duplicate --include-js-source-maps");
      options->config.include_js_source_maps = LAGHU_MODE_ON;
    } else if (strcmp(name, "--image-quality") == 0) {
      NEED_VALUE();
      if (quality_seen ||
          !proxy_uint(value, 1U, 100U, &options->config.image_quality))
        return proxy_error(error, error_size,
                           "invalid or duplicate --image-quality");
      quality_seen = true;
    } else if (strcmp(name, "--workers") == 0) {
      NEED_VALUE();
      if (workers_seen || !proxy_uint(value, 1U, 256U, &options->workers))
        return proxy_error(error, error_size, "invalid or duplicate --workers");
      workers_seen = true;
    } else if (strcmp(name, "--connection-queue") == 0) {
      NEED_VALUE();
      if (connection_queue_seen ||
          !proxy_uint(value, 1U, 65536U, &options->connection_queue))
        return proxy_error(error, error_size,
                           "invalid or duplicate --connection-queue");
      connection_queue_seen = true;
    } else if (strcmp(name, "--connect-timeout") == 0) {
      NEED_VALUE();
      if (connect_timeout_seen ||
          !proxy_uint(value, 1U, 300U, &options->connect_timeout))
        return proxy_error(error, error_size,
                           "invalid or duplicate --connect-timeout");
      connect_timeout_seen = true;
    } else if (strcmp(name, "--io-timeout") == 0) {
      NEED_VALUE();
      if (io_timeout_seen || !proxy_uint(value, 1U, 300U, &options->io_timeout))
        return proxy_error(error, error_size,
                           "invalid or duplicate --io-timeout");
      io_timeout_seen = true;
    } else if (strcmp(name, "--drain-timeout") == 0) {
      NEED_VALUE();
      if (drain_timeout_seen ||
          !proxy_uint(value, 1U, 300U, &options->drain_timeout))
        return proxy_error(error, error_size,
                           "invalid or duplicate --drain-timeout");
      drain_timeout_seen = true;
    } else if (strcmp(name, "--origin-ca-file") == 0) {
      NEED_VALUE();
      if (ca_seen || !proxy_copy(options->origin_ca_file,
                                 sizeof(options->origin_ca_file), value))
        return proxy_error(error, error_size,
                           "invalid or duplicate --origin-ca-file");
      ca_seen = true;
    } else if (strcmp(name, "--forwarded-headers") == 0) {
      NEED_VALUE();
      if (forwarded_seen)
        return proxy_error(error, error_size, "duplicate --forwarded-headers");
      if (strcmp(value, "off") == 0)
        options->forwarded_mode = LAGHU_PROXY_FORWARDED_OFF;
      else if (strcmp(value, "forwarded") == 0)
        options->forwarded_mode = LAGHU_PROXY_FORWARDED_STANDARD;
      else if (strcmp(value, "x-forwarded") == 0)
        options->forwarded_mode = LAGHU_PROXY_FORWARDED_X;
      else if (strcmp(value, "both") == 0)
        options->forwarded_mode = LAGHU_PROXY_FORWARDED_BOTH;
      else
        return proxy_error(error, error_size, "invalid --forwarded-headers");
      forwarded_seen = true;
    } else if (strcmp(name, "--trusted-proxy") == 0) {
      laghu_proxy_cidr parsed;
      size_t cidr_index;
      NEED_VALUE();
      if (options->trusted_proxy_count == LAGHU_PROXY_MAX_TRUSTED_PROXIES ||
          !proxy_parse_cidr(value, &parsed))
        return proxy_error(error, error_size, "invalid --trusted-proxy");
      for (cidr_index = 0U; cidr_index < options->trusted_proxy_count;
           ++cidr_index)
        if (proxy_cidr_equal(&parsed, &options->trusted_proxies[cidr_index]))
          return proxy_error(error, error_size, "duplicate --trusted-proxy");
      options->trusted_proxies[options->trusted_proxy_count++] = parsed;
    } else if (strcmp(name, "--service") == 0) {
#ifdef _WIN32
      if (options->service_mode)
        return proxy_error(error, error_size, "duplicate --service");
      options->service_mode = true;
#else
      return proxy_error(error, error_size,
                         "--service is available only on Windows");
#endif
    } else {
      return proxy_error(error, error_size, "unknown option");
    }
#undef NEED_VALUE
  }
  if (asset_queue_seen != asset_offload_seen)
    return proxy_error(
        error, error_size,
        "asset offload config and upload queue are required together");
  if (asset_offload_seen && strcmp(options->asset_offload.queue_path,
                                   options->asset_upload_queue_path) != 0)
    return proxy_error(error, error_size,
                       "asset upload queue must match the asset configuration");
  if (!laghu_source_policy_validate(&options->source_policy, false, error,
                                    error_size))
    return LAGHU_PROXY_PARSE_ERROR;
  if (options->source_policy.mode != LAGHU_SOURCE_FILE_OFF &&
      !asset_offload_seen)
    return proxy_error(error, error_size,
                       "direct file loading requires asset offload");
  if (!listen_seen || !origin_seen || !cache_seen || !queue_seen)
    return proxy_error(error, error_size,
                       "--listen, --origin, a file cache backend, and "
                       "--worker-queue are required");
  if (font_config_seen != font_queue_seen)
    return proxy_error(
        error, error_size,
        "font provider config and fetch queue require each other");
  if (ca_seen && !options->origin_tls)
    return proxy_error(error, error_size,
                       "--origin-ca-file requires an https origin");
  if (options->trusted_proxy_count != 0U &&
      options->forwarded_mode == LAGHU_PROXY_FORWARDED_OFF &&
      options->config.respect_x_forwarded_proto != LAGHU_MODE_ON)
    return proxy_error(error, error_size,
                       "--trusted-proxy requires forwarded headers");
  if (options->config.respect_x_forwarded_proto == LAGHU_MODE_ON &&
      options->trusted_proxy_count == 0U)
    return proxy_error(error, error_size,
                       "--respect-x-forwarded-proto requires --trusted-proxy");
  if ((options->purge_method || options->purge_query || options->statistics ||
       options->metrics || options->readiness) &&
      (!purge_token_seen || options->purge_allow_count == 0U))
    return proxy_error(
        error, error_size,
        "network administration requires --purge-token-file and --purge-allow");
  {
    laghu_policy policy;
    if (!laghu_resolve_config_policy(&options->config, &policy))
      return proxy_error(error, error_size, "invalid filter policy");
  }
  return LAGHU_PROXY_PARSE_OK;
}

static int proxy_hex(unsigned char byte) {
  if (byte >= '0' && byte <= '9') return byte - '0';
  if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
  if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
  return -1;
}

bool laghu_proxy_decode_chunked(laghu_buffer encoded, unsigned char *decoded,
                                size_t capacity, size_t *decoded_length) {
  size_t input = 0U, output = 0U;
  if (decoded == NULL || decoded_length == NULL ||
      (encoded.data == NULL && encoded.length != 0U))
    return false;
  while (input < encoded.length) {
    size_t size = 0U;
    bool digit = false;
    while (input < encoded.length && encoded.data[input] != '\r') {
      int value;
      if (encoded.data[input] == ';') {
        while (input < encoded.length && encoded.data[input] != '\r') ++input;
        break;
      }
      value = proxy_hex(encoded.data[input++]);
      if (value < 0 || size > (SIZE_MAX - (size_t)value) / 16U) return false;
      digit = true;
      size = size * 16U + (size_t)value;
    }
    if (!digit || input + 1U >= encoded.length || encoded.data[input] != '\r' ||
        encoded.data[input + 1U] != '\n')
      return false;
    input += 2U;
    if (size == 0U) {
      if (input + 1U == encoded.length && encoded.data[input] == '\n') {
        *decoded_length = output;
        return true;
      }
      if (input + 1U >= encoded.length || encoded.data[input] != '\r' ||
          encoded.data[input + 1U] != '\n')
        return false;
      *decoded_length = output;
      return input + 2U == encoded.length;
    }
    if (size > capacity - output || size > encoded.length - input) return false;
    memcpy(decoded + output, encoded.data + input, size);
    output += size;
    input += size;
    if (input + 1U >= encoded.length || encoded.data[input] != '\r' ||
        encoded.data[input + 1U] != '\n')
      return false;
    input += 2U;
  }
  return false;
}

static bool proxy_name_equal(const char *left, const char *right) {
  while (*left != '\0' && *right != '\0') {
    if (tolower((unsigned char)*left++) != tolower((unsigned char)*right++))
      return false;
  }
  return *left == '\0' && *right == '\0';
}

static proxy_header *proxy_find(proxy_header *headers, size_t count,
                                const char *name) {
  size_t index;
  for (index = 0U; index < count; ++index)
    if (proxy_name_equal(headers[index].name, name)) return &headers[index];
  return NULL;
}

static size_t proxy_header_count(proxy_header *headers, size_t count,
                                 const char *name) {
  size_t index, found = 0U;
  for (index = 0U; index < count; ++index)
    if (proxy_name_equal(headers[index].name, name)) ++found;
  return found;
}

static bool proxy_parse_headers(char *storage, size_t length, char **first_line,
                                proxy_header *headers, size_t *count,
                                size_t maximum) {
  char *cursor, *end;
  size_t found = 0U;
  if (length < 4U) return false;
  cursor = storage;
  end = strstr(cursor, "\r\n");
  if (end == NULL || (size_t)(end - cursor) > LAGHU_PROXY_LINE_BYTES)
    return false;
  *end = '\0';
  *first_line = cursor;
  cursor = end + 2U;
  while (*cursor != '\0') {
    char *colon;
    end = strstr(cursor, "\r\n");
    if (end == NULL) return false;
    if (end == cursor) {
      *count = found;
      return true;
    }
    if (*cursor == ' ' || *cursor == '\t' || found == maximum) return false;
    *end = '\0';
    colon = strchr(cursor, ':');
    if (colon == NULL || colon == cursor ||
        (size_t)(colon - cursor) > LAGHU_HTTP_MAX_HEADER_NAME)
      return false;
    {
      char *byte;
      for (byte = cursor; byte < colon; ++byte)
        if (!isalnum((unsigned char)*byte) &&
            strchr("!#$%&'*+-.^_`|~", *byte) == NULL)
          return false;
    }
    *colon++ = '\0';
    while (*colon == ' ' || *colon == '\t') ++colon;
    if (strlen(colon) > LAGHU_HTTP_MAX_HEADER_VALUE) return false;
    {
      const unsigned char *byte = (const unsigned char *)colon;
      while (*byte != '\0') {
        if ((*byte < 32U && *byte != '\t') || *byte == 127U) return false;
        ++byte;
      }
    }
    headers[found++] = (proxy_header){cursor, colon};
    cursor = end + 2U;
  }
  return false;
}

static bool proxy_content_length(proxy_header *headers, size_t count,
                                 size_t *value, bool *present) {
  size_t index;
  unsigned long parsed = 0U;
  bool seen = false;
  for (index = 0U; index < count; ++index)
    if (proxy_name_equal(headers[index].name, "Content-Length")) {
      char *end = NULL;
      unsigned long current;
      errno = 0;
      current = strtoul(headers[index].value, &end, 10);
      if (errno || end == headers[index].value || *end ||
          (seen && parsed != current))
        return false;
      parsed = current;
      seen = true;
    }
  *value = (size_t)parsed;
  *present = seen;
  return true;
}

static bool proxy_parse_request(proxy_request *request, size_t length) {
  char *line, *space1, *space2;
  proxy_header *host, *transfer, *expect, *upgrade;
  if (!proxy_parse_headers(request->storage, length, &line, request->headers,
                           &request->header_count,
                           LAGHU_HTTP_MAX_REQUEST_HEADERS))
    return false;
  space1 = strchr(line, ' ');
  if (space1 == NULL) return false;
  *space1++ = '\0';
  space2 = strchr(space1, ' ');
  if (space2 == NULL || strchr(space2 + 1, ' ') != NULL) return false;
  *space2++ = '\0';
  if (!proxy_copy(request->method, sizeof(request->method), line) ||
      !proxy_copy(request->target, sizeof(request->target), space1) ||
      !proxy_copy(request->version, sizeof(request->version), space2) ||
      request->target[0] != '/' || strchr(request->target, '#') != NULL ||
      (strcmp(request->version, "HTTP/1.1") &&
       strcmp(request->version, "HTTP/1.0")))
    return false;
  host = proxy_find(request->headers, request->header_count, "Host");
  if ((!strcmp(request->version, "HTTP/1.1") && host == NULL) ||
      proxy_header_count(request->headers, request->header_count, "Host") > 1U)
    return false;
  transfer =
      proxy_find(request->headers, request->header_count, "Transfer-Encoding");
  expect = proxy_find(request->headers, request->header_count, "Expect");
  upgrade = proxy_find(request->headers, request->header_count, "Upgrade");
  if (!proxy_content_length(request->headers, request->header_count,
                            &request->content_length,
                            &request->has_content_length))
    return false;
  request->expect = expect != NULL;
  request->upgrade = upgrade != NULL;
  request->chunked = transfer != NULL;
  return !(request->chunked && request->has_content_length);
}

static bool proxy_parse_response(proxy_response *response, size_t length) {
  char *line, *space1, *space2;
  proxy_header *transfer;
  unsigned int status;
  if (!proxy_parse_headers(response->storage, length, &line, response->headers,
                           &response->header_count,
                           LAGHU_HTTP_MAX_RESPONSE_HEADERS))
    return false;
  space1 = strchr(line, ' ');
  if (space1 == NULL) return false;
  *space1++ = '\0';
  space2 = strchr(space1, ' ');
  if (space2 == NULL) return false;
  *space2++ = '\0';
  if (strcmp(line, "HTTP/1.1") && strcmp(line, "HTTP/1.0")) return false;
  if (strlen(space1) != 3U || !proxy_uint(space1, 100U, 599U, &status))
    return false;
  response->status = status;
  if (!proxy_copy(response->reason, sizeof(response->reason), space2) ||
      !proxy_content_length(response->headers, response->header_count,
                            &response->content_length,
                            &response->has_content_length))
    return false;
  transfer = proxy_find(response->headers, response->header_count,
                        "Transfer-Encoding");
  response->chunked =
      transfer != NULL && proxy_name_equal(transfer->value, "chunked");
  return (transfer == NULL || response->chunked) &&
         !(response->chunked && response->has_content_length) &&
         response->status >= 200U;
}

static bool proxy_hop(const char *name) {
  return proxy_name_equal(name, "Connection") ||
         proxy_name_equal(name, "Keep-Alive") ||
         proxy_name_equal(name, "Proxy-Authenticate") ||
         proxy_name_equal(name, "Proxy-Authorization") ||
         proxy_name_equal(name, "TE") || proxy_name_equal(name, "Trailer") ||
         proxy_name_equal(name, "Transfer-Encoding") ||
         proxy_name_equal(name, "Upgrade");
}

static bool proxy_connection_nominates(const proxy_header *headers,
                                       size_t count, const char *name) {
  size_t index;
  for (index = 0U; index < count; ++index) {
    const char *cursor;
    if (!proxy_name_equal(headers[index].name, "Connection")) continue;
    cursor = headers[index].value;
    while (*cursor != '\0') {
      const char *comma = strchr(cursor, ',');
      const char *end = comma;
      const char *first = cursor;
      size_t length;
      while (*first == ' ' || *first == '\t') ++first;
      if (end == NULL) end = cursor + strlen(cursor);
      while (end > first && (end[-1] == ' ' || end[-1] == '\t')) --end;
      length = (size_t)(end - first);
      if (strlen(name) == length) {
        size_t offset;
        bool equal = true;
        for (offset = 0U; offset < length; ++offset)
          if (tolower((unsigned char)first[offset]) !=
              tolower((unsigned char)name[offset])) {
            equal = false;
            break;
          }
        if (equal) return true;
      }
      cursor = comma != NULL ? comma + 1 : end;
    }
  }
  return false;
}

static bool proxy_forwarding_name(const char *name) {
  return proxy_name_equal(name, "Forwarded") ||
         proxy_name_equal(name, "X-Forwarded-For") ||
         proxy_name_equal(name, "X-Forwarded-Proto") ||
         proxy_name_equal(name, "X-Forwarded-Host");
}

static bool proxy_peer_trusted(const laghu_proxy_options *options,
                               const proxy_connection *connection) {
  const unsigned char *peer;
  size_t index;
  unsigned int family;
  if (connection->peer.ss_family == AF_INET) {
    peer =
        (const unsigned char *)&((const struct sockaddr_in *)&connection->peer)
            ->sin_addr;
    family = AF_INET;
  } else if (connection->peer.ss_family == AF_INET6) {
    peer =
        (const unsigned char *)&((const struct sockaddr_in6 *)&connection->peer)
            ->sin6_addr;
    family = AF_INET6;
  } else {
    return false;
  }
  for (index = 0U; index < options->trusted_proxy_count; ++index) {
    const laghu_proxy_cidr *cidr = &options->trusted_proxies[index];
    unsigned int bit;
    bool equal = cidr->family == family;
    for (bit = 0U; equal && bit < cidr->prefix; ++bit)
      equal = (peer[bit / 8U] & (1U << (7U - bit % 8U))) ==
              (cidr->address[bit / 8U] & (1U << (7U - bit % 8U)));
    if (equal) return true;
  }
  return false;
}

static const char *proxy_effective_scheme(const laghu_proxy_options *options,
                                          const proxy_connection *connection,
                                          const proxy_request *request) {
  const proxy_header *header = NULL;
  size_t index;
  if (options->config.respect_x_forwarded_proto != LAGHU_MODE_ON ||
      !proxy_peer_trusted(options, connection))
    return "http";
  for (index = 0U; index < request->header_count; ++index)
    if (proxy_name_equal(request->headers[index].name, "X-Forwarded-Proto")) {
      if (header != NULL) return "http";
      header = &request->headers[index];
    }
  if (header == NULL || strchr(header->value, ',') != NULL) return "http";
  if (proxy_name_equal(header->value, "https")) return "https";
  return "http";
}

static bool proxy_peer_in_cidrs(const proxy_connection *connection,
                                const laghu_proxy_cidr *cidrs, size_t count) {
  const unsigned char *peer;
  unsigned int family;
  size_t index;
  if (connection->peer.ss_family == AF_INET) {
    peer =
        (const unsigned char *)&((const struct sockaddr_in *)&connection->peer)
            ->sin_addr;
    family = AF_INET;
  } else if (connection->peer.ss_family == AF_INET6) {
    peer =
        (const unsigned char *)&((const struct sockaddr_in6 *)&connection->peer)
            ->sin6_addr;
    family = AF_INET6;
  } else {
    return false;
  }
  for (index = 0U; index < count; ++index) {
    unsigned int bit;
    bool equal = cidrs[index].family == family;
    for (bit = 0U; equal && bit < cidrs[index].prefix; ++bit)
      equal = (peer[bit / 8U] & (1U << (7U - bit % 8U))) ==
              (cidrs[index].address[bit / 8U] & (1U << (7U - bit % 8U)));
    if (equal) return true;
  }
  return false;
}

static bool proxy_admin_token(const laghu_proxy_options *options,
                              const proxy_request *request) {
  proxy_header *provided =
      proxy_find((proxy_header *)request->headers, request->header_count,
                 "X-Laghu-Purge-Token");
  unsigned char expected[257U];
  size_t length, provided_length, index, maximum;
  unsigned char difference = 0U;
  FILE *file;
#ifndef _WIN32
  struct stat status;
  if (lstat(options->purge_token_file, &status) != 0 ||
      !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
      (status.st_mode & (S_IRWXG | S_IRWXO)) != 0U)
    return false;
#else
  DWORD attributes = GetFileAttributesA(options->purge_token_file);
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U)
    return false;
#endif
  if (provided == NULL) return false;
#ifdef _WIN32
  if (fopen_s(&file, options->purge_token_file, "rb") != 0) return false;
#else
  file = fopen(options->purge_token_file, "rb");
  if (file == NULL) return false;
#endif
  length = fread(expected, 1U, sizeof(expected), file);
  if (fclose(file) != 0 || length == 0U || length == sizeof(expected))
    return false;
  while (length != 0U &&
         (expected[length - 1U] == '\n' || expected[length - 1U] == '\r'))
    --length;
  if (length < 16U) return false;
  for (index = 0U; index < length; ++index)
    if (expected[index] <= 0x20U || expected[index] == 0x7fU) return false;
  provided_length = strlen(provided->value);
  maximum = length > provided_length ? length : provided_length;
  difference = (unsigned char)(length ^ provided_length);
  for (index = 0U; index < maximum; ++index) {
    unsigned char left = index < length ? expected[index] : 0U;
    unsigned char right =
        index < provided_length ? (unsigned char)provided->value[index] : 0U;
    difference |= (unsigned char)(left ^ right);
  }
  return difference == 0U;
}

static void proxy_poll_flush_file(const laghu_proxy_options *options) {
  if (options->cache_flush_file[0] != '\0')
    (void)laghu_cache_flush_file_poll(options->cache_path,
                                      options->cache_flush_file,
                                      (uint64_t)time(NULL), NULL);
}

static bool proxy_peer_text(const proxy_connection *connection, char *output,
                            size_t capacity, bool bracket_ipv6) {
  const void *address;
  char raw[INET6_ADDRSTRLEN];
  int family = connection->peer.ss_family;
  if (family == AF_INET)
    address = &((const struct sockaddr_in *)&connection->peer)->sin_addr;
  else if (family == AF_INET6)
    address = &((const struct sockaddr_in6 *)&connection->peer)->sin6_addr;
  else
    return false;
  if (inet_ntop(family, address, raw, sizeof(raw)) == NULL) return false;
  if (family == AF_INET6 && bracket_ipv6) {
    int count = snprintf(output, capacity, "\"[%s]\"", raw);
    return count > 0 && (size_t)count < capacity;
  }
  return proxy_copy(output, capacity, raw);
}

static const char *proxy_single_header(const proxy_request *request,
                                       const char *name) {
  const char *value = NULL;
  size_t index;
  for (index = 0U; index < request->header_count; ++index) {
    size_t offset;
    if (!proxy_name_equal(request->headers[index].name, name)) continue;
    if (value != NULL || request->headers[index].value[0] == '\0' ||
        strlen(request->headers[index].value) > LAGHU_HTTP_MAX_HEADER_VALUE)
      return NULL;
    for (offset = 0U; request->headers[index].value[offset] != '\0'; ++offset)
      if ((unsigned char)request->headers[index].value[offset] < 32U ||
          (unsigned char)request->headers[index].value[offset] == 127U)
        return NULL;
    value = request->headers[index].value;
  }
  return value;
}

static bool proxy_forwarded_value_valid(const char *value) {
  bool quoted = false, escaped = false, content = false, equals = false;
  size_t index;
  for (index = 0U; value[index] != '\0'; ++index) {
    unsigned char byte = (unsigned char)value[index];
    if (escaped) {
      escaped = false;
      content = true;
    } else if (quoted && byte == '\\') {
      escaped = true;
    } else if (byte == '"') {
      quoted = !quoted;
      content = true;
    } else if (!quoted && byte == ',') {
      if (!content || !equals) return false;
      content = false;
      equals = false;
    } else if (!quoted && byte == '=') {
      equals = true;
      content = true;
    } else if (byte != ' ' && byte != '\t') {
      content = true;
    }
  }
  return content && equals && !quoted && !escaped;
}

static bool proxy_xff_value_valid(const char *value) {
  const char *cursor = value;
  while (*cursor != '\0') {
    const char *comma = strchr(cursor, ',');
    const char *end = comma == NULL ? cursor + strlen(cursor) : comma;
    char address[INET6_ADDRSTRLEN];
    unsigned char binary[16];
    size_t length;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    while (end > cursor && (end[-1] == ' ' || end[-1] == '\t')) --end;
    length = (size_t)(end - cursor);
    if (length == 0U || length >= sizeof(address)) return false;
    memcpy(address, cursor, length);
    address[length] = '\0';
    if (inet_pton(AF_INET, address, binary) != 1 &&
        inet_pton(AF_INET6, address, binary) != 1)
      return false;
    cursor = comma == NULL ? end : comma + 1;
  }
  return true;
}

static bool proxy_append_line(char *output, size_t capacity, size_t *length,
                              const char *name, const char *existing,
                              const char *value) {
  int count = snprintf(output + *length, capacity - *length, "%s: %s%s%s\r\n",
                       name, existing == NULL ? "" : existing,
                       existing == NULL ? "" : ", ", value);
  if (count <= 0 || (size_t)count >= capacity - *length) return false;
  *length += (size_t)count;
  return true;
}

static bool proxy_append_forwarding(const laghu_proxy_options *options,
                                    const proxy_connection *connection,
                                    const proxy_request *request,
                                    const char *host, char *output,
                                    size_t capacity, size_t *length) {
  bool trusted = proxy_peer_trusted(options, connection);
  char peer[INET6_ADDRSTRLEN + 4U];
  const char *existing;
  size_t host_index;
  if (options->forwarded_mode == LAGHU_PROXY_FORWARDED_OFF) return true;
  for (host_index = 0U; host[host_index] != '\0'; ++host_index)
    if (!isalnum((unsigned char)host[host_index]) && host[host_index] != '.' &&
        host[host_index] != '-' && host[host_index] != ':' &&
        host[host_index] != '[' && host[host_index] != ']')
      return false;
  if (options->forwarded_mode == LAGHU_PROXY_FORWARDED_STANDARD ||
      options->forwarded_mode == LAGHU_PROXY_FORWARDED_BOTH) {
    char standard[INET6_ADDRSTRLEN + 300U];
    if (!proxy_peer_text(connection, peer, sizeof(peer), true) ||
        snprintf(standard, sizeof(standard), "for=%s;proto=http;host=\"%s\"",
                 peer, host) <= 0)
      return false;
    existing = trusted ? proxy_single_header(request, "Forwarded") : NULL;
    if (existing != NULL && !proxy_forwarded_value_valid(existing))
      existing = NULL;
    if (!proxy_append_line(output, capacity, length, "Forwarded", existing,
                           standard))
      return false;
  }
  if (options->forwarded_mode == LAGHU_PROXY_FORWARDED_X ||
      options->forwarded_mode == LAGHU_PROXY_FORWARDED_BOTH) {
    if (!proxy_peer_text(connection, peer, sizeof(peer), false)) return false;
    existing = trusted ? proxy_single_header(request, "X-Forwarded-For") : NULL;
    if (existing != NULL && !proxy_xff_value_valid(existing)) existing = NULL;
    if (!proxy_append_line(output, capacity, length, "X-Forwarded-For",
                           existing, peer) ||
        !proxy_append_line(output, capacity, length, "X-Forwarded-Proto", NULL,
                           "http") ||
        !proxy_append_line(output, capacity, length, "X-Forwarded-Host", NULL,
                           host))
      return false;
  }
  return true;
}

static void proxy_timeout(laghu_socket socket, unsigned int seconds) {
#ifdef _WIN32
  DWORD value = seconds * 1000U;
  (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&value,
                   sizeof(value));
  (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&value,
                   sizeof(value));
#else
  struct timeval value = {(time_t)seconds, 0};
  (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
  (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
#endif
}

static bool proxy_send_all(laghu_socket socket, const void *data,
                           size_t length) {
  const char *bytes = data;
  while (length != 0U) {
    int sent = send(socket, bytes, (int)(length > 65536U ? 65536U : length), 0);
    if (sent <= 0) return false;
    bytes += sent;
    length -= (size_t)sent;
  }
  return true;
}

static bool proxy_socket_timed_out(void) {
#ifdef _WIN32
  int error = WSAGetLastError();
  return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT;
#endif
}

static int proxy_origin_recv(laghu_socket socket, SSL *tls, void *data,
                             size_t length) {
  if (tls == NULL) return recv(socket, data, (int)length, 0);
  return SSL_read(tls, data, (int)length);
}

static bool proxy_origin_send_all(laghu_socket socket, SSL *tls,
                                  const void *data, size_t length) {
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

static SSL_CTX *proxy_tls_context(const laghu_proxy_options *options) {
  SSL_CTX *context;
  if (!options->origin_tls) return NULL;
  context = SSL_CTX_new(TLS_client_method());
  if (context == NULL) return NULL;
  if (!SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) ||
      !SSL_CTX_set_default_verify_paths(context) ||
      (options->origin_ca_file[0] != '\0' &&
       !SSL_CTX_load_verify_locations(context, options->origin_ca_file,
                                      NULL))) {
    SSL_CTX_free(context);
    return NULL;
  }
  SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
  SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
  return context;
}

static SSL *proxy_tls_handshake(proxy_worker *worker, laghu_socket socket,
                                const char *host, unsigned int timeout,
                                bool *timed_out) {
  static const unsigned char alpn[] = {8U,  'h', 't', 't', 'p',
                                       '/', '1', '.', '1'};
  SSL *tls = SSL_new(worker->queue->tls_context);
  X509_VERIFY_PARAM *parameters;
  bool ip_literal;
  bool complete = false;
  uint64_t deadline;
#ifdef _WIN32
  u_long nonblocking = 1U;
#else
  int flags = fcntl(socket, F_GETFL, 0);
#endif
  *timed_out = false;
  if (tls == NULL) return NULL;
  ip_literal = inet_pton(AF_INET, host, (unsigned char[4]){0}) == 1 ||
               inet_pton(AF_INET6, host, (unsigned char[16]){0}) == 1;
  parameters = SSL_get0_param(tls);
  if ((ip_literal && !X509_VERIFY_PARAM_set1_ip_asc(parameters, host)) ||
      (!ip_literal &&
       (!SSL_set_tlsext_host_name(tls, host) || !SSL_set1_host(tls, host))) ||
      SSL_set_alpn_protos(tls, alpn, sizeof(alpn)) != 0 ||
      !SSL_set_fd(tls, (int)socket)) {
    SSL_free(tls);
    return NULL;
  }
#ifdef _WIN32
  if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
#else
  if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0) {
#endif
    SSL_free(tls);
    return NULL;
  }
  deadline = proxy_monotonic_ms() + (uint64_t)timeout * 1000U;
  for (;;) {
    int result = SSL_connect(tls);
    int error;
    fd_set set;
    struct timeval wait = {0, 200000};
    if (result == 1) {
      complete = true;
      break;
    }
    error = SSL_get_error(tls, result);
    if (proxy_monotonic_ms() >= deadline) {
      *timed_out = true;
#ifdef _WIN32
      WSASetLastError(WSAETIMEDOUT);
#else
      errno = ETIMEDOUT;
#endif
      break;
    }
    if ((error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) ||
        proxy_is_forcing(worker->queue)) {
      break;
    }
    FD_ZERO(&set);
    FD_SET(socket, &set);
#ifdef _WIN32
    (void)select(0, error == SSL_ERROR_WANT_READ ? &set : NULL,
                 error == SSL_ERROR_WANT_WRITE ? &set : NULL, NULL, &wait);
#else
    (void)select(socket + 1, error == SSL_ERROR_WANT_READ ? &set : NULL,
                 error == SSL_ERROR_WANT_WRITE ? &set : NULL, NULL, &wait);
#endif
  }
#ifdef _WIN32
  nonblocking = 0U;
  (void)ioctlsocket(socket, FIONBIO, &nonblocking);
#else
  (void)fcntl(socket, F_SETFL, flags);
#endif
  if (!complete) {
    SSL_free(tls);
    return NULL;
  }
  {
    const unsigned char *selected = NULL;
    unsigned int selected_length = 0U;
    SSL_get0_alpn_selected(tls, &selected, &selected_length);
    if (SSL_get_verify_result(tls) != X509_V_OK ||
        (selected_length != 0U &&
         (selected_length != 8U || memcmp(selected, "http/1.1", 8U) != 0))) {
      SSL_free(tls);
      return NULL;
    }
  }
  return tls;
}

static bool proxy_read_headers(laghu_socket socket, char *buffer, SSL *tls,
                               size_t *length, unsigned char **body_start,
                               size_t *body_initial) {
  size_t used = 0U;
  while (used < LAGHU_PROXY_HEADER_BYTES) {
    int got = proxy_origin_recv(socket, tls, buffer + used,
                                LAGHU_PROXY_HEADER_BYTES - used);
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

static laghu_socket proxy_connect(proxy_worker *worker, const char *host,
                                  const char *port, unsigned int timeout) {
  struct addrinfo hints, *addresses = NULL, *address;
  laghu_socket descriptor = LAGHU_INVALID_SOCKET;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, port, &hints, &addresses) != 0) return descriptor;
  for (address = addresses; address != NULL; address = address->ai_next) {
    descriptor = (laghu_socket)socket(address->ai_family, address->ai_socktype,
                                      address->ai_protocol);
    if (descriptor == LAGHU_INVALID_SOCKET) continue;
    proxy_worker_origin(worker, descriptor);
    {
      bool connected = false;
#ifdef _WIN32
      u_long nonblocking = 1U;
      int result;
      (void)ioctlsocket(descriptor, FIONBIO, &nonblocking);
      result = connect(descriptor, address->ai_addr,
                       (laghu_socklen)address->ai_addrlen);
      if (result == 0) {
        connected = true;
      } else {
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS) {
#else
      int flags = fcntl(descriptor, F_GETFL, 0);
      int result;
      if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        proxy_worker_origin(worker, LAGHU_INVALID_SOCKET);
        laghu_close(descriptor);
        descriptor = LAGHU_INVALID_SOCKET;
        continue;
      }
      result = connect(descriptor, address->ai_addr,
                       (laghu_socklen)address->ai_addrlen);
      if (result == 0) {
        connected = true;
      } else if (errno == EINPROGRESS) {
#endif
          uint64_t deadline = proxy_monotonic_ms() + (uint64_t)timeout * 1000U;
          int socket_error = 0;
          laghu_socklen error_length = (laghu_socklen)sizeof(socket_error);
          while (!proxy_is_forcing(worker->queue) &&
                 proxy_monotonic_ms() < deadline) {
            fd_set writable;
            struct timeval wait = {0, 200000};
            int selected;
            FD_ZERO(&writable);
            FD_SET(descriptor, &writable);
#ifdef _WIN32
            selected = select(0, NULL, &writable, NULL, &wait);
#else
          selected = select(descriptor + 1, NULL, &writable, NULL, &wait);
#endif
            if (selected > 0 &&
                getsockopt(descriptor, SOL_SOCKET, SO_ERROR,
#ifdef _WIN32
                           (char *)&socket_error,
#else
                         &socket_error,
#endif
                           &error_length) == 0 &&
                socket_error == 0) {
              connected = true;
              break;
            }
            if (selected < 0) break;
          }
#ifdef _WIN32
        }
      }
      nonblocking = 0U;
      (void)ioctlsocket(descriptor, FIONBIO, &nonblocking);
#else
      }
      (void)fcntl(descriptor, F_SETFL, flags);
#endif
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

static void proxy_error_response(laghu_socket client, unsigned int status,
                                 const char *reason) {
  char response[512];
  int length = snprintf(response, sizeof(response),
                        "HTTP/1.1 %u %s\r\nContent-Length: 0\r\nConnection: "
                        "close\r\nX-Laghu: bypass-error\r\n\r\n",
                        status, reason);
  if (length > 0) (void)proxy_send_all(client, response, (size_t)length);
}

static void proxy_reject_connection(proxy_queue *queue, laghu_socket client,
                                    const char *failure) {
  unsigned char discarded[4096U];
  proxy_access_log access;
  proxy_access_init(&access, queue);
  access.status = 503U;
  access.failure = failure;
  proxy_error_response(client, 503U, "Service Unavailable");
  (void)shutdown(client, LAGHU_SHUT_WRITE);
#ifdef _WIN32
  {
    proxy_timeout(client, 1U);
    while (recv(client, (char *)discarded, sizeof(discarded), 0) > 0) {
    }
  }
#else
  while (recv(client, discarded, sizeof(discarded), MSG_DONTWAIT) > 0) {
  }
#endif
  laghu_close(client);
  proxy_access_write(queue, &access);
}

static bool proxy_read_body(laghu_socket socket, SSL *tls,
                            const unsigned char *initial, size_t initial_length,
                            size_t expected, bool to_close,
                            unsigned char **body, size_t *length) {
  size_t capacity = expected != 0U ? expected : 65536U, used = 0U;
  unsigned char *data;
  if (capacity > LAGHU_PROXY_MAX_BODY) return false;
  data = malloc(capacity == 0U ? 1U : capacity);
  if (data == NULL) return false;
  if (initial_length > capacity) {
    free(data);
    return false;
  }
  memcpy(data, initial, initial_length);
  used = initial_length;
  while ((expected != 0U && used < expected) || (expected == 0U && to_close)) {
    int got;
    if (used == capacity) {
      size_t grown = capacity * 2U;
      unsigned char *replacement;
      if (grown > LAGHU_PROXY_MAX_BODY) grown = LAGHU_PROXY_MAX_BODY;
      if (grown == capacity) {
        free(data);
        return false;
      }
      replacement = realloc(data, grown);
      if (replacement == NULL) {
        free(data);
        return false;
      }
      data = replacement;
      capacity = grown;
    }
    got = proxy_origin_recv(socket, tls, data + used, capacity - used);
    if (got == 0 && to_close) break;
    if (got <= 0) {
      free(data);
      return false;
    }
    used += (size_t)got;
  }
  if (expected != 0U && used != expected) {
    free(data);
    return false;
  }
  *body = data;
  *length = used;
  return true;
}

static bool proxy_operation_removes(const laghu_http_transaction_result *result,
                                    const char *name) {
  size_t index;
  for (index = 0U; index < result->header_operation_count; ++index)
    if (proxy_name_equal(result->header_operations[index].name, name) &&
        (result->header_operations[index].kind == LAGHU_HTTP_HEADER_REMOVE ||
         result->header_operations[index].kind == LAGHU_HTTP_HEADER_SET))
      return true;
  return false;
}

static bool proxy_beacon_allowed(proxy_queue *queue, uint64_t now) {
  bool allowed;
#ifdef _WIN32
  EnterCriticalSection(&queue->lock);
#else
  pthread_mutex_lock(&queue->lock);
#endif
  if (queue->beacon_second != now) {
    queue->beacon_second = now;
    queue->beacon_count = 0U;
  }
  allowed = ++queue->beacon_count <= 32U;
#ifdef _WIN32
  LeaveCriticalSection(&queue->lock);
#else
  pthread_mutex_unlock(&queue->lock);
#endif
  return allowed;
}

static bool proxy_send_headers(laghu_socket client,
                               const proxy_response *origin,
                               const laghu_http_transaction_result *result,
                               size_t content_length, bool has_content_length) {
  char line[16384];
  size_t index;
  int count = snprintf(line, sizeof(line), "HTTP/1.1 %u %s\r\n", origin->status,
                       origin->reason[0] ? origin->reason : "OK");
  if (count <= 0 || !proxy_send_all(client, line, (size_t)count)) return false;
  for (index = 0U; index < origin->header_count; ++index) {
    if (proxy_hop(origin->headers[index].name) ||
        proxy_connection_nominates(origin->headers, origin->header_count,
                                   origin->headers[index].name) ||
        proxy_name_equal(origin->headers[index].name, "Content-Length") ||
        proxy_operation_removes(result, origin->headers[index].name))
      continue;
    count = snprintf(line, sizeof(line), "%s: %s\r\n",
                     origin->headers[index].name, origin->headers[index].value);
    if (count <= 0 || (size_t)count >= sizeof(line) ||
        !proxy_send_all(client, line, (size_t)count))
      return false;
  }
  for (index = 0U; index < result->header_operation_count; ++index) {
    const laghu_http_header_operation *operation =
        &result->header_operations[index];
    if (operation->kind == LAGHU_HTTP_HEADER_REMOVE ||
        proxy_name_equal(operation->name, "Content-Length"))
      continue;
    count = snprintf(line, sizeof(line), "%s: %s\r\n", operation->name,
                     operation->value == NULL ? "" : operation->value);
    if (count <= 0 || (size_t)count >= sizeof(line) ||
        !proxy_send_all(client, line, (size_t)count))
      return false;
  }
  count = has_content_length
              ? snprintf(line, sizeof(line),
                         "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                         content_length)
              : snprintf(line, sizeof(line), "Connection: close\r\n\r\n");
  return count > 0 && proxy_send_all(client, line, (size_t)count);
}

static bool proxy_send_result(laghu_socket client, const proxy_response *origin,
                              const laghu_http_transaction_result *result,
                              laghu_buffer body) {
  return proxy_send_headers(client, origin, result, body.length, true) &&
         (body.length == 0U || proxy_send_all(client, body.data, body.length));
}

static bool proxy_stream_body(laghu_socket origin, SSL *tls,
                              laghu_socket client, const unsigned char *initial,
                              size_t initial_length, size_t expected,
                              bool until_close) {
  unsigned char buffer[65536U];
  size_t sent = 0U;
  if (expected != 0U && initial_length > expected) return false;
  if (initial_length != 0U && !proxy_send_all(client, initial, initial_length))
    return false;
  sent = initial_length;
  while ((expected != 0U && sent < expected) || until_close) {
    size_t wanted = sizeof(buffer);
    int got;
    if (expected != 0U && wanted > expected - sent) wanted = expected - sent;
    got = proxy_origin_recv(origin, tls, buffer, wanted);
    if (got == 0 && until_close) return true;
    if (got <= 0) return false;
    if (!proxy_send_all(client, buffer, (size_t)got)) return false;
    sent += (size_t)got;
  }
  return expected == 0U || sent == expected;
}

static void proxy_queue_lock(proxy_queue *queue) {
#ifdef _WIN32
  EnterCriticalSection(&queue->lock);
#else
  pthread_mutex_lock(&queue->lock);
#endif
}

static void proxy_queue_unlock(proxy_queue *queue) {
#ifdef _WIN32
  LeaveCriticalSection(&queue->lock);
#else
  pthread_mutex_unlock(&queue->lock);
#endif
}

static void proxy_worker_origin(proxy_worker *worker, laghu_socket origin) {
  proxy_queue_lock(worker->queue);
  worker->active_origin = origin;
  proxy_queue_unlock(worker->queue);
}

static bool proxy_is_forcing(proxy_queue *queue) {
  bool forcing;
  proxy_queue_lock(queue);
  forcing = queue->state == PROXY_FORCING;
  proxy_queue_unlock(queue);
  return forcing;
}

static proxy_lifecycle_state proxy_state(proxy_queue *queue) {
  proxy_lifecycle_state state;
  proxy_queue_lock(queue);
  state = queue->state;
  proxy_queue_unlock(queue);
  return state;
}

static const char *proxy_state_name(proxy_lifecycle_state state) {
  switch (state) {
    case PROXY_STARTING:
      return "starting";
    case PROXY_RUNNING:
      return "running";
    case PROXY_DRAINING:
      return "draining";
    case PROXY_FORCING:
      return "forcing";
    case PROXY_STOPPED:
      return "stopped";
  }
  return "unknown";
}

static void proxy_send_json(laghu_socket client, unsigned int status,
                            const char *reason, const char *json, bool head) {
  char headers[512];
  size_t length = strlen(json);
  int count = snprintf(headers, sizeof(headers),
                       "HTTP/1.1 %u %s\r\nContent-Type: application/json\r\n"
                       "Content-Length: %zu\r\nCache-Control: no-store\r\n"
                       "Connection: close\r\n\r\n",
                       status, reason, length);
  if (count > 0 && (size_t)count < sizeof(headers) &&
      proxy_send_all(client, headers, (size_t)count) && !head)
    (void)proxy_send_all(client, json, length);
}

static void proxy_send_admin_json(laghu_socket client, unsigned int status,
                                  const char *reason, const char *json,
                                  bool head) {
  char headers[512];
  size_t length = strlen(json);
  int count = snprintf(headers, sizeof(headers),
                       "HTTP/1.1 %u %s\r\nContent-Type: application/json\r\n"
                       "Cache-Control: no-store\r\nContent-Length: %zu\r\n"
                       "Connection: close\r\n\r\n",
                       status, reason, length);
  if (count > 0 && (size_t)count < sizeof(headers)) {
    (void)proxy_send_all(client, headers, (size_t)count);
    if (!head) (void)proxy_send_all(client, json, length);
  }
}

static void proxy_send_metrics(laghu_socket client, const char *body,
                               size_t length, bool head) {
  char headers[512];
  int count = snprintf(headers, sizeof(headers),
                       "HTTP/1.1 200 OK\r\nContent-Type: text/plain; "
                       "version=0.0.4; charset=utf-8\r\n"
                       "Cache-Control: no-store\r\nContent-Length: %zu\r\n"
                       "Connection: close\r\n\r\n",
                       length);
  if (count > 0 && (size_t)count < sizeof(headers)) {
    (void)proxy_send_all(client, headers, (size_t)count);
    if (!head) (void)proxy_send_all(client, body, length);
  }
}

static bool proxy_json_escape(const char *input, char *output,
                              size_t capacity) {
  size_t used = 0U;
  const unsigned char *cursor = (const unsigned char *)input;
  if (capacity == 0U) return false;
  while (*cursor != '\0') {
    const char *escape = NULL;
    char unicode[7];
    size_t length;
    if (*cursor == '"')
      escape = "\\\"";
    else if (*cursor == '\\')
      escape = "\\\\";
    else if (*cursor == '\n')
      escape = "\\n";
    else if (*cursor == '\r')
      escape = "\\r";
    else if (*cursor == '\t')
      escape = "\\t";
    else if (*cursor < 32U) {
      (void)snprintf(unicode, sizeof(unicode), "\\u%04x", *cursor);
      escape = unicode;
    }
    length = escape == NULL ? 1U : strlen(escape);
    if (used + length >= capacity) return false;
    if (escape == NULL)
      output[used++] = (char)*cursor;
    else {
      memcpy(output + used, escape, length);
      used += length;
    }
    ++cursor;
  }
  output[used] = '\0';
  return true;
}

static void proxy_timestamp(char output[32]) {
  time_t now = time(NULL);
  struct tm value;
#ifdef _WIN32
  (void)gmtime_s(&value, &now);
#else
  (void)gmtime_r(&now, &value);
#endif
  if (strftime(output, 32U, "%Y-%m-%dT%H:%M:%SZ", &value) == 0U)
    memcpy(output, "1970-01-01T00:00:00Z", 21U);
}

static void proxy_log_line(proxy_queue *queue, const char *line) {
  if (queue != NULL) proxy_queue_lock(queue);
  (void)fwrite(line, 1U, strlen(line), stderr);
  (void)fputc('\n', stderr);
  (void)fflush(stderr);
  if (queue != NULL) proxy_queue_unlock(queue);
}

static void proxy_log_event(proxy_queue *queue, const char *event,
                            const char *state) {
  char timestamp[32], line[512];
  proxy_timestamp(timestamp);
  (void)snprintf(line, sizeof(line),
                 "{\"timestamp\":\"%s\",\"event\":\"%s\","
                 "\"state\":\"%s\"}",
                 timestamp, event, state);
  proxy_log_line(queue, line);
}

static void proxy_log_startup_failure(proxy_queue *queue, const char *failure) {
  char timestamp[32], line[512];
  proxy_timestamp(timestamp);
  (void)snprintf(line, sizeof(line),
                 "{\"timestamp\":\"%s\",\"event\":\"startup_failure\","
                 "\"state\":\"stopped\",\"failure\":\"%s\"}",
                 timestamp, failure);
  proxy_log_line(queue, line);
}

static void proxy_access_init(proxy_access_log *access, proxy_queue *queue) {
  memset(access, 0, sizeof(*access));
  access->started_ms = proxy_monotonic_ms();
  access->decision = LAGHU_DECISION_BYPASS_ERROR;
  access->cache_state = "none";
  access->failure = "none";
  proxy_queue_lock(queue);
  access->request_id = queue->request_prefix + ++queue->request_counter;
  proxy_queue_unlock(queue);
}

static void proxy_access_write(proxy_queue *queue,
                               const proxy_access_log *access) {
  char timestamp[32], method[64], path[LAGHU_RUNTIME_PATH_SIZE * 2U];
  char defer_path[LAGHU_RUNTIME_PATH_SIZE * 2U];
  char line[LAGHU_RUNTIME_PATH_SIZE * 4U + 1400U];
  uint64_t elapsed = proxy_monotonic_ms() - access->started_ms;
  (void)laghu_operational_registry_heartbeat(
      &queue->operational, (uint64_t)time(NULL), true, 0U, 0U);
  laghu_operational_decision operational_decision =
      access->output_bytes < access->original_response_bytes
          ? LAGHU_OPERATIONAL_DECISION_OPTIMIZED
          : LAGHU_OPERATIONAL_DECISION_ORIGINAL;
  if (access->decision != LAGHU_DECISION_PASS &&
      access->decision != LAGHU_DECISION_IMAGE_HIT)
    operational_decision = LAGHU_OPERATIONAL_DECISION_BYPASS;
  if (strcmp(access->cache_state, "hit") == 0)
    operational_decision = LAGHU_OPERATIONAL_DECISION_CACHED;
  else if (access->job_published)
    operational_decision = LAGHU_OPERATIONAL_DECISION_QUEUED;
  laghu_operational_registry_record(&queue->operational, operational_decision,
                                    access->original_response_bytes,
                                    access->output_bytes,
                                    elapsed * UINT64_C(1000));
  if (strcmp(access->failure, "none") != 0)
    laghu_operational_registry_failure(&queue->operational,
                                       LAGHU_OPERATIONAL_FAILURE_TRANSPORT);
  proxy_timestamp(timestamp);
  if (!proxy_json_escape(access->method[0] ? access->method : "unknown", method,
                         sizeof(method)) ||
      !proxy_json_escape(access->path[0] ? access->path : "/", path,
                         sizeof(path)) ||
      !proxy_json_escape(access->javascript_defer_path, defer_path,
                         sizeof(defer_path)))
    return;
  (void)snprintf(
      line, sizeof(line),
      "{\"timestamp\":\"%s\",\"event\":\"transaction\","
      "\"request_id\":\"%016llx\",\"method\":\"%s\","
      "\"path\":\"%s\",\"status\":%u,\"decision\":\"%s\","
      "\"input_bytes\":%zu,\"output_bytes\":%zu,"
      "\"duration_ms\":%llu,\"cache\":\"%s\","
      "\"job_published\":%s,\"failure\":\"%s\","
      "\"javascript_defer_recommended\":%s,"
      "\"javascript_defer_rollback_recommended\":%s,"
      "\"javascript_defer_path\":\"%s\","
      "\"javascript_defer_template\":\"%s\","
      "\"javascript_defer_bucket\":%u,"
      "\"javascript_defer_observations\":%llu}",
      timestamp, (unsigned long long)access->request_id, method, path,
      access->status, laghu_decision_name(access->decision),
      access->input_bytes, access->output_bytes, (unsigned long long)elapsed,
      access->cache_state, access->job_published ? "true" : "false",
      access->failure, access->javascript_defer_recommended ? "true" : "false",
      access->javascript_defer_rollback_recommended ? "true" : "false",
      defer_path, access->javascript_defer_template,
      access->javascript_defer_bucket,
      (unsigned long long)access->javascript_defer_observations);
  proxy_log_line(queue, line);
}

static void proxy_handle(const proxy_connection *connection,
                         proxy_worker *worker) {
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
  {
    bool purge_control = false;
    char purge_target[LAGHU_RUNTIME_PATH_SIZE];
    bool normalized = laghu_cache_source_normalize(
        request.target, purge_target, sizeof(purge_target), &purge_control);
    bool purge_request = strcmp(request.method, "PURGE") == 0 ||
                         purge_control ||
                         strstr(request.target, "laghu=purge") != NULL;
    bool stats_request =
        normalized && strcmp(purge_target, "/.laghu/stats") == 0;
    bool metrics_request =
        normalized && strcmp(purge_target, "/.laghu/metrics") == 0;
    bool readiness_request =
        normalized && strcmp(purge_target, "/.laghu/ready") == 0;
    if ((metrics_request && !options->metrics) ||
        (readiness_request && !options->readiness)) {
      PROXY_FAIL(404U, "Not Found", "request_limit");
      goto done;
    }
    if (purge_request || stats_request || metrics_request ||
        readiness_request) {
      bool head = strcmp(request.method, "HEAD") == 0;
      bool authorized = proxy_peer_in_cidrs(connection, options->purge_allow,
                                            options->purge_allow_count) &&
                        proxy_admin_token(options, &request);
      if (!normalized) {
        proxy_send_admin_json(client, 400U, "Bad Request",
                              "{\"status\":\"malformed\"}", false);
        access.status = 400U;
        access.failure = "admin_malformed";
      } else if (!authorized) {
        proxy_send_admin_json(client, 403U, "Forbidden",
                              "{\"status\":\"forbidden\"}", head);
        access.status = 403U;
        access.failure = "admin_auth";
      } else if (metrics_request || readiness_request) {
        (void)laghu_operational_registry_heartbeat(
            &worker->queue->operational, (uint64_t)time(NULL), true, 0U, 0U);
        proxy_operational_response *operational =
            calloc(1U, sizeof(*operational));
        laghu_cache_stats stats = {0};
        size_t output_length = 0U;
        bool enabled = metrics_request ? options->metrics : options->readiness;
        bool method = strcmp(request.method, "GET") == 0 || head;
        if (operational == NULL) {
          proxy_send_admin_json(client, 503U, "Service Unavailable",
                                "{\"status\":\"unavailable\"}", head);
          access.status = 503U;
          access.failure = "runtime";
        } else if (!enabled || !method) {
          proxy_send_admin_json(client, 405U, "Method Not Allowed",
                                "{\"status\":\"method_not_allowed\"}", head);
          access.status = 405U;
          access.failure = "admin_method";
        } else if (!laghu_operational_registry_snapshot(
                       &worker->queue->operational, &operational->snapshot)) {
          proxy_send_admin_json(client, 503U, "Service Unavailable",
                                "{\"status\":\"unavailable\"}", head);
          access.status = 503U;
          access.failure = "runtime";
        } else if (metrics_request &&
                   laghu_operational_render_prometheus(
                       &operational->snapshot, (uint64_t)time(NULL),
                       operational->output, sizeof(operational->output),
                       &output_length)) {
          proxy_send_metrics(client, operational->output, output_length, head);
          access.status = 200U;
          access.output_bytes = head ? 0U : output_length;
        } else if (readiness_request) {
          laghu_operational_readiness readiness;
          bool cache_ready =
              proxy_cache_probe(options->cache_path) &&
              laghu_cache_backend_health_path(options->cache_path, &stats);
          laghu_operational_registry_cache(&worker->queue->operational, &stats);
          if (!laghu_operational_readiness_evaluate(
                  &operational->snapshot, (uint64_t)time(NULL),
                  proxy_state(worker->queue) == PROXY_RUNNING, cache_ready,
                  options->readiness_strict, &readiness) ||
              !laghu_operational_render_readiness(
                  &readiness, options->readiness_strict, operational->output,
                  sizeof(operational->output), &output_length)) {
            proxy_send_admin_json(client, 503U, "Service Unavailable",
                                  "{\"status\":\"unavailable\"}", head);
            access.status = 503U;
            access.failure = "runtime";
          } else {
            unsigned int status = readiness.runtime_ready &&
                                          readiness.cache_ready &&
                                          readiness.workers_ready
                                      ? 200U
                                      : 503U;
            proxy_send_admin_json(client, status,
                                  status == 200U ? "OK" : "Service Unavailable",
                                  operational->output, head);
            access.status = status;
            access.output_bytes = head ? 0U : output_length;
            access.failure = status == 200U ? "none" : "readiness";
          }
        } else {
          proxy_send_admin_json(client, 503U, "Service Unavailable",
                                "{\"status\":\"unavailable\"}", head);
          access.status = 503U;
          access.failure = "runtime";
        }
        free(operational);
      } else if (stats_request) {
        laghu_cache_stats stats = {0};
        char json[1536];
        uint64_t requests;
        if (!options->statistics ||
            (strcmp(request.method, "GET") != 0 && !head)) {
          proxy_send_admin_json(client, 405U, "Method Not Allowed",
                                "{\"status\":\"method_not_allowed\"}", head);
          access.status = 405U;
          access.failure = "admin_method";
        } else if (!laghu_cache_backend_health_path(options->cache_path,
                                                    &stats)) {
          proxy_send_admin_json(client, 503U, "Service Unavailable",
                                "{\"status\":\"unavailable\"}", head);
          access.status = 503U;
          access.failure = "cache";
        } else {
          requests = stats.hits + stats.misses;
          (void)snprintf(
              json, sizeof(json),
              "{\"schema\":\"laghu-cache-stats-v1\",\"backend\":\"file\","
              "\"capacity\":{\"bytes\":%llu,\"files\":%llu},"
              "\"usage\":{\"bytes\":%llu,\"files\":%llu},"
              "\"requests\":{\"hits\":%llu,\"misses\":%llu,"
              "\"hit_ratio_ppm\":%llu},\"publications\":%llu,"
              "\"rejected_writes\":%llu,\"evictions\":%llu,"
              "\"purges\":{\"url\":%llu,\"full\":%llu,"
              "\"artifacts\":%llu,\"bytes\":%llu,\"generation\":%llu,"
              "\"last\":%llu},\"corrupt_removals\":%llu,"
              "\"cleaner_active\":%s,\"rebuilding\":%s,"
              "\"last_maintenance\":%llu}",
              (unsigned long long)options->cache_limits.size_limit,
              (unsigned long long)options->cache_limits.inode_limit,
              (unsigned long long)stats.bytes, (unsigned long long)stats.files,
              (unsigned long long)stats.hits, (unsigned long long)stats.misses,
              (unsigned long long)(requests == 0U
                                       ? 0U
                                       : stats.hits * UINT64_C(1000000) /
                                             requests),
              (unsigned long long)stats.publications,
              (unsigned long long)stats.rejected_publications,
              (unsigned long long)stats.evictions,
              (unsigned long long)stats.url_purges,
              (unsigned long long)stats.full_purges,
              (unsigned long long)stats.invalidated_artifacts,
              (unsigned long long)stats.invalidated_bytes,
              (unsigned long long)stats.cache_generation,
              (unsigned long long)stats.last_purge,
              (unsigned long long)stats.corrupt_removals,
              stats.cleaner_active ? "true" : "false",
              stats.rebuilding ? "true" : "false",
              (unsigned long long)stats.last_cleanup);
          proxy_send_admin_json(client, 200U, "OK", json, head);
          access.status = 200U;
          access.output_bytes = head ? 0U : strlen(json);
        }
      } else if ((strcmp(request.method, "PURGE") == 0 &&
                  !options->purge_method) ||
                 (purge_control && !options->purge_query) ||
                 (strcmp(request.method, "PURGE") != 0 &&
                  strcmp(request.method, "GET") != 0)) {
        proxy_send_admin_json(client, 405U, "Method Not Allowed",
                              "{\"status\":\"method_not_allowed\"}", false);
        access.status = 405U;
        access.failure = "admin_method";
      } else {
        uint64_t matched = 0U;
        laghu_cache_purge_result purged = laghu_cache_backend_purge_url_path(
            options->cache_path, purge_target, (uint64_t)time(NULL), &matched);
        unsigned int status =
            purged == LAGHU_CACHE_PURGE_ACCEPTED
                ? 202U
                : (purged == LAGHU_CACHE_PURGE_SATURATED
                       ? 429U
                       : (purged == LAGHU_CACHE_PURGE_INVALID ? 400U : 503U));
        char json[192];
        const char *reason = status == 202U   ? "Accepted"
                             : status == 429U ? "Too Many Requests"
                             : status == 400U ? "Bad Request"
                                              : "Service Unavailable";
        (void)snprintf(json, sizeof(json),
                       "{\"status\":\"%s\",\"matched_artifacts\":%llu}",
                       status == 202U ? "accepted" : "rejected",
                       (unsigned long long)matched);
        proxy_send_admin_json(client, status, reason, json, false);
        access.status = status;
        access.failure = status == 202U ? "none" : "cache";
        access.output_bytes = strlen(json);
      }
      goto done;
    }
  }
  if (!strncmp(request.target, "/.laghu/", 8U)) {
    if (!strcmp(request.target, "/.laghu/health")) {
      bool head = !strcmp(request.method, "HEAD");
      if (strcmp(request.method, "GET") != 0 && !head) {
        PROXY_FAIL(405U, "Method Not Allowed", "request_limit");
      } else {
        char json[128];
        const char *state = proxy_state_name(proxy_state(worker->queue));
        (void)snprintf(json, sizeof(json),
                       "{\"status\":\"ok\",\"state\":\"%s\"}", state);
        proxy_send_json(client, 200U, "OK", json, head);
        access.status = 200U;
        access.output_bytes = head ? 0U : strlen(json);
      }
      goto done;
    }
    if (!strcmp(request.target, "/.laghu/beacon/images.js") &&
        options->config.image_beacon == LAGHU_MODE_ON &&
        !strcmp(request.method, "GET")) {
      char head[256];
      int n = snprintf(head, sizeof(head),
                       "HTTP/1.1 200 OK\r\nContent-Type: "
                       "application/javascript\r\nContent-Length: "
                       "%zu\r\nConnection: close\r\n\r\n",
                       sizeof(laghu_beacon_script) - 1U);
      if (n > 0) {
        (void)proxy_send_all(client, head, (size_t)n);
        (void)proxy_send_all(client, laghu_beacon_script,
                             sizeof(laghu_beacon_script) - 1U);
      }
      access.status = 200U;
      access.output_bytes = sizeof(laghu_beacon_script) - 1U;
      goto done;
    }
    if (!strcmp(request.target, "/.laghu/beacon/images") &&
        options->config.image_beacon == LAGHU_MODE_ON &&
        !strcmp(request.method, "POST")) {
      proxy_header *type =
          proxy_find(request.headers, request.header_count, "Content-Type");
      proxy_header *site =
          proxy_find(request.headers, request.header_count, "Sec-Fetch-Site");
      laghu_image_beacon_record beacon;
      laghu_policy policy;
      char policy_key[LAGHU_RUNTIME_KEY_SIZE];
      uint64_t now = (uint64_t)time(NULL);
      if (type == NULL || strncmp(type->value, "application/json", 16U) != 0 ||
          site == NULL || !proxy_name_equal(site->value, "same-origin") ||
          request_body_length == 0U ||
          request_body_length > LAGHU_PROXY_BEACON_BODY) {
        PROXY_FAIL(400U, "Bad Request", "client_parse");
      } else if (!proxy_beacon_allowed(worker->queue, now)) {
        PROXY_FAIL(429U, "Too Many Requests", "request_limit");
      } else if (!laghu_runtime_parse_image_beacon(
                     (laghu_buffer){request_body, request_body_length},
                     &beacon) ||
                 !laghu_resolve_config_policy(&options->config, &policy) ||
                 !laghu_variant_key((laghu_buffer){NULL, 0U}, &policy,
                                    policy_key) ||
                 (worker->runtime_queue.mapping == NULL &&
                  !laghu_runtime_queue_open(&worker->runtime_queue,
                                            options->worker_queue_path)) ||
                 !laghu_runtime_queue_refresh(&worker->runtime_queue) ||
                 !laghu_catalog_apply_beacon(
                     worker->queue->rum, options->cache_path, policy_key,
                     worker->runtime_queue.capabilities, now,
                     options->config.image_metadata_ttl, &beacon)) {
        PROXY_FAIL(400U, "Bad Request", "worker");
      } else {
        static const char response_204[] =
            "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        (void)proxy_send_all(client, response_204, sizeof(response_204) - 1U);
        access.status = 204U;
      }
      goto done;
    }
    if (!strcmp(request.target, "/.laghu/beacon/critical-css.js") &&
        options->config.critical_css_beacon == LAGHU_MODE_ON &&
        !strcmp(request.method, "GET")) {
      const char *script = laghu_runtime_critical_css_beacon_script();
      char head[256];
      size_t script_length = strlen(script);
      int n = snprintf(head, sizeof(head),
                       "HTTP/1.1 200 OK\r\nContent-Type: "
                       "application/javascript\r\nContent-Length: "
                       "%zu\r\nConnection: close\r\n\r\n",
                       script_length);
      if (n > 0) {
        (void)proxy_send_all(client, head, (size_t)n);
        (void)proxy_send_all(client, script, script_length);
      }
      access.status = 200U;
      access.output_bytes = script_length;
      goto done;
    }
    if (!strcmp(request.target, "/.laghu/beacon/critical-css") &&
        options->config.critical_css_beacon == LAGHU_MODE_ON &&
        !strcmp(request.method, "POST")) {
      proxy_header *type =
          proxy_find(request.headers, request.header_count, "Content-Type");
      proxy_header *site =
          proxy_find(request.headers, request.header_count, "Sec-Fetch-Site");
      laghu_critical_css_beacon beacon;
      laghu_policy policy;
      char policy_key[LAGHU_RUNTIME_KEY_SIZE];
      uint64_t now = (uint64_t)time(NULL);
      if (type == NULL || strncmp(type->value, "application/json", 16U) != 0 ||
          site == NULL || !proxy_name_equal(site->value, "same-origin") ||
          request_body_length == 0U ||
          request_body_length > LAGHU_PROXY_BEACON_BODY) {
        PROXY_FAIL(400U, "Bad Request", "client_parse");
      } else if (!proxy_beacon_allowed(worker->queue, now)) {
        PROXY_FAIL(429U, "Too Many Requests", "request_limit");
      } else if (!laghu_runtime_parse_critical_css_beacon(
                     (laghu_buffer){request_body, request_body_length},
                     &beacon) ||
                 !laghu_resolve_config_policy(&options->config, &policy) ||
                 !laghu_variant_key((laghu_buffer){NULL, 0U}, &policy,
                                    policy_key) ||
                 !laghu_critical_css_apply_beacon(
                     worker->queue->rum, options->cache_path, policy_key, now,
                     options->config.image_metadata_ttl, &beacon)) {
        PROXY_FAIL(400U, "Bad Request", "worker");
      } else {
        static const char response_204[] =
            "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        (void)proxy_send_all(client, response_204, sizeof(response_204) - 1U);
        access.status = 204U;
      }
      goto done;
    }
    if (!strcmp(request.target, "/.laghu/beacon/instrumentation.js") &&
        options->config.instrumentation_beacon == LAGHU_MODE_ON &&
        !strcmp(request.method, "GET")) {
      const char *script = laghu_runtime_instrumentation_script();
      char head[256];
      size_t script_length = strlen(script);
      int n =
          snprintf(head, sizeof(head),
                   "HTTP/1.1 200 OK\r\nContent-Type: application/javascript\r\n"
                   "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                   script_length);
      if (n > 0) {
        (void)proxy_send_all(client, head, (size_t)n);
        (void)proxy_send_all(client, script, script_length);
      }
      access.status = 200U;
      access.output_bytes = script_length;
      goto done;
    }
    if (!strcmp(request.target, "/.laghu/beacon/instrumentation") &&
        options->config.instrumentation_beacon == LAGHU_MODE_ON &&
        !strcmp(request.method, "POST")) {
      proxy_header *type =
          proxy_find(request.headers, request.header_count, "Content-Type");
      proxy_header *site =
          proxy_find(request.headers, request.header_count, "Sec-Fetch-Site");
      laghu_instrumentation_beacon beacon;
      uint64_t now = (uint64_t)time(NULL);
      if (type == NULL || strncmp(type->value, "application/json", 16U) != 0 ||
          site == NULL || !proxy_name_equal(site->value, "same-origin") ||
          request_body_length == 0U ||
          request_body_length > LAGHU_PROXY_BEACON_BODY) {
        PROXY_FAIL(400U, "Bad Request", "client_parse");
      } else if (!proxy_beacon_allowed(worker->queue, now)) {
        PROXY_FAIL(429U, "Too Many Requests", "request_limit");
      } else if (!laghu_runtime_parse_instrumentation_beacon(
                     (laghu_buffer){request_body, request_body_length},
                     &beacon) ||
                 !laghu_instrumentation_apply_beacon(
                     worker->queue->rum, options->cache_path, now,
                     options->config.image_metadata_ttl, &beacon)) {
        PROXY_FAIL(400U, "Bad Request", "worker");
      } else {
        static const char response_204[] =
            "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        (void)proxy_send_all(client, response_204, sizeof(response_204) - 1U);
        access.status = 204U;
      }
      goto done;
    }
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
      proxy_copy(response.reason, sizeof(response.reason), "OK");
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

static bool queue_push(proxy_queue *queue, const proxy_connection *connection) {
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
static DWORD WINAPI proxy_worker_main(LPVOID argument)
#else
static void *proxy_worker_main(void *argument)
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

static laghu_socket proxy_listen(const char *host, const char *port) {
  struct addrinfo hints, *addresses = NULL, *address;
  laghu_socket listener = LAGHU_INVALID_SOCKET;
  int enabled = 1;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  if (getaddrinfo(host, port, &hints, &addresses) != 0) return listener;
  for (address = addresses; address != NULL; address = address->ai_next) {
    listener = (laghu_socket)socket(address->ai_family, address->ai_socktype,
                                    address->ai_protocol);
    if (listener == LAGHU_INVALID_SOCKET) continue;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&enabled,
                     sizeof(enabled));
    if (bind(listener, address->ai_addr, (laghu_socklen)address->ai_addrlen) ==
            0 &&
        listen(listener, SOMAXCONN) == 0)
      break;
    laghu_close(listener);
    listener = LAGHU_INVALID_SOCKET;
  }
  freeaddrinfo(addresses);
  return listener;
}

static uint64_t proxy_monotonic_ms(void) {
#ifdef _WIN32
  return (uint64_t)GetTickCount64();
#else
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0U;
  return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
#endif
}

static void proxy_pause_ms(unsigned int milliseconds) {
#ifdef _WIN32
  Sleep(milliseconds);
#else
  struct timespec value = {(time_t)(milliseconds / 1000U),
                           (long)(milliseconds % 1000U) * 1000000L};
  (void)nanosleep(&value, NULL);
#endif
}

static bool proxy_cache_probe(const char *cache_path) {
  char path[LAGHU_RUNTIME_PATH_SIZE];
  int count;
#ifdef _WIN32
  HANDLE file;
  DWORD written;
  unsigned char marker = 0x4cU;
  count = snprintf(path, sizeof(path), "%s\\.laghu-ready-%lu-%lu-%llu",
                   cache_path, (unsigned long)GetCurrentProcessId(),
                   (unsigned long)GetCurrentThreadId(),
                   (unsigned long long)proxy_monotonic_ms());
  if (count <= 0 || (size_t)count >= sizeof(path)) return false;
  file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                     FILE_ATTRIBUTE_TEMPORARY, NULL);
  if (file == INVALID_HANDLE_VALUE) return false;
  if (!WriteFile(file, &marker, 1U, &written, NULL) || written != 1U ||
      !FlushFileBuffers(file)) {
    CloseHandle(file);
    (void)DeleteFileA(path);
    return false;
  }
  CloseHandle(file);
  return DeleteFileA(path) != 0;
#else
  int file;
  unsigned char marker = 0x4cU;
  count =
      snprintf(path, sizeof(path), "%s/.laghu-ready-%ld-%llu-%llu", cache_path,
               (long)getpid(), (unsigned long long)(uintptr_t)pthread_self(),
               (unsigned long long)proxy_monotonic_ms());
  if (count <= 0 || (size_t)count >= sizeof(path)) return false;
  file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (file < 0) return false;
  if (write(file, &marker, 1U) != 1 || fsync(file) != 0) {
    close(file);
    (void)unlink(path);
    return false;
  }
  if (close(file) != 0) {
    (void)unlink(path);
    return false;
  }
  return unlink(path) == 0;
#endif
}

static bool proxy_queue_path_valid(const char *path) {
  laghu_runtime_queue queue;
  bool exists;
  bool valid = false;
  unsigned int attempt;
#ifdef _WIN32
  DWORD attributes = GetFileAttributesA(path);
  exists = attributes != INVALID_FILE_ATTRIBUTES;
  if (!exists && GetLastError() != ERROR_FILE_NOT_FOUND &&
      GetLastError() != ERROR_PATH_NOT_FOUND)
    return false;
#else
  exists = access(path, F_OK) == 0;
  if (!exists && errno != ENOENT) return false;
#endif
  if (!exists) {
    char parent[LAGHU_RUNTIME_PATH_SIZE];
    const char *separator = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    DWORD parent_attributes;
    if (backslash != NULL && (separator == NULL || backslash > separator))
      separator = backslash;
#endif
    if (separator == NULL) return true;
    if (separator == path) {
      parent[0] = *separator;
      parent[1] = '\0';
    } else {
      size_t length = (size_t)(separator - path);
      if (length >= sizeof(parent)) return false;
      memcpy(parent, path, length);
      parent[length] = '\0';
    }
#ifdef _WIN32
    parent_attributes = GetFileAttributesA(parent);
    return parent_attributes != INVALID_FILE_ATTRIBUTES &&
           (parent_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
#else
    return access(parent, R_OK | X_OK) == 0;
#endif
  }
  for (attempt = 0U; attempt < 10U; ++attempt) {
    laghu_runtime_queue_init(&queue);
    valid = laghu_runtime_queue_open(&queue, path);
    laghu_runtime_queue_close(&queue);
    if (valid) break;
    proxy_pause_ms(10U);
  }
  return valid;
}

#ifdef _WIN32
static BOOL WINAPI proxy_console_control(DWORD event) {
  if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT &&
      event != CTRL_CLOSE_EVENT && event != CTRL_SHUTDOWN_EVENT)
    return FALSE;
  InterlockedIncrement(&proxy_stop_requests);
  return TRUE;
}
#else
static void proxy_signal_handler(int signal_number) {
  (void)signal_number;
  if (proxy_stop_requests < 2) ++proxy_stop_requests;
}
#endif

static bool proxy_listener_ready(laghu_socket listener) {
  fd_set readable;
  struct timeval wait = {0, 200000};
  FD_ZERO(&readable);
  FD_SET(listener, &readable);
#ifdef _WIN32
  return select(0, &readable, NULL, NULL, &wait) > 0;
#else
  return select(listener + 1, &readable, NULL, NULL, &wait) > 0;
#endif
}

static void proxy_begin_drain(proxy_queue *queue) {
  proxy_queue_lock(queue);
  queue->state = PROXY_DRAINING;
  queue->stopping = true;
#ifdef _WIN32
  WakeAllConditionVariable(&queue->ready);
#else
  pthread_cond_broadcast(&queue->ready);
#endif
  proxy_queue_unlock(queue);
  for (;;) {
    laghu_socket client;
    proxy_queue_lock(queue);
    if (queue->count == 0U) {
      proxy_queue_unlock(queue);
      break;
    }
    client = queue->items[queue->head].socket;
    queue->head = (queue->head + 1U) % queue->capacity;
    --queue->count;
    proxy_queue_unlock(queue);
    proxy_reject_connection(queue, client, "shutdown");
  }
}

static unsigned int proxy_active_count(proxy_queue *queue) {
  unsigned int count;
  proxy_queue_lock(queue);
  count = queue->active_count;
  proxy_queue_unlock(queue);
  return count;
}

static void proxy_force_workers(proxy_queue *queue, proxy_worker *workers,
                                unsigned int worker_count) {
  unsigned int index;
  proxy_queue_lock(queue);
  queue->state = PROXY_FORCING;
  for (index = 0U; index < worker_count; ++index) {
    if (workers[index].active_client != LAGHU_INVALID_SOCKET)
      (void)shutdown(workers[index].active_client, LAGHU_SHUT_BOTH);
    if (workers[index].active_origin != LAGHU_INVALID_SOCKET)
      (void)shutdown(workers[index].active_origin, LAGHU_SHUT_BOTH);
  }
  proxy_queue_unlock(queue);
}

int laghu_proxy_run(const laghu_proxy_options *options) {
  proxy_queue queue;
  proxy_worker *workers;
  laghu_socket listener = LAGHU_INVALID_SOCKET;
  unsigned int index, started = 0U;
  int result = 1;
  bool lock_ready = false;
  if (options == NULL) return 1;
#ifndef _WIN32
  bool ready_condition = false, drained_condition = false;
#endif
#ifdef _WIN32
  WSADATA data;
  HANDLE *threads;
  bool sockets_ready = false;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
  sockets_ready = true;
#else
  pthread_t *threads;
#endif
  memset(&queue, 0, sizeof(queue));
  laghu_operational_registry_init(&queue.operational);
  queue.options = options;
  queue.capacity = options->connection_queue;
  queue.items = calloc(queue.capacity, sizeof(*queue.items));
  workers = calloc(options->workers, sizeof(*workers));
  threads = calloc(options->workers, sizeof(*threads));
  if (queue.items == NULL || workers == NULL || threads == NULL) goto cleanup;
#ifdef _WIN32
  InitializeCriticalSection(&queue.lock);
  lock_ready = true;
  InitializeConditionVariable(&queue.ready);
  InitializeConditionVariable(&queue.drained);
#else
  if (pthread_mutex_init(&queue.lock, NULL) != 0) goto cleanup;
  lock_ready = true;
  if (pthread_cond_init(&queue.ready, NULL) != 0) goto cleanup;
  ready_condition = true;
  if (pthread_cond_init(&queue.drained, NULL) != 0) goto cleanup;
  drained_condition = true;
#endif
  queue.state = PROXY_STARTING;
  queue.cache_readiness = -1;
  queue.optimizer_readiness = -1;
  queue.request_prefix = proxy_monotonic_ms() << 16U;
  {
    laghu_rum_options rum_options;
    char snapshot[LAGHU_RUNTIME_PATH_SIZE];
    char rum_error[160U];
    laghu_rum_options_init(&rum_options);
    if (options->rum_snapshot_path[0] != '\0')
      (void)snprintf(snapshot, sizeof(snapshot), "%s",
                     options->rum_snapshot_path);
    else if (snprintf(snapshot, sizeof(snapshot), "%s/rum.snapshot",
                      options->cache_path) <= 0)
      goto cleanup;
    rum_options.store_uri = options->rum_store;
    rum_options.snapshot_path = snapshot;
    rum_options.client_library = options->rum_client_library[0] == '\0'
                                     ? NULL
                                     : options->rum_client_library;
    rum_options.memory_limit = options->rum_memory_limit;
    rum_options.pending_limit = options->rum_pending_limit;
    rum_options.ttl_seconds = options->rum_ttl;
    rum_options.sync_interval_seconds = options->rum_sync_interval;
    rum_options.timeout_ms = options->rum_timeout_ms;
    rum_options.retry_limit = options->rum_retry_limit;
    rum_options.required = options->rum_store_required;
    queue.rum =
        laghu_rum_engine_create(&rum_options, rum_error, sizeof(rum_error));
    if (queue.rum == NULL && options->rum_store_required) goto cleanup;
    if (queue.rum == NULL) {
      proxy_log_event(&queue, "rum_store_fallback", "degraded");
      rum_options.store_uri = "local:";
      rum_options.client_library = NULL;
      rum_options.required = false;
      queue.rum =
          laghu_rum_engine_create(&rum_options, rum_error, sizeof(rum_error));
    }
    if (queue.rum == NULL) goto cleanup;
  }
  if (!proxy_cache_probe(options->cache_path)) {
    proxy_log_startup_failure(&queue, "cache_unavailable");
    goto cleanup;
  }
  if (!proxy_queue_path_valid(options->worker_queue_path) ||
      (options->font_providers_loaded &&
       !proxy_queue_path_valid(options->font_fetch_queue_path)) ||
      (options->javascript_queue_enabled &&
       !proxy_queue_path_valid(options->javascript_queue_path))) {
    proxy_log_startup_failure(&queue, "queue_unavailable");
    goto cleanup;
  }
  if (options->source_policy.mode != LAGHU_SOURCE_FILE_OFF &&
      !laghu_source_registry_publish(options->asset_upload_queue_path,
                                     &options->source_policy)) {
    proxy_log_startup_failure(&queue, "source_registry");
    goto cleanup;
  }
  if (!laghu_cache_backend_register_path(options->cache_path,
                                         &options->cache_limits)) {
    proxy_log_startup_failure(&queue, "cache_backend");
    goto cleanup;
  }
  if ((options->metrics || options->readiness) &&
      !laghu_operational_registry_open(&queue.operational, options->cache_path,
                                       LAGHU_OPERATIONAL_SURFACE_STANDALONE,
                                       LAGHU_OPERATIONAL_PROCESS_ADAPTER, true,
                                       (uint64_t)time(NULL))) {
    proxy_log_event(&queue, "observability", "unavailable");
  }
  if (options->origin_tls &&
      (queue.tls_context = proxy_tls_context(options)) == NULL) {
    proxy_log_startup_failure(&queue, "origin_tls");
    goto cleanup;
  }
  listener = proxy_listen(options->listen_host, options->listen_port);
  if (listener == LAGHU_INVALID_SOCKET) {
    proxy_log_startup_failure(&queue, "listen");
    goto cleanup;
  }
  proxy_stop_requests = 0;
#ifdef _WIN32
  (void)SetConsoleCtrlHandler(proxy_console_control, TRUE);
#else
  {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = proxy_signal_handler;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    action.sa_handler = SIG_IGN;
    (void)sigaction(SIGPIPE, &action, NULL);
  }
#endif
  for (index = 0U; index < options->workers; ++index) {
    workers[index].queue = &queue;
    workers[index].active_client = LAGHU_INVALID_SOCKET;
    workers[index].active_origin = LAGHU_INVALID_SOCKET;
#ifdef _WIN32
    threads[index] =
        CreateThread(NULL, 0U, proxy_worker_main, &workers[index], 0U, NULL);
    if (threads[index] == NULL) break;
#else
    if (pthread_create(&threads[index], NULL, proxy_worker_main,
                       &workers[index]) != 0)
      break;
#endif
    ++started;
  }
  if (started != options->workers) {
    proxy_log_event(&queue, "startup_failure", "forcing");
    proxy_stop_requests = 2;
  } else {
    queue.state = PROXY_RUNNING;
    proxy_log_event(&queue, "startup", "running");
    while (proxy_stop_requests == 0) {
      if (proxy_listener_ready(listener)) {
        proxy_connection connection;
        memset(&connection, 0, sizeof(connection));
        connection.peer_length = (laghu_socklen)sizeof(connection.peer);
        connection.socket =
            accept(listener, (struct sockaddr *)&connection.peer,
                   &connection.peer_length);
        if (connection.socket != LAGHU_INVALID_SOCKET &&
            !queue_push(&queue, &connection))
          proxy_reject_connection(&queue, connection.socket, "queue_saturated");
      }
    }
  }
  laghu_close(listener);
  listener = LAGHU_INVALID_SOCKET;
  proxy_begin_drain(&queue);
  proxy_log_event(&queue, "shutdown", "draining");
  {
    uint64_t deadline =
        proxy_monotonic_ms() + (uint64_t)options->drain_timeout * 1000U;
    while (proxy_active_count(&queue) != 0U && proxy_stop_requests < 2 &&
           proxy_monotonic_ms() < deadline)
      proxy_pause_ms(20U);
  }
  if (proxy_active_count(&queue) != 0U) {
    proxy_force_workers(&queue, workers, started);
    proxy_log_event(&queue, "shutdown", "forcing");
  }
  for (index = 0U; index < started; ++index) {
#ifdef _WIN32
    (void)WaitForSingleObject(threads[index], INFINITE);
    CloseHandle(threads[index]);
#else
    (void)pthread_join(threads[index], NULL);
#endif
  }
  queue.state = PROXY_STOPPED;
  proxy_log_event(&queue, "shutdown", "stopped");
  result = started == options->workers ? 0 : 1;
cleanup:
  laghu_operational_registry_close(&queue.operational);
  laghu_rum_engine_destroy(queue.rum);
  if (listener != LAGHU_INVALID_SOCKET) laghu_close(listener);
  SSL_CTX_free(queue.tls_context);
#ifdef _WIN32
  if (lock_ready) DeleteCriticalSection(&queue.lock);
#else
  if (drained_condition) pthread_cond_destroy(&queue.drained);
  if (ready_condition) pthread_cond_destroy(&queue.ready);
  if (lock_ready) pthread_mutex_destroy(&queue.lock);
#endif
  free(threads);
  free(workers);
  free(queue.items);
#ifdef _WIN32
  if (sockets_ready) WSACleanup();
#endif
  return result;
}

#ifdef _WIN32
static DWORD WINAPI proxy_service_control(DWORD control, DWORD event_type,
                                          LPVOID event_data, LPVOID context) {
  (void)event_type;
  (void)event_data;
  (void)context;
  if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
    InterlockedIncrement(&proxy_stop_requests);
    proxy_service_status.dwCurrentState = SERVICE_STOP_PENDING;
    proxy_service_status.dwWaitHint =
        proxy_service_options->drain_timeout * 1000U;
    (void)SetServiceStatus(proxy_service_handle, &proxy_service_status);
    return NO_ERROR;
  }
  return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI proxy_service_main(DWORD argument_count, LPSTR *arguments) {
  int result;
  (void)argument_count;
  (void)arguments;
  memset(&proxy_service_status, 0, sizeof(proxy_service_status));
  proxy_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  proxy_service_status.dwCurrentState = SERVICE_START_PENDING;
  proxy_service_status.dwControlsAccepted =
      SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  proxy_service_handle =
      RegisterServiceCtrlHandlerExA("laghu", proxy_service_control, NULL);
  if (proxy_service_handle == NULL) return;
  (void)SetServiceStatus(proxy_service_handle, &proxy_service_status);
  proxy_service_status.dwCurrentState = SERVICE_RUNNING;
  (void)SetServiceStatus(proxy_service_handle, &proxy_service_status);
  result = laghu_proxy_run(proxy_service_options);
  proxy_service_status.dwCurrentState = SERVICE_STOPPED;
  proxy_service_status.dwWin32ExitCode =
      result == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
  proxy_service_status.dwServiceSpecificExitCode = (DWORD)result;
  (void)SetServiceStatus(proxy_service_handle, &proxy_service_status);
}

int laghu_proxy_run_service(const laghu_proxy_options *options) {
  SERVICE_TABLE_ENTRYA table[] = {{"laghu", proxy_service_main}, {NULL, NULL}};
  proxy_service_options = options;
  return StartServiceCtrlDispatcherA(table) != 0 ? 0 : 1;
}
#endif
