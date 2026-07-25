// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/proxy.h"

#include <ctype.h>
#include <errno.h>
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
#else
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
typedef int laghu_socket;
typedef socklen_t laghu_socklen;
#define LAGHU_INVALID_SOCKET (-1)
#define laghu_close close
#define LAGHU_SHUT_WRITE SHUT_WR
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
  const laghu_proxy_options *options;
  laghu_socket *items;
  unsigned int capacity;
  unsigned int head;
  unsigned int count;
  uint64_t beacon_second;
  unsigned int beacon_count;
  bool stopping;
#ifdef _WIN32
  CRITICAL_SECTION lock;
  CONDITION_VARIABLE ready;
#else
  pthread_mutex_t lock;
  pthread_cond_t ready;
#endif
} proxy_queue;

typedef struct {
  proxy_queue *queue;
  laghu_runtime_queue runtime_queue;
} proxy_worker;

static const char laghu_beacon_script[] =
    "addEventListener('load',()=>{document.querySelectorAll('img[src]').forEach"
    "(i=>{const r=i.getBoundingClientRect();if(r.width<1||r.height<1)return;"
    "fetch('/.laghu/beacon/images',{method:'POST',headers:{'Content-Type':"
    "'application/json'},body:JSON.stringify({url:new URL(i.currentSrc||i.src,"
    "location.href).pathname,width:Math.round(r.width),height:Math.round(r."
    "height),viewport_width:innerWidth,dpr_hundredths:Math.min(400,Math.max("
    "100,Math.round(devicePixelRatio*100))),above_fold:r.top<innerHeight,"
    "mobile:innerWidth<768}),keepalive:true})})});";

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
  unsigned int port = 80U;
  if (value == NULL || strncmp(value, "http://", 7U) != 0) return false;
  authority = value + 7U;
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
  bool queue_seen = false, selector_seen = false;
  bool quality_seen = false, workers_seen = false;
  bool connection_queue_seen = false, connect_timeout_seen = false;
  bool io_timeout_seen = false;
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
      if (cache_seen ||
          !proxy_copy(options->cache_path, sizeof(options->cache_path), value))
        return proxy_error(error, error_size, "invalid or duplicate --cache");
      cache_seen = true;
    } else if (strcmp(name, "--worker-queue") == 0) {
      NEED_VALUE();
      if (queue_seen || !proxy_copy(options->worker_queue_path,
                                    sizeof(options->worker_queue_path), value))
        return proxy_error(error, error_size,
                           "invalid or duplicate --worker-queue");
      queue_seen = true;
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
    } else if (strcmp(name, "--allow-api") == 0) {
      if (options->config.allow_api == LAGHU_MODE_ON)
        return proxy_error(error, error_size, "duplicate --allow-api");
      options->config.allow_api = LAGHU_MODE_ON;
    } else if (strcmp(name, "--image-beacon") == 0) {
      if (options->config.image_beacon == LAGHU_MODE_ON)
        return proxy_error(error, error_size, "duplicate --image-beacon");
      options->config.image_beacon = LAGHU_MODE_ON;
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
    } else {
      return proxy_error(error, error_size, "unknown option");
    }
#undef NEED_VALUE
  }
  if (!listen_seen || !origin_seen || !cache_seen || !queue_seen)
    return proxy_error(
        error, error_size,
        "--listen, --origin, --cache, and --worker-queue are required");
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

