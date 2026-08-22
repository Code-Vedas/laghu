// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <zlib.h>

#include "server_internal.h"

#define LAGHU_STATIC_MAX_BYTES (64U * 1024U * 1024U)

static const char *static_type(const char *path) {
  const char *extension = strrchr(path, '.');
  if (extension == NULL) return "application/octet-stream";
  if (!strcmp(extension, ".html") || !strcmp(extension, ".htm")) return "text/html; charset=utf-8";
  if (!strcmp(extension, ".css")) return "text/css; charset=utf-8";
  if (!strcmp(extension, ".js") || !strcmp(extension, ".mjs")) return "application/javascript; charset=utf-8";
  if (!strcmp(extension, ".json")) return "application/json";
  if (!strcmp(extension, ".svg")) return "image/svg+xml";
  if (!strcmp(extension, ".png")) return "image/png";
  if (!strcmp(extension, ".jpg") || !strcmp(extension, ".jpeg")) return "image/jpeg";
  if (!strcmp(extension, ".webp")) return "image/webp";
  if (!strcmp(extension, ".avif")) return "image/avif";
  if (!strcmp(extension, ".woff")) return "font/woff";
  if (!strcmp(extension, ".woff2")) return "font/woff2";
  return "application/octet-stream";
}

static bool static_target_safe(const char *target, char path[LAGHU_RUNTIME_PATH_SIZE]) {
  const char *query = strchr(target, '?');
  size_t length = query == NULL ? strlen(target) : (size_t)(query - target);
  size_t index;
  if (length == 0U || length >= LAGHU_RUNTIME_PATH_SIZE || target[0] != '/') return false;
  for (index = 0U; index < length; ++index) {
    if (target[index] == '%' || target[index] == '\\' || (target[index] == '.' && index + 1U < length && target[index + 1U] == '.')) return false;
    path[index] = target[index];
  }
  path[length] = '\0';
  return true;
}

static bool static_range(const char *value, size_t total, size_t *start, size_t *length) {
  unsigned long long first, last;
  char *end = NULL;
  if (value == NULL || strncmp(value, "bytes=", 6U) != 0 || strchr(value + 6U, ',') != NULL) return false;
  errno = 0;
  first = strtoull(value + 6U, &end, 10);
  if (errno != 0 || end == value + 6U || *end != '-') return false;
  if (end[1] == '\0')
    last = total == 0U ? 0U : total - 1U;
  else {
    errno = 0;
    last = strtoull(end + 1U, &end, 10);
    if (errno != 0 || *end != '\0') return false;
  }
  if (first >= total || last < first || last >= total) return false;
  *start = (size_t)first;
  *length = (size_t)(last - first + 1U);
  return true;
}

static bool static_accepts_gzip(const proxy_request *request) {
  const proxy_header *header = proxy_find((proxy_header *)request->headers, request->header_count, "Accept-Encoding");
  const char *cursor;
  if (header == NULL) return false;
  cursor = header->value;
  while (*cursor != '\0') {
    const char *end = strchr(cursor, ',');
    const char *token = cursor;
    const char *token_end = end == NULL ? cursor + strlen(cursor) : end;
    bool disabled = false;
    while (token < token_end && (*token == ' ' || *token == '\t')) ++token;
    while (token_end > token && (token_end[-1] == ' ' || token_end[-1] == '\t')) --token_end;
    {
      const char *parameters = memchr(token, ';', (size_t)(token_end - token));
      size_t name_length = parameters == NULL ? (size_t)(token_end - token) : (size_t)(parameters - token);
      if (parameters != NULL) {
        const char *parameter = parameters + 1U;
        while (parameter < token_end) {
          const char *parameter_end = memchr(parameter, ';', (size_t)(token_end - parameter));
          const char *value;
          if (parameter_end == NULL) parameter_end = token_end;
          while (parameter < parameter_end && (*parameter == ' ' || *parameter == '\t')) ++parameter;
          while (parameter_end > parameter && (parameter_end[-1] == ' ' || parameter_end[-1] == '\t')) --parameter_end;
          if ((size_t)(parameter_end - parameter) >= 3U && (parameter[0] == 'q' || parameter[0] == 'Q') && parameter[1] == '=' &&
              parameter[2] == '0') {
            value = parameter + 3U;
            if (value == parameter_end)
              disabled = true;
            else if (*value == '.') {
              ++value;
              while (value < parameter_end && *value == '0') ++value;
              if (value == parameter_end) disabled = true;
            }
          }
          parameter = parameter_end == token_end ? token_end : parameter_end + 1U;
        }
      }
      if (!disabled && name_length == 4U && !strncasecmp(token, "gzip", 4U)) return true;
    }
    cursor = end == NULL ? token_end : end + 1U;
  }
  return false;
}

