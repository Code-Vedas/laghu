// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/status.h"

#include <ctype.h>
#include <errno.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server_internal.h"

#define STATUS_HEADER_LIMIT 16384U
#define STATUS_BODY_LIMIT 32768U
#define STATUS_HEADER_COUNT 64U

typedef struct {
  char host[256];
  char authority[264];
  char port[6];
  bool tls;
} status_origin;

static bool status_uint(const char *value, unsigned int minimum,
                        unsigned int maximum, unsigned int *output) {
  char *end = NULL;
  unsigned long parsed;
  if (value == NULL || *value == '\0') return false;
  errno = 0;
  parsed = strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < minimum ||
      parsed > maximum)
    return false;
  *output = (unsigned int)parsed;
  return true;
}

static bool status_origin_parse(const char *value, status_origin *origin) {
  const char *authority, *colon;
  unsigned int port;
  size_t host_length, index;
  memset(origin, 0, sizeof(*origin));
  if (value != NULL && strncmp(value, "http://", 7U) == 0) {
    origin->tls = false;
    authority = value + 7U;
    port = 80U;
  } else if (value != NULL && strncmp(value, "https://", 8U) == 0) {
    origin->tls = true;
    authority = value + 8U;
    port = 443U;
  } else {
    return false;
  }
  if (*authority == '\0' || strpbrk(authority, "/?#@\\\r\n\t ") != NULL)
    return false;
  if (*authority == '[') {
    const char *end = strchr(authority, ']');
    if (end == NULL || (end[1] != '\0' && end[1] != ':')) return false;
    host_length = (size_t)(end - authority - 1U);
    if (host_length == 0U || host_length >= sizeof(origin->host) ||
        inet_pton(AF_INET6, authority + 1, (unsigned char[16]){0}) != 1)
      return false;
    memcpy(origin->host, authority + 1, host_length);
    if (end[1] == ':' && !status_uint(end + 2U, 1U, 65535U, &port)) return false;
  } else {
    colon = strrchr(authority, ':');
    if (colon != NULL) {
      if (strchr(authority, ':') != colon) return false;
      host_length = (size_t)(colon - authority);
      if (!status_uint(colon + 1U, 1U, 65535U, &port)) return false;
    } else {
      host_length = strlen(authority);
    }
    if (host_length == 0U || host_length >= sizeof(origin->host)) return false;
    memcpy(origin->host, authority, host_length);
  }
  origin->host[host_length] = '\0';
  for (index = 0U; origin->host[index] != '\0'; ++index)
    if (!(isalnum((unsigned char)origin->host[index]) || origin->host[index] ==
                                                       '.' ||
          origin->host[index] == '-'))
      return false;
  if (snprintf(origin->port, sizeof(origin->port), "%u", port) < 1 ||
      snprintf(origin->authority, sizeof(origin->authority),
               strchr(origin->host, ':') != NULL ? "[%s]:%u" : "%s:%u",
               origin->host, port) < 1)
    return false;
  return true;
}

static bool status_token(const char *path, char token[257]) {
  struct stat information;
  FILE *file;
  size_t length;
  if (path == NULL || path[0] != '/' || lstat(path, &information) != 0 ||
      S_ISLNK(information.st_mode) || !S_ISREG(information.st_mode) ||
      information.st_uid != getuid() || (information.st_mode & 0077U) != 0)
    return false;
  file = fopen(path, "rb");
  if (file == NULL) return false;
  length = fread(token, 1U, 256U, file);
  if (ferror(file) || fgetc(file) != EOF) {
    fclose(file);
    return false;
  }
  fclose(file);
  while (length != 0U && (token[length - 1U] == '\r' ||
                          token[length - 1U] == '\n'))
    --length;
  if (length < 16U) return false;
  for (size_t index = 0U; index < length; ++index)
    if ((unsigned char)token[index] < 33U || (unsigned char)token[index] > 126U)
      return false;
  token[length] = '\0';
  return true;
}