static bool proxy_read_headers(laghu_socket socket, char *buffer,
                               size_t *length, unsigned char **body_start,
                               size_t *body_initial) {
  size_t used = 0U;
  while (used < LAGHU_PROXY_HEADER_BYTES) {
    int got =
        recv(socket, buffer + used, (int)(LAGHU_PROXY_HEADER_BYTES - used), 0);
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

static laghu_socket proxy_connect(const char *host, const char *port,
                                  unsigned int timeout) {
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
          fd_set writable;
          struct timeval wait = {(long)timeout, 0};
          int selected;
          int socket_error = 0;
          laghu_socklen error_length = (laghu_socklen)sizeof(socket_error);
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
              socket_error == 0)
            connected = true;
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

static void proxy_reject_overload(laghu_socket client) {
  unsigned char discarded[4096U];
  proxy_error_response(client, 503U, "Service Unavailable");
  (void)shutdown(client, LAGHU_SHUT_WRITE);
#ifdef _WIN32
  {
    u_long nonblocking = 1U;
    (void)ioctlsocket(client, FIONBIO, &nonblocking);
    while (recv(client, (char *)discarded, sizeof(discarded), 0) > 0) {
    }
  }
#else
  while (recv(client, discarded, sizeof(discarded), MSG_DONTWAIT) > 0) {
  }
#endif
  laghu_close(client);
}

static bool proxy_read_body(laghu_socket socket, const unsigned char *initial,
                            size_t initial_length, size_t expected,
                            bool to_close, unsigned char **body,
                            size_t *length) {
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
    got = recv(socket, (char *)data + used, (int)(capacity - used), 0);
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

static bool proxy_stream_body(laghu_socket origin, laghu_socket client,
                              const unsigned char *initial,
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
    got = recv(origin, (char *)buffer, (int)wanted, 0);
    if (got == 0 && until_close) return true;
    if (got <= 0) return false;
    if (!proxy_send_all(client, buffer, (size_t)got)) return false;
    sent += (size_t)got;
  }
  return expected == 0U || sent == expected;
}

static void proxy_handle(laghu_socket client, proxy_worker *worker) {
  const laghu_proxy_options *options = worker->queue->options;
  proxy_request request;
  proxy_response response;
  unsigned char *initial;
  size_t initial_length, header_length;
  unsigned char *request_body = NULL, *origin_body = NULL, *decoded = NULL;
  size_t request_body_length = 0U, origin_body_length = 0U;
  laghu_socket origin = LAGHU_INVALID_SOCKET;
  char outbound[LAGHU_PROXY_HEADER_BYTES + 1U];
  size_t outbound_length = 0U, index;
  laghu_http_header request_headers[LAGHU_HTTP_MAX_REQUEST_HEADERS];
  laghu_http_header response_headers[LAGHU_HTTP_MAX_RESPONSE_HEADERS];
  laghu_http_request normalized_request;
  laghu_http_response normalized_response;
  laghu_http_environment environment;
  laghu_http_transaction transaction;
  laghu_http_transaction_result prepared, finalized;
  bool prepared_ok = false;
  memset(&request, 0, sizeof(request));
  memset(&response, 0, sizeof(response));
  memset(&prepared, 0, sizeof(prepared));
  memset(&finalized, 0, sizeof(finalized));
  proxy_timeout(client, options->io_timeout);
  if (!proxy_read_headers(client, request.storage, &header_length, &initial,
                          &initial_length) ||
      !proxy_parse_request(&request, header_length)) {
    proxy_error_response(client, 400U, "Bad Request");
    goto done;
  }
  if (!strcmp(request.method, "CONNECT") || !strcmp(request.method, "TRACE")) {
    proxy_error_response(client, 405U, "Method Not Allowed");
    goto done;
  }
  if (request.expect) {
    proxy_error_response(client, 417U, "Expectation Failed");
    goto done;
  }
  if (request.upgrade) {
    proxy_error_response(client, 400U, "Bad Request");
    goto done;
  }
  if (request.chunked) {
    proxy_error_response(client, 501U, "Not Implemented");
    goto done;
  }
  if (request.content_length > LAGHU_PROXY_REQUEST_BODY_BYTES) {
    proxy_error_response(client, 413U, "Payload Too Large");
    goto done;
  }
  if (request.content_length != 0U &&
      !proxy_read_body(client, initial, initial_length, request.content_length,
                       false, &request_body, &request_body_length)) {
    proxy_error_response(client, 400U, "Bad Request");
    goto done;
  }
  if (!strncmp(request.target, "/.laghu/", 8U)) {
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
        proxy_error_response(client, 400U, "Bad Request");
      } else if (!proxy_beacon_allowed(worker->queue, now)) {
        proxy_error_response(client, 429U, "Too Many Requests");
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
                     options->cache_path, policy_key,
                     worker->runtime_queue.capabilities, now,
                     options->config.image_metadata_ttl, &beacon)) {
        proxy_error_response(client, 400U, "Bad Request");
      } else {
        static const char response_204[] =
            "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        (void)proxy_send_all(client, response_204, sizeof(response_204) - 1U);
      }
      goto done;
    }
    if (strcmp(request.method, "GET") != 0 &&
        strcmp(request.method, "HEAD") != 0) {
      proxy_error_response(client, 405U, "Method Not Allowed");
      goto done;
    }
    memset(&normalized_request, 0, sizeof(normalized_request));
    memset(&normalized_response, 0, sizeof(normalized_response));
    memset(&environment, 0, sizeof(environment));
    normalized_request = (laghu_http_request){
        LAGHU_HTTP_ABI_VERSION,
        sizeof(normalized_request),
        {(const unsigned char *)request.method, strlen(request.method)},
        {(const unsigned char *)"http", 4U},
        {NULL, 0U},
        {(const unsigned char *)request.target, strlen(request.target)},
        NULL,
        0U};
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
    environment = (laghu_http_environment){LAGHU_HTTP_ABI_VERSION,
                                           sizeof(environment),
                                           options->config,
                                           options->cache_path,
                                           options->worker_queue_path,
                                           &worker->runtime_queue,
                                           (uint64_t)time(NULL)};
    laghu_http_transaction_init(&transaction);
    if (laghu_http_transaction_prepare(&transaction, &normalized_request,
                                       &normalized_response, &environment,
                                       &prepared) &&
        prepared.action == LAGHU_HTTP_ACTION_SERVE_CACHED) {
      response.status = 200U;
      proxy_copy(response.reason, sizeof(response.reason), "OK");
      (void)proxy_send_result(client, &response, &prepared, prepared.selected);
    } else
      proxy_error_response(client, 404U, "Not Found");
    laghu_http_transaction_result_release(&prepared);
    goto done;
  }
  origin = proxy_connect(options->origin_host, options->origin_port,
                         options->connect_timeout);
  if (origin == LAGHU_INVALID_SOCKET) {
    proxy_error_response(client, 502U, "Bad Gateway");
    goto done;
  }
  proxy_timeout(origin, options->io_timeout);
  outbound_length = (size_t)snprintf(
      outbound, sizeof(outbound),
      "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n", request.method,
      request.target, options->origin_authority);
  for (index = 0U; index < request.header_count; ++index) {
    int n;
    if (proxy_hop(request.headers[index].name) ||
        proxy_connection_nominates(request.headers, request.header_count,
                                   request.headers[index].name) ||
        proxy_name_equal(request.headers[index].name, "Host") ||
        proxy_name_equal(request.headers[index].name, "Content-Length"))
      continue;
    n = snprintf(outbound + outbound_length, sizeof(outbound) - outbound_length,
                 "%s: %s\r\n", request.headers[index].name,
                 request.headers[index].value);
    if (n <= 0 || (size_t)n >= sizeof(outbound) - outbound_length) {
      proxy_error_response(client, 400U, "Bad Request");
      goto done;
    }
    outbound_length += (size_t)n;
  }
  if (request.content_length != 0U)
    outbound_length += (size_t)snprintf(
        outbound + outbound_length, sizeof(outbound) - outbound_length,
        "Content-Length: %zu\r\n", request.content_length);
  if (outbound_length + 2U >= sizeof(outbound)) {
    proxy_error_response(client, 400U, "Bad Request");
    goto done;
  }
  memcpy(outbound + outbound_length, "\r\n", 2U);
  outbound_length += 2U;
  if (!proxy_send_all(origin, outbound, outbound_length) ||
      (request_body_length &&
       !proxy_send_all(origin, request_body, request_body_length)) ||
      !proxy_read_headers(origin, response.storage, &header_length, &initial,
                          &initial_length) ||
      !proxy_parse_response(&response, header_length)) {
    proxy_error_response(client, 502U, "Bad Gateway");
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
  normalized_request = (laghu_http_request){
      LAGHU_HTTP_ABI_VERSION,
      sizeof(normalized_request),
      {(unsigned char *)request.method, strlen(request.method)},
      {(unsigned char *)"http", 4U},
      {NULL, 0U},
      {(unsigned char *)request.target, strlen(request.target)},
      request_headers,
      request.header_count};
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
  environment = (laghu_http_environment){LAGHU_HTTP_ABI_VERSION,
                                         sizeof(environment),
                                         options->config,
                                         options->cache_path,
                                         options->worker_queue_path,
                                         &worker->runtime_queue,
                                         (uint64_t)time(NULL)};
  if (!response.chunked) {
    bool bodyless = !strcmp(request.method, "HEAD") ||
                    (response.status >= 100U && response.status < 200U) ||
                    response.status == 204U || response.status == 304U;
    laghu_http_transaction_init(&transaction);
    prepared_ok = laghu_http_transaction_prepare(
        &transaction, &normalized_request, &normalized_response, &environment,
        &prepared);
    if (!prepared_ok) {
      proxy_error_response(client, 502U, "Bad Gateway");
      goto done;
    }
    if (prepared.action == LAGHU_HTTP_ACTION_SERVE_CACHED) {
      (void)proxy_send_result(client, &response, &prepared, prepared.selected);
      goto done;
    }
    if (prepared.action == LAGHU_HTTP_ACTION_BYPASS) {
      size_t expected = bodyless ? 0U : response.content_length;
      bool until_close = !bodyless && !response.has_content_length;
      if (!proxy_send_headers(client, &response, &prepared,
                              response.content_length,
                              response.has_content_length) ||
          !proxy_stream_body(origin, client, initial,
                             bodyless ? 0U : initial_length, expected,
                             until_close)) {
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
            origin, initial, initial_length,
            bodyless
                ? 0U
                : (response.has_content_length ? response.content_length : 0U),
            !bodyless && !response.has_content_length, &origin_body,
            &origin_body_length)) {
      proxy_error_response(client, 502U, "Bad Gateway");
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
      proxy_error_response(client, 502U, "Bad Gateway");
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
      proxy_error_response(client, 502U, "Bad Gateway");
      goto done;
    }
  }
  if (prepared.action == LAGHU_HTTP_ACTION_SERVE_CACHED) {
    (void)proxy_send_result(client, &response, &prepared, prepared.selected);
  } else if (prepared.action == LAGHU_HTTP_ACTION_BYPASS) {
    (void)proxy_send_result(client, &response, &prepared,
                            (laghu_buffer){origin_body, origin_body_length});
  } else if (laghu_http_transaction_finalize(
                 &transaction, (laghu_buffer){origin_body, origin_body_length},
                 &finalized)) {
    (void)proxy_send_result(client, &response, &finalized, finalized.selected);
  } else {
    (void)proxy_send_result(client, &response, &finalized,
                            (laghu_buffer){origin_body, origin_body_length});
  }
done:
  laghu_http_transaction_result_release(&prepared);
  laghu_http_transaction_result_release(&finalized);
  free(request_body);
  free(origin_body);
  free(decoded);
  if (origin != LAGHU_INVALID_SOCKET) laghu_close(origin);
  laghu_close(client);
}