static bool static_read_full(int file, size_t offset, unsigned char *output, size_t length) {
  size_t read_length = 0U;
  if (file == LAGHU_INVALID_SOCKET || output == NULL || lseek(file, (off_t)offset, SEEK_SET) < 0) return false;
  while (read_length < length) {
    ssize_t received = read(file, output + read_length, length - read_length);
    if (received <= 0) return false;
    read_length += (size_t)received;
  }
  return true;
}

static bool static_compressible(const char *type) {
  return !strncasecmp(type, "text/", 5U) || !strncasecmp(type, "application/javascript", 22U) || !strncasecmp(type, "application/json", 16U) ||
         !strncasecmp(type, "image/svg+xml", 13U);
}

static bool static_gzip(laghu_buffer input, unsigned char **output, size_t *output_length) {
  z_stream stream;
  unsigned char *compressed;
  size_t capacity;
  int result;
  if (input.length > UINT_MAX) return false;
  capacity = (size_t)compressBound((uLong)input.length);
  compressed = malloc(capacity == 0U ? 1U : capacity);
  if (compressed == NULL) return false;
  memset(&stream, 0, sizeof(stream));
  stream.next_in = (Bytef *)input.data;
  stream.avail_in = (uInt)input.length;
  stream.next_out = compressed;
  stream.avail_out = (uInt)capacity;
  result = deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY);
  if (result != Z_OK) {
    free(compressed);
    return false;
  }
  result = deflate(&stream, Z_FINISH);
  if (result != Z_STREAM_END) {
    (void)deflateEnd(&stream);
    free(compressed);
    return false;
  }
  if (deflateEnd(&stream) != Z_OK) {
    free(compressed);
    return false;
  }
  *output = compressed;
  *output_length = stream.total_out;
  return true;
}

static void static_environment(laghu_http_environment *environment, proxy_worker *worker, const laghu_config *core,
                               const laghu_service_config *service) {
  *environment =
      (laghu_http_environment){.config = *core,
                               .cache_path = service->image_cache,
                               .asset_offload = service->asset_offload,
                               .rum = worker->queue->rum,
                               .queue = proxy_runtime_queue(worker),
                               .queue_capabilities = proxy_runtime_queue_capabilities(worker),
                               .font_fetch_queue = service->font_providers != NULL ? proxy_font_fetch_queue(worker) : NULL,
                               .font_providers = service->font_providers,
                               .javascript_queue = service->javascript_queue[0] != '\0' ? proxy_javascript_queue(worker) : NULL,
                               .chrome_analysis_queue = service->chrome_analysis_queue[0] != '\0' ? proxy_chrome_analysis_queue(worker) : NULL,
                               .otel_trace_queue = service->otel_trace_queue[0] != '\0' ? proxy_otel_trace_queue(worker) : NULL,
                               .otel_sampling_rate = service->otel_sampling_rate,
                               .chrome_analysis_timeout_ms = service->chrome_analysis_timeout_ms,
                               .javascript_target = service->javascript_target,
                               .javascript_observations = service->javascript_observations,
                               .javascript_defer = service->javascript_defer,
                               .layout_reservations = service->layout_reservations,
                               .now = (uint64_t)time(NULL)};
}

