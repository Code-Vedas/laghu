// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

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

static const laghu_proxy_site *static_site(const laghu_proxy_options *options, const proxy_request *request) {
  proxy_header *host = proxy_find((proxy_header *)request->headers, request->header_count, "Host");
  size_t index;
  if (host == NULL) return NULL;
  for (index = 0U; index < options->site_count; ++index) {
    size_t length = strlen(options->sites[index].host);
    if (!strncasecmp(host->value, options->sites[index].host, length) &&
        (host->value[length] == '\0' || host->value[length] == ':'))
      return &options->sites[index];
  }
  return NULL;
}

static bool static_range(const char *value, size_t total, size_t *start, size_t *length) {
  unsigned long long first, last;
  char *end = NULL;
  if (value == NULL || strncmp(value, "bytes=", 6U) != 0 || strchr(value + 6U, ',') != NULL) return false;
  errno = 0;
  first = strtoull(value + 6U, &end, 10);
  if (errno != 0 || end == value + 6U || *end != '-') return false;
  if (end[1] == '\0') last = total == 0U ? 0U : total - 1U;
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

static int static_open(const char *root, const char *relative, const char *index_file) {
  char components[LAGHU_RUNTIME_PATH_SIZE];
  char *cursor;
  int directory;
  int file = -1;
  (void)snprintf(components, sizeof(components), "%s%s", relative + 1U,
                 relative[strlen(relative) - 1U] == '/' ? index_file : "");
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

static bool static_directory(laghu_socket client, SSL *tls, int file, bool head, proxy_access_log *access) {
  char body[16384];
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
  {
    char headers[256];
    int header_length = snprintf(headers, sizeof(headers), "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                                                       "Content-Length: %llu\r\n\r\n", (unsigned long long)used);
    if (header_length <= 0 || (size_t)header_length >= sizeof(headers) || !proxy_client_send_all(client, tls, headers, (size_t)header_length) ||
        (!head && !proxy_client_send_all(client, tls, body, used)))
      return false;
  }
  access->status = 200U;
  access->output_bytes = used;
  return true;
}

bool proxy_static_serve(const laghu_proxy_options *options, const proxy_request *request, laghu_socket client, SSL *tls,
                        proxy_access_log *access) {
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
  {
    const laghu_proxy_site *site = static_site(options, request);
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
  if (snprintf(path, sizeof(path), "%s%s%s", document_root, relative,
               relative[strlen(relative) - 1U] == '/' ? index_file : "") <= 0 ||
      strlen(path) >= sizeof(path)) {
    proxy_error_response(client, tls, 414U, "URI Too Long");
    access->status = 414U;
    return true;
  }
  file = static_open(document_root, relative, index_file);
  if (file < 0 && options->directory_listing && relative[strlen(relative) - 1U] == '/')
    file = static_open(document_root, relative, "");
  if (file < 0) return false;
  if (fstat(file, &status) != 0) {
    (void)close(file);
    proxy_error_response(client, tls, 404U, "Not Found");
    access->status = 404U;
    return true;
  }
  if (S_ISDIR(status.st_mode)) {
    if (options->directory_listing) return static_directory(client, tls, file, strcmp(request->method, "HEAD") == 0, access);
    (void)close(file);
    proxy_error_response(client, tls, 403U, "Forbidden");
    access->status = 403U;
    return true;
  }
  if (!S_ISREG(status.st_mode) || status.st_size < 0 || (uintmax_t)status.st_size > LAGHU_STATIC_MAX_BYTES) {
    (void)close(file);
    proxy_error_response(client, tls, 404U, "Not Found");
    access->status = 404U;
    return true;
  }
  length = (size_t)status.st_size;
  (void)snprintf(etag, sizeof(etag), "\"%llx-%llx\"", (unsigned long long)status.st_mtime, (unsigned long long)length);
  if_none_match = proxy_find((proxy_header *)request->headers, request->header_count, "If-None-Match");
  head = strcmp(request->method, "HEAD") == 0;
  if (if_none_match != NULL && strcmp(if_none_match->value, etag) == 0) {
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
  if (range == NULL)
    written = snprintf(headers, sizeof(headers), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
                                              "Accept-Ranges: bytes\r\nETag: %s\r\n\r\n",
                       static_type(path), (unsigned long long)length, etag);
  else
    written = snprintf(headers, sizeof(headers), "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
                                              "Accept-Ranges: bytes\r\nContent-Range: bytes %llu-%llu/%llu\r\nETag: %s\r\n\r\n",
                       static_type(path), (unsigned long long)length, (unsigned long long)offset,
                       (unsigned long long)(offset + length - 1U), (unsigned long long)status.st_size, etag);
  if (written > 0 && (size_t)written < sizeof(headers)) {
    size_t used = (size_t)written - 2U;
    if (options->static_cache_control[0] != '\0') {
      int added = snprintf(headers + used, sizeof(headers) - used, "Cache-Control: %s\r\n", options->static_cache_control);
      if (added <= 0 || (size_t)added >= sizeof(headers) - used) written = -1;
      else used += (size_t)added;
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