static bool queue_push(proxy_queue *queue, laghu_socket socket) {
  bool accepted = false;
#ifdef _WIN32
  EnterCriticalSection(&queue->lock);
#else
  pthread_mutex_lock(&queue->lock);
#endif
  if (queue->count < queue->capacity) {
    queue->items[(queue->head + queue->count) % queue->capacity] = socket;
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

static laghu_socket queue_pop(proxy_queue *queue) {
  laghu_socket socket;
#ifdef _WIN32
  EnterCriticalSection(&queue->lock);
  while (queue->count == 0U && !queue->stopping)
    SleepConditionVariableCS(&queue->ready, &queue->lock, INFINITE);
#else
  pthread_mutex_lock(&queue->lock);
  while (queue->count == 0U && !queue->stopping)
    pthread_cond_wait(&queue->ready, &queue->lock);
#endif
  socket =
      queue->count == 0U ? LAGHU_INVALID_SOCKET : queue->items[queue->head];
  if (queue->count != 0U) {
    queue->head = (queue->head + 1U) % queue->capacity;
    --queue->count;
  }
#ifdef _WIN32
  LeaveCriticalSection(&queue->lock);
#else
  pthread_mutex_unlock(&queue->lock);
#endif
  return socket;
}

#ifdef _WIN32
static DWORD WINAPI proxy_worker_main(LPVOID argument)
#else
static void *proxy_worker_main(void *argument)
#endif
{
  proxy_worker *worker = argument;
  laghu_socket client;
  laghu_runtime_queue_init(&worker->runtime_queue);
  while ((client = queue_pop(worker->queue)) != LAGHU_INVALID_SOCKET)
    proxy_handle(client, worker);
  laghu_runtime_queue_close(&worker->runtime_queue);
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

int laghu_proxy_run(const laghu_proxy_options *options) {
  proxy_queue queue;
  proxy_worker *workers;
  laghu_socket listener;
  unsigned int index;
#ifdef _WIN32
  WSADATA data;
  HANDLE *threads;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
#else
  pthread_t *threads;
#endif
  memset(&queue, 0, sizeof(queue));
  queue.options = options;
  queue.capacity = options->connection_queue;
  queue.items = calloc(queue.capacity, sizeof(*queue.items));
  workers = calloc(options->workers, sizeof(*workers));
  threads = calloc(options->workers, sizeof(*threads));
  if (queue.items == NULL || workers == NULL || threads == NULL) return 1;
#ifdef _WIN32
  InitializeCriticalSection(&queue.lock);
  InitializeConditionVariable(&queue.ready);
#else
  pthread_mutex_init(&queue.lock, NULL);
  pthread_cond_init(&queue.ready, NULL);
#endif
  listener = proxy_listen(options->listen_host, options->listen_port);
  if (listener == LAGHU_INVALID_SOCKET) {
    fprintf(stderr, "laghu: cannot bind %s:%s\n", options->listen_host,
            options->listen_port);
    return 1;
  }
  for (index = 0U; index < options->workers; ++index) {
    workers[index].queue = &queue;
#ifdef _WIN32
    threads[index] =
        CreateThread(NULL, 0U, proxy_worker_main, &workers[index], 0U, NULL);
#else
    if (pthread_create(&threads[index], NULL, proxy_worker_main,
                       &workers[index]) != 0)
      return 1;
#endif
  }
  fprintf(stderr, "laghu: listening on %s:%s, origin=http://%s\n",
          options->listen_host, options->listen_port,
          options->origin_authority);
  for (;;) {
    laghu_socket client = accept(listener, NULL, NULL);
    if (client == LAGHU_INVALID_SOCKET) break;
    if (!queue_push(&queue, client)) {
      proxy_reject_overload(client);
    }
  }
  return 1;
}