/*
 * The shared HTTP finalizer deliberately has a substantial stack footprint.
 * Static delivery runs it from the proxy worker, which already owns request
 * and origin framing buffers.  Keep this per-request state off that worker
 * stack so a perfectly ordinary transformed static document cannot exhaust
 * the POSIX thread stack.
 */
typedef struct {
  laghu_http_header request_headers[LAGHU_HTTP_MAX_REQUEST_HEADERS];
  laghu_http_header response_headers[LAGHU_HTTP_MAX_RESPONSE_HEADERS];
  proxy_response response;
  laghu_http_request normalized_request;
  laghu_http_response normalized_response;
  laghu_http_environment environment;
  laghu_http_transaction transaction;
  laghu_http_transaction_result prepared;
  laghu_http_transaction_result finalized;
  char cache_control[256U];
  char etag_wire[112U];
  char range_wire[96U];
} static_transform_context;

static bool static_send_transformed(const laghu_proxy_options *options, const proxy_connection *connection, proxy_worker *worker,
                                    const proxy_request *request, const laghu_config *core, const laghu_service_config *service,
                                    const laghu_proxy_rules *rules, laghu_socket client, SSL *tls, proxy_access_log *access, int file,
                                    const unsigned char *source, const char *type, const char *etag, size_t offset, size_t length,
                                    size_t total_length, bool partial, bool head) {
  static_transform_context *context = calloc(1U, sizeof(*context));
  unsigned char *body = NULL;
  unsigned char *compressed = NULL;
  size_t compressed_length = 0U;
  laghu_buffer selected;
  size_t index;
  bool prepared_ok;
  bool transformed = false;
  bool gzip = false;
  bool sent;
  if (context == NULL) return false;
#define static_request_headers (context->request_headers)
#define static_response_headers (context->response_headers)
#define response (context->response)
#define normalized_request (context->normalized_request)
#define normalized_response (context->normalized_response)
#define environment (context->environment)
#define transaction (context->transaction)
#define prepared (context->prepared)
#define finalized (context->finalized)
#define cache_control (context->cache_control)
#define etag_wire (context->etag_wire)
#define range_wire (context->range_wire)
  if (length != 0U && (body = malloc(length)) == NULL) goto fail;
  if (length != 0U && source != NULL)
    memcpy(body, source, length);
  else if (length != 0U && !static_read_full(file, offset, body, length))
    goto fail;
  (void)snprintf(response.reason, sizeof(response.reason), "%s", partial ? "Partial Content" : "OK");
  response.status = partial ? 206U : 200U;
  response.headers[response.header_count++] = (proxy_header){"Content-Type", (char *)type};
  if (etag != NULL) {
    (void)snprintf(etag_wire, sizeof(etag_wire), "%s", etag);
    response.headers[response.header_count++] = (proxy_header){"ETag", etag_wire};
  }
  if (partial) {
    int range_length = snprintf(range_wire, sizeof(range_wire), "bytes %llu-%llu/%llu", (unsigned long long)offset,
                                (unsigned long long)(offset + length - 1U), (unsigned long long)total_length);
    if (range_length <= 0 || (size_t)range_length >= sizeof(range_wire)) goto fail;
    response.headers[response.header_count++] = (proxy_header){"Accept-Ranges", "bytes"};
    response.headers[response.header_count++] = (proxy_header){"Content-Range", range_wire};
  }
  if (options->static_cache_control[0] != '\0') {
    (void)snprintf(cache_control, sizeof(cache_control), "%s", options->static_cache_control);
    response.headers[response.header_count++] = (proxy_header){"Cache-Control", cache_control};
  }
  for (index = 0U; index < options->response_header_count && response.header_count < LAGHU_HTTP_MAX_RESPONSE_HEADERS; ++index)
    response.headers[response.header_count++] =
        (proxy_header){(char *)options->response_headers[index].name, (char *)options->response_headers[index].value};
  for (index = 0U; index < request->header_count; ++index)
    static_request_headers[index] = (laghu_http_header){{(unsigned char *)request->headers[index].name, strlen(request->headers[index].name)},
                                                        {(unsigned char *)request->headers[index].value, strlen(request->headers[index].value)}};
  for (index = 0U; index < response.header_count; ++index)
    static_response_headers[index] = (laghu_http_header){{(unsigned char *)response.headers[index].name, strlen(response.headers[index].name)},
                                                         {(unsigned char *)response.headers[index].value, strlen(response.headers[index].value)}};
  normalized_request = (laghu_http_request){{(unsigned char *)request->method, strlen(request->method)},
                                            {(unsigned char *)proxy_effective_scheme(core, service, connection, request),
                                             strlen(proxy_effective_scheme(core, service, connection, request))},
                                            {NULL, 0U},
                                            {(unsigned char *)request->target, strlen(request->target)},
                                            static_request_headers,
                                            request->header_count};
  {
    proxy_header *host = proxy_find((proxy_header *)request->headers, request->header_count, "Host");
    normalized_request.authority = (laghu_buffer){(unsigned char *)(host == NULL ? options->listen_host : host->value),
                                                  strlen(host == NULL ? options->listen_host : host->value)};
  }
  normalized_response = (laghu_http_response){response.status,
                                              static_response_headers,
                                              response.header_count,
                                              length,
                                              !partial,
                                              true,
                                              partial,
                                              {NULL, 0U}};
  static_environment(&environment, worker, core, service);
  laghu_http_transaction_init(&transaction);
  prepared_ok = laghu_http_transaction_prepare(&transaction, &normalized_request, &normalized_response, &environment, &prepared);
  selected = (laghu_buffer){body, length};
  if (!partial && prepared_ok && prepared.action == LAGHU_HTTP_ACTION_SERVE_CACHED && proxy_materialize_cached_result(&prepared)) {
    selected = prepared.selected;
  } else if (!partial && prepared_ok && prepared.action != LAGHU_HTTP_ACTION_BYPASS && !proxy_is_forcing(worker->queue) &&
             laghu_http_transaction_finalize(&transaction, (laghu_buffer){body, length}, &finalized)) {
    selected = finalized.selected;
    transformed = true;
  }
  if (!partial && !head && rules != NULL && rules->compression == LAGHU_PROXY_COMPRESSION_GZIP && core->mode != LAGHU_MODE_OFF &&
      response.header_count + 2U <= LAGHU_HTTP_MAX_RESPONSE_HEADERS && static_accepts_gzip(request) && static_compressible(type) &&
      selected.length >= 256U && static_gzip(selected, &compressed, &compressed_length) && compressed_length < selected.length) {
    if (etag != NULL) {
      size_t etag_length = strlen(etag);
      if (etag_length < 2U || etag[0] != '"' || etag[etag_length - 1U] != '"') goto fail;
      (void)snprintf(etag_wire, sizeof(etag_wire), "W/\"%.*s-gzip\"", (int)(etag_length - 2U), etag + 1U);
      response.headers[1].value = etag_wire;
    }
    response.headers[response.header_count++] = (proxy_header){"Content-Encoding", "gzip"};
    response.headers[response.header_count++] = (proxy_header){"Vary", "Accept-Encoding"};
    selected = (laghu_buffer){compressed, compressed_length};
    gzip = true;
  }
  if (prepared_ok) {
    laghu_http_transaction_result *result = transformed ? &finalized : &prepared;
    result->not_modified = !gzip && laghu_http_request_matches_result_etag(&normalized_request, result);
    sent = (!result->not_modified && !proxy_send_early_hints(client, tls, request->version, result))
               ? false
               : (head ? proxy_send_headers(client, tls, &response, result, selected.length, true)
                       : proxy_send_result(client, tls, &response, result, selected));
    access->decision = result->decision;
    access->job_published = result->job_published;
  } else {
    sent = head ? proxy_send_headers(client, tls, &response, NULL, selected.length, true) : proxy_send_result(client, tls, &response, NULL, selected);
    access->decision = LAGHU_DECISION_BYPASS_ERROR;
  }
  access->static_response = true;
  (void)snprintf(access->route, sizeof(access->route), "%s", "static");
  access->status = response.status;
  access->original_response_bytes = length;
  access->output_bytes = head ? 0U : selected.length;
  access->compressed = gzip;
  if (!sent) access->failure = "client_disconnect";
  laghu_http_transaction_result_release(&prepared);
  laghu_http_transaction_result_release(&finalized);
  free(compressed);
  free(body);
  free(context);
#undef static_request_headers
#undef static_response_headers
#undef response
#undef normalized_request
#undef normalized_response
#undef environment
#undef transaction
#undef prepared
#undef finalized
#undef cache_control
#undef etag_wire
#undef range_wire
  return true;
fail:
  free(body);
  free(context);
#undef static_request_headers
#undef static_response_headers
#undef response
#undef normalized_request
#undef normalized_response
#undef environment
#undef transaction
#undef prepared
#undef finalized
#undef cache_control
#undef etag_wire
#undef range_wire
  return false;
}