static SSL *status_tls(SSL_CTX *context, int socket, const status_origin *origin) {
  SSL *tls = SSL_new(context);
  X509_VERIFY_PARAM *parameters;
  if (tls == NULL) return NULL;
  parameters = SSL_get0_param(tls);
  if ((inet_pton(AF_INET, origin->host, (unsigned char[4]){0}) == 1 &&
       !X509_VERIFY_PARAM_set1_ip_asc(parameters, origin->host)) ||
      (inet_pton(AF_INET, origin->host, (unsigned char[4]){0}) != 1 &&
       !SSL_set_tlsext_host_name(tls, origin->host)) || !SSL_set1_host(tls, origin->host) ||
      !SSL_set_fd(tls, socket) || SSL_connect(tls) != 1) {
    SSL_free(tls);
    return NULL;
  }
  return tls;
}

static int status_connect(const status_origin *origin, unsigned int timeout) {
  struct addrinfo hints = {0}, *addresses = NULL, *item;
  int status_socket = -1;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  if (getaddrinfo(origin->host, origin->port, &hints, &addresses) != 0) return -1;
  for (item = addresses; item != NULL; item = item->ai_next) {
    status_socket = (int)socket(item->ai_family, item->ai_socktype, item->ai_protocol);
    if (status_socket < 0) continue;
    {
      struct timeval value = {(time_t)timeout, 0};
      (void)setsockopt(status_socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
      (void)setsockopt(status_socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
    }
    if (connect(status_socket, item->ai_addr, item->ai_addrlen) == 0) break;
    close(status_socket);
    status_socket = -1;
  }
  freeaddrinfo(addresses);
  return status_socket;
}

/* Strict HTTP/1.1 fixed-length JSON fetch.  Returns 0 success, status code or
 * negative local/framing failure. */
static int status_fetch(const status_origin *origin, SSL_CTX *context,
                        const char *token, const char *path, unsigned int timeout,
                        char body[STATUS_BODY_LIMIT + 1U], size_t *body_length) {
  char request[1024], header[STATUS_HEADER_LIMIT + 1U];
  int socket = status_connect(origin, timeout), received, request_length;
  SSL *tls = NULL;
  char *line, *next, *header_end, *content_length = NULL, *content_type = NULL;
  unsigned int declared_length;
  size_t length;
  unsigned int headers = 0U, status;
  if (socket < 0) return -1;
  if (origin->tls && (tls = status_tls(context, socket, origin)) == NULL) {
    close(socket); return -1;
  }
  request_length = snprintf(request, sizeof(request),
                            "GET %s HTTP/1.1\r\nHost: %s\r\nAccept: application/json\r\n"
                            "X-Laghu-Purge-Token: %s\r\nConnection: close\r\n\r\n",
                            path, origin->authority, token);
  if (request_length < 0 || (size_t)request_length >= sizeof(request) ||
      (tls != NULL ? SSL_write(tls, request, request_length) :
                     send(socket, request, (size_t)request_length, 0)) != request_length) {
    SSL_free(tls); close(socket); return -1;
  }
  size_t used = 0U;
  while (used < STATUS_HEADER_LIMIT &&
         (received = tls != NULL ? SSL_read(tls, header + used, STATUS_HEADER_LIMIT - used) :
                                   recv(socket, header + used, STATUS_HEADER_LIMIT - used, 0)) > 0) {
    used += (size_t)received;
    header[used] = '\0';
    if (used >= 4U && (next = strstr(header, "\r\n\r\n")) != NULL) break;
  }
  if (received <= 0 || used >= STATUS_HEADER_LIMIT || (next = strstr(header, "\r\n\r\n")) == NULL) {
    SSL_free(tls); close(socket); return -2;
  }
  header_end = next;
  *header_end = '\0';
  if (sscanf(header, "HTTP/1.1 %u", &status) != 1 || status < 100U || status > 599U) {
    SSL_free(tls); close(socket); return -2;
  }
  line = strstr(header, "\r\n");
  if (line == NULL) { SSL_free(tls); close(socket); return -2; }
  line += 2U;
  while (*line != '\0') {
    char *colon;
    next = strstr(line, "\r\n");
    if (next == NULL) { SSL_free(tls); close(socket); return -2; }
    *next = '\0';
    if (++headers > STATUS_HEADER_COUNT || (colon = strchr(line, ':')) == NULL ||
        strchr(line, '\r') != NULL || strchr(line, '\n') != NULL) { SSL_free(tls); close(socket); return -2; }
    *colon++ = '\0'; while (*colon == ' ' || *colon == '\t') ++colon;
    if (strcasecmp(line, "Content-Length") == 0) { if (content_length != NULL) { SSL_free(tls); close(socket); return -2; } content_length = colon; }
    else if (strcasecmp(line, "Content-Type") == 0) content_type = colon;
    else if (strcasecmp(line, "Transfer-Encoding") == 0) { SSL_free(tls); close(socket); return -2; }
    line = next + 2U;
  }
  if (content_length == NULL || content_type == NULL || strncasecmp(content_type, "application/json", 16U) != 0 ||
      !status_uint(content_length, 0U, STATUS_BODY_LIMIT, &declared_length)) { SSL_free(tls); close(socket); return -2; }
  length = declared_length;
  size_t initial = used - ((size_t)(header_end + 4U - header));
  if (initial > length) { SSL_free(tls); close(socket); return -2; }
  memcpy(body, header_end + 4U, initial); used = initial;
  while (used < length && (received = tls != NULL ? SSL_read(tls, body + used, length - used) : recv(socket, body + used, length - used, 0)) > 0) used += (size_t)received;
  SSL_free(tls); close(socket);
  if (used != length) return -2;
  body[used] = '\0'; *body_length = used;
  return (int)status;
}

int laghu_status_run(int argc, char **argv) {
  status_origin origin; char token[257], ready[STATUS_BODY_LIMIT + 1U], stats[STATUS_BODY_LIMIT + 1U];
  const char *token_file = NULL, *ca_file = NULL; unsigned int timeout = 5U; bool json = false; SSL_CTX *context = NULL;
  int index, ready_status, stats_status; size_t ready_length, stats_length;
  if (argc < 2 || !status_origin_parse(argv[1], &origin)) goto usage;
  for (index = 2; index < argc; ++index) {
    if (strcmp(argv[index], "--token-file") == 0 && index + 1 < argc) token_file = argv[++index];
    else if (strcmp(argv[index], "--timeout") == 0 && index + 1 < argc && status_uint(argv[++index], 1U, 30U, &timeout)) {}
    else if (strcmp(argv[index], "--ca-file") == 0 && index + 1 < argc) ca_file = argv[++index];
    else if (strcmp(argv[index], "--json") == 0) json = true;
    else goto usage;
  }
  if (token_file == NULL || !status_token(token_file, token) || (ca_file != NULL && !origin.tls)) goto usage;
  if (origin.tls) {
    context = SSL_CTX_new(TLS_client_method());
    if (context == NULL || !SSL_CTX_set_default_verify_paths(context) ||
        (ca_file != NULL && !SSL_CTX_load_verify_locations(context, ca_file, NULL))) { SSL_CTX_free(context); fputs("laghu status: connection failed\n", stderr); return 4; }
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
  }
  ready_status = status_fetch(&origin, context, token, "/.laghu/ready", timeout, ready, &ready_length);
  stats_status = status_fetch(&origin, context, token, "/.laghu/stats", timeout, stats, &stats_length);
  SSL_CTX_free(context);
  if (ready_status == 401 || ready_status == 403 || stats_status == 401 || stats_status == 403) { fputs("laghu status: authorization failed\n", stderr); return 3; }
  if (ready_status < 0 || stats_status < 0) { fputs(ready_status == -2 || stats_status == -2 ? "laghu status: malformed response\n" : "laghu status: connection failed\n", stderr); return ready_status == -2 || stats_status == -2 ? 5 : 4; }
  if (ready_status == 503 || stats_status == 503) { fputs("laghu status: service unavailable\n", stderr); return 6; }
  if (ready_status != 200 || stats_status != 200) { fprintf(stderr, "laghu status: HTTP %d\n", ready_status != 200 ? ready_status : stats_status); return 7; }
  if (strstr(ready, "\"status\"") == NULL || strstr(stats, "\"schema\":\"laghu-cache-stats-v1\"") == NULL) { fputs("laghu status: malformed response\n", stderr); return 5; }
  if (json) printf("{\"schema\":\"laghu-status-v1\",\"ready\":%s,\"stats\":%s}\n", ready, stats);
  else printf("ready: %s\ncache: hits/misses %s/%s\n", strstr(ready, "\"status\":\"") + 10U, strstr(stats, "\"hits\":") + 7U, strstr(stats, "\"misses\":") + 9U);
  return 0;
usage:
  fputs("Usage: laghu status URL --token-file PATH [--timeout SECONDS] [--ca-file PATH] [--json]\n", stderr);
  return 2;
}