static int static_open(const char *root, const char *relative, const char *index_file) {
  char components[LAGHU_RUNTIME_PATH_SIZE];
  char *cursor;
  int directory;
  int file = -1;
  (void)snprintf(components, sizeof(components), "%s%s", relative + 1U, relative[strlen(relative) - 1U] == '/' ? index_file : "");
  directory = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (directory < 0) return -1;
  cursor = components;
  for (;;) {
    char *separator = strchr(cursor, '/');
    int next;
    if (separator != NULL) *separator = '\0';
    if (cursor[0] == '\0') {
      file = directory;
      break;
    }
    if (!strcmp(cursor, ".") || !strcmp(cursor, "..")) break;
    next = openat(directory, cursor, O_RDONLY | O_NOFOLLOW | (separator == NULL ? 0 : O_DIRECTORY));
    close(directory);
    directory = next;
    if (directory < 0) return -1;
    if (separator == NULL) {
      file = directory;
      break;
    }
    cursor = separator + 1U;
  }
  if (file < 0) close(directory);
  return file;
}

static bool static_directory(const laghu_proxy_options *options, const proxy_connection *connection, proxy_worker *worker,
                             const proxy_request *request, const laghu_config *core, const laghu_service_config *service,
                             const laghu_proxy_rules *rules, laghu_socket client, SSL *tls, int file, bool head, proxy_access_log *access) {
  unsigned char body[16384];
  size_t used = 0U;
  DIR *directory = fdopendir(file);
  struct dirent *entry;
  if (directory == NULL) return false;
  while ((entry = readdir(directory)) != NULL) {
    size_t length = strlen(entry->d_name);
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || length > 240U || used + length + 1U >= sizeof(body)) continue;
    memcpy(body + used, entry->d_name, length);
    used += length;
    body[used++] = '\n';
  }
  (void)closedir(directory);
  if (worker != NULL && connection != NULL && core != NULL && service != NULL)
    return static_send_transformed(options, connection, worker, request, core, service, rules, client, tls, access, LAGHU_INVALID_SOCKET, body,
                                   "text/plain; charset=utf-8", NULL, 0U, used, used, false, head);
  {
    char headers[256];
    int header_length = snprintf(headers, sizeof(headers),
                                 "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                                 "Content-Length: %llu\r\n\r\n",
                                 (unsigned long long)used);
    if (header_length <= 0 || (size_t)header_length >= sizeof(headers) || !proxy_client_send_all(client, tls, headers, (size_t)header_length) ||
        (!head && !proxy_client_send_all(client, tls, body, used)))
      return false;
  }
  access->status = 200U;
  access->output_bytes = used;
  return true;
}

bool proxy_static_serve_with_context(const laghu_proxy_options *options, const proxy_connection *connection, proxy_worker *worker,
                                     const proxy_request *request, const laghu_config *core, const laghu_service_config *service,
                                     const laghu_proxy_rules *rules, laghu_socket client, SSL *tls, proxy_access_log *access) {
  char relative[LAGHU_RUNTIME_PATH_SIZE], path[LAGHU_RUNTIME_PATH_SIZE], etag[96], headers[4096];
  const proxy_header *range;
  const proxy_header *if_none_match;
  struct stat status;
  size_t offset = 0U, length;
  int file;
  int written;
  bool head;
  size_t header_index;
  const char *document_root;
  const char *index_file;
  unsigned char buffer[16384];
  if (rules == NULL) rules = &options->rules;
  {
    size_t site_index = proxy_site_index(options, request);
    const laghu_proxy_site *site = site_index == LAGHU_PROXY_SITE_GLOBAL ? NULL : &options->sites[site_index];
    document_root = site == NULL ? options->document_root : site->document_root;
    index_file = site == NULL ? options->index_file : site->index_file;
  }
  if (document_root[0] == '\0') return false;
  if (strcmp(request->method, "GET") != 0 && strcmp(request->method, "HEAD") != 0) return false;
  if (!static_target_safe(request->target, relative)) {
    proxy_error_response(client, tls, 400U, "Bad Request");
    access->status = 400U;
    access->failure = "static_path";
    return true;
  }
  if (snprintf(path, sizeof(path), "%s%s%s", document_root, relative, relative[strlen(relative) - 1U] == '/' ? index_file : "") <= 0 ||
      strlen(path) >= sizeof(path)) {
    proxy_error_response(client, tls, 414U, "URI Too Long");
    access->status = 414U;
    return true;
  }
  file = static_open(document_root, relative, index_file);
  if (file < 0 && options->directory_listing && relative[strlen(relative) - 1U] == '/') file = static_open(document_root, relative, "");
  if (file < 0 && rules->spa_fallback[0] != '\0' && strcmp(relative, rules->spa_fallback) != 0 && static_target_safe(rules->spa_fallback, relative)) {
    if (snprintf(path, sizeof(path), "%s%s", document_root, relative) <= 0 || strlen(path) >= sizeof(path)) {
      proxy_error_response(client, tls, 414U, "URI Too Long");
      access->status = 414U;
      return true;
    }
    file = static_open(document_root, relative, index_file);
    if (file >= 0) access->spa_fallback = true;
  }
  if (file < 0) return false;
  if (fstat(file, &status) != 0) {
    (void)close(file);
    proxy_error_response(client, tls, 404U, "Not Found");
    access->status = 404U;
    return true;
  }
  if (S_ISDIR(status.st_mode)) {
    if (options->directory_listing)
      return static_directory(options, connection, worker, request, core, service, rules, client, tls, file, strcmp(request->method, "HEAD") == 0,
                              access);
    (void)close(file);
    proxy_error_response(client, tls, 403U, "Forbidden");
    access->status = 403U;
    return true;
  }
  if (!S_ISREG(status.st_mode) || status.st_size < 0 || (uintmax_t)status.st_size > rules->static_max_bytes ||
      (uintmax_t)status.st_size > LAGHU_STATIC_MAX_BYTES) {
    (void)close(file);
    proxy_error_response(client, tls, 413U, "Payload Too Large");
    access->status = 413U;
    access->failure = "static_limit";
    return true;
  }
  length = (size_t)status.st_size;
  (void)snprintf(etag, sizeof(etag), "\"%llx-%llx\"", (unsigned long long)status.st_mtime, (unsigned long long)length);
  if_none_match = proxy_find((proxy_header *)request->headers, request->header_count, "If-None-Match");
  head = strcmp(request->method, "HEAD") == 0;
  if (worker == NULL && if_none_match != NULL && strcmp(if_none_match->value, etag) == 0) {
    written = snprintf(headers, sizeof(headers), "HTTP/1.1 304 Not Modified\r\nETag: %s\r\nContent-Length: 0\r\n\r\n", etag);
    (void)close(file);
    if (written > 0) (void)proxy_client_send_all(client, tls, headers, (size_t)written);
    access->status = 304U;
    return true;
  }
  range = proxy_find((proxy_header *)request->headers, request->header_count, "Range");
  if (range != NULL && !static_range(range->value, length, &offset, &length)) {
    written = snprintf(headers, sizeof(headers), "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%llu\r\nContent-Length: 0\r\n\r\n",
                       (unsigned long long)status.st_size);
    (void)close(file);
    if (written > 0) (void)proxy_client_send_all(client, tls, headers, (size_t)written);
    access->status = 416U;
    return true;
  }
  if (worker != NULL && connection != NULL && core != NULL && service != NULL) {
    bool transformed = static_send_transformed(options, connection, worker, request, core, service, rules, client, tls, access, file, NULL,
                                               static_type(path), etag, offset, length, (size_t)status.st_size, range != NULL, head);
    (void)close(file);
    if (!transformed) {
      proxy_error_response(client, tls, 500U, "Internal Server Error");
      access->status = 500U;
      access->failure = "static_read";
    }
    return true;
  }
  if (range == NULL)
    written = snprintf(headers, sizeof(headers),
                       "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
                       "Accept-Ranges: bytes\r\nETag: %s\r\n\r\n",
                       static_type(path), (unsigned long long)length, etag);
  else
    written = snprintf(headers, sizeof(headers),
                       "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
                       "Accept-Ranges: bytes\r\nContent-Range: bytes %llu-%llu/%llu\r\nETag: %s\r\n\r\n",
                       static_type(path), (unsigned long long)length, (unsigned long long)offset, (unsigned long long)(offset + length - 1U),
                       (unsigned long long)status.st_size, etag);
  if (written > 0 && (size_t)written < sizeof(headers)) {
    size_t used = (size_t)written - 2U;
    if (options->static_cache_control[0] != '\0') {
      int added = snprintf(headers + used, sizeof(headers) - used, "Cache-Control: %s\r\n", options->static_cache_control);
      if (added <= 0 || (size_t)added >= sizeof(headers) - used)
        written = -1;
      else
        used += (size_t)added;
    }
    for (header_index = 0U; header_index < options->response_header_count; ++header_index) {
      int added = snprintf(headers + used, sizeof(headers) - used, "%s: %s\r\n", options->response_headers[header_index].name,
                           options->response_headers[header_index].value);
      if (added <= 0 || (size_t)added >= sizeof(headers) - used) {
        written = -1;
        break;
      }
      used += (size_t)added;
    }
    if (written > 0) {
      headers[used++] = '\r';
      headers[used++] = '\n';
      written = (int)used;
    }
  }
  if (written <= 0 || (size_t)written >= sizeof(headers) || !proxy_client_send_all(client, tls, headers, (size_t)written)) {
    (void)close(file);
    access->failure = "client_disconnect";
    return true;
  }
  if (!head && lseek(file, (off_t)offset, SEEK_SET) >= 0) {
    size_t remaining = length;
    while (remaining != 0U) {
      ssize_t got = read(file, buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
      if (got <= 0 || !proxy_client_send_all(client, tls, buffer, (size_t)got)) {
        access->failure = "client_disconnect";
        break;
      }
      remaining -= (size_t)got;
    }
  }
  (void)close(file);
  access->status = range == NULL ? 200U : 206U;
  access->output_bytes = length;
  return true;
}

bool proxy_static_serve(const laghu_proxy_options *options, const proxy_request *request, laghu_socket client, SSL *tls, proxy_access_log *access) {
  return proxy_static_serve_with_context(options, NULL, NULL, request, NULL, NULL, &options->rules, client, tls, access);
}
