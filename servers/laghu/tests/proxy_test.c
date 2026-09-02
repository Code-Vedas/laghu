// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/proxy.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "server_internal.h"

#define CHECK(value)                                          \
  do {                                                        \
    if (!(value)) {                                           \
      fprintf(stderr, "check failed at line %d\n", __LINE__); \
      return 1;                                               \
    }                                                         \
  } while (0)

#define TEST_BACKEND_URI "file:///tmp/cache"

static bool service_apply(laghu_service_config *config, laghu_service_setting setting, const char *value) {
  laghu_service_diagnostic diagnostic;
  return laghu_service_config_apply(config, setting, value, &diagnostic);
}

static bool service_finalize(laghu_service_config *config) {
  laghu_service_finalize_options options = {.native_file_loading = false,
                                            .require_cache = true,
                                            .require_worker_queue = true,
                                            .require_admin_authorization = true,
                                            .respect_x_forwarded_proto = false};
  laghu_service_diagnostic diagnostic;
  return laghu_service_config_finalize(config, &options, &diagnostic);
}

static bool proxy_test_path_join(char *target, size_t target_size, const char *base, const char *suffix) {
  size_t base_length = strlen(base);
  size_t suffix_length = strlen(suffix);
  if (base_length >= target_size || suffix_length >= target_size - base_length) return false;
  memcpy(target, base, base_length);
  memcpy(target + base_length, suffix, suffix_length + 1U);
  return true;
}

static bool runtime_queue_capabilities_cache_test(void) {
  char directory[] = "/tmp/laghu-runtime-capabilities-XXXXXX";
  char path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_runtime_queue runtime_queue;
  proxy_queue queue = {0};
  proxy_worker worker = {0};
  uint64_t previous;
  bool result = false;
  if (mkdtemp(directory) == NULL || !proxy_test_path_join(path, sizeof(path), directory, "/jobs.queue")) return false;
  laghu_runtime_queue_init(&runtime_queue);
  if (!laghu_runtime_queue_create(&runtime_queue, path, 1U, 64U) || !laghu_runtime_queue_set_backend(&runtime_queue, 7U, "test-worker") ||
      !laghu_runtime_queue_heartbeat(&runtime_queue, (uint64_t)time(NULL)))
    goto done;
  queue.runtime_queue = runtime_queue;
  queue.runtime_queue_ready = true;
  worker.queue = &queue;
  if (proxy_runtime_queue_capabilities(&worker) != 7U) goto done;
  previous = (uint64_t)time(NULL);
  while ((uint64_t)time(NULL) == previous) {
  }
  worker.runtime_queue_capabilities_checked_at = (uint64_t)time(NULL);
  worker.runtime_queue_capabilities = 7U;
  if (!laghu_runtime_queue_set_backend(&queue.runtime_queue, 3U, "test-worker") || proxy_runtime_queue_capabilities(&worker) != 7U) goto done;
  worker.runtime_queue_capabilities_checked_at = 0U;
  result = proxy_runtime_queue_capabilities(&worker) == 3U;
done:
  laghu_runtime_queue_close(&queue.runtime_queue);
  (void)unlink(path);
  (void)rmdir(directory);
  return result;
}

static bool write_yaml_fixture(char path[]) {
  static const char fixture[] =
      ""
      "runtime:\n"
      "  listen: 127.0.0.1:8080\n"
      "  origin: http://127.0.0.1:8000\n"
      "  access_log: off\n"
      "  cache: /tmp/cache\n"
      "  worker_queue: /tmp/jobs\n"
      "  request_header_timeout: 7\n"
      "  request_body_timeout: 19\n"
      "  preset: balanced\n"
      "  trusted_proxy:\n"
      "    - 127.0.0.0/8\n"
      "  forwarded_headers: both\n"
      "  add_header:\n"
      "    - [X-Static-Policy, enabled]\n"
      "    - [X-Frame-Options, DENY]\n"
      "sites:\n"
      "  - host: static.example.test\n"
      "    document_root: /srv/static\n"
      "    laghu:\n"
      "      preset: safe\n"
      "    service:\n"
      "      javascript_target: defaults\n"
      "    routes:\n"
      "      - match: exact\n"
      "        pattern: /site-policy\n"
      "        redirect: /site-result\n"
      "        laghu:\n"
      "          mode: off\n"
      "routes:\n"
      "  - match: ordered_regex\n"
      "    pattern: ^/old/[0-9]+$\n"
      "    proxy_pass: https://127.0.0.1:9000\n"
      "    health_check: /healthz\n"
      "    health_interval: 9\n"
      "    failover:\n"
      "      - https://127.0.0.1:9001\n";
  int file = mkstemp(path);
  return file >= 0 && write(file, fixture, sizeof(fixture) - 1U) == (ssize_t)(sizeof(fixture) - 1U) && close(file) == 0;
}

static bool gateway_health_rejected_test(void) {
  static const char fixture[] =
      ""
      "runtime:\n"
      "  listen: 127.0.0.1:8080\n"
      "  origin: http://127.0.0.1:8000\n"
      "  cache: /tmp/cache\n"
      "  worker_queue: /tmp/jobs\n"
      "routes:\n"
      "  - match: exact\n"
      "    pattern: /app\n"
      "    proxy_pass: fastcgi://127.0.0.1:9000\n"
      "    health_check: /healthz\n";
  char path[] = "/tmp/laghu-gateway-health-XXXXXX";
  laghu_proxy_options options;
  char error[128U];
  int file = mkstemp(path);
  bool rejected;
  if (file < 0 || write(file, fixture, sizeof(fixture) - 1U) != (ssize_t)(sizeof(fixture) - 1U) || close(file) != 0) return false;
  laghu_proxy_options_init(&options);
  rejected = laghu_proxy_load_yaml(path, &options, error, sizeof(error)) != LAGHU_PROXY_PARSE_OK;
  laghu_proxy_options_dispose(&options);
  (void)unlink(path);
  return rejected;
}

static bool health_interval_requires_check_test(void) {
  static const char fixture[] =
      ""
      "runtime:\n"
      "  listen: 127.0.0.1:8080\n"
      "  origin: http://127.0.0.1:8000\n"
      "  cache: /tmp/cache\n"
      "  worker_queue: /tmp/jobs\n"
      "routes:\n"
      "  - match: exact\n"
      "    pattern: /app\n"
      "    proxy_pass: http://127.0.0.1:9000\n"
      "    health_interval: 1\n";
  char path[] = "/tmp/laghu-health-interval-XXXXXX";
  laghu_proxy_options options;
  char error[128U];
  int file = mkstemp(path);
  bool rejected;
  if (file < 0 || write(file, fixture, sizeof(fixture) - 1U) != (ssize_t)(sizeof(fixture) - 1U) || close(file) != 0) return false;
  laghu_proxy_options_init(&options);
  rejected = laghu_proxy_load_yaml(path, &options, error, sizeof(error)) != LAGHU_PROXY_PARSE_OK;
  laghu_proxy_options_dispose(&options);
  (void)unlink(path);
  return rejected;
}

static bool request_line_parse_test(void) {
  static const char valid_origin[] = "GET /path?encoded=%ZZ HTTP/1.1\r\nHost: example.test\r\n\r\n";
  static const char valid_double_slash[] = "GET //path?encoded=%ZZ HTTP/1.1\r\nHost: example.test\r\n\r\n";
  static const char valid_tchar[] = "!#$%&'*+-.^_`|~ / HTTP/1.0\r\n\r\n";
  static const char invalid_cases[][192U] = {
      "GET  / HTTP/1.1\r\nHost: example.test\r\n\r\n",
      "GET\t/ HTTP/1.1\r\nHost: example.test\r\n\r\n",
      "G\x01ET / HTTP/1.1\r\nHost: example.test\r\n\r\n",
      "GET /bad\tpath HTTP/1.1\r\nHost: example.test\r\n\r\n",
      "GET /bad\x1fpath HTTP/1.1\r\nHost: example.test\r\n\r\n",
      "GET /bad\x7fpath HTTP/1.1\r\nHost: example.test\r\n\r\n",
      "GET relative HTTP/1.1\r\nHost: example.test\r\n\r\n",
      "GET http://example.test/ HTTP/1.1\r\nHost: example.test\r\n\r\n",
      "CONNECT example.test:443 HTTP/1.1\r\nHost: example.test\r\n\r\n",
      "OPTIONS * HTTP/1.1\r\nHost: example.test\r\n\r\n",
      "GET /fragment#part HTTP/1.1\r\nHost: example.test\r\n\r\n",
      "GET / HTTP/1.2\r\nHost: example.test\r\n\r\n",
      "GET / HTTP/1.1 extra\r\nHost: example.test\r\n\r\n",
      "GET / HTTP/1.1\r\nHost: one.example\r\nHost: two.example\r\n\r\n",
      "POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
  };
  proxy_request request;
  size_t index;
  memset(&request, 0, sizeof(request));
  memcpy(request.storage, valid_origin, sizeof(valid_origin));
  if (!proxy_parse_request(&request, sizeof(valid_origin) - 1U) || strcmp(request.method, "GET") || strcmp(request.target, "/path?encoded=%ZZ"))
    return false;
  memset(&request, 0, sizeof(request));
  memcpy(request.storage, valid_double_slash, sizeof(valid_double_slash));
  if (!proxy_parse_request(&request, sizeof(valid_double_slash) - 1U) || strcmp(request.target, "//path?encoded=%ZZ")) return false;
  memset(&request, 0, sizeof(request));
  memcpy(request.storage, valid_tchar, sizeof(valid_tchar));
  if (!proxy_parse_request(&request, sizeof(valid_tchar) - 1U) || strcmp(request.method, "!#$%&'*+-.^_`|~")) return false;
  for (index = 0U; index < sizeof(invalid_cases) / sizeof(invalid_cases[0]); ++index) {
    size_t length = strlen(invalid_cases[index]);
    memset(&request, 0, sizeof(request));
    memcpy(request.storage, invalid_cases[index], length + 1U);
    if (proxy_parse_request(&request, length)) return false;
  }
  return true;
}

static bool write_yaml_fragment_fixture(char directory[], char root[], char fragment[]) {
  static const char root_contents[] =
      ""
      "runtime:\n"
      "  listen: 127.0.0.1:8080\n"
      "  origin: http://127.0.0.1:8000\n"
      "  cache: /tmp/cache\n"
      "  worker_queue: /tmp/jobs\n";
  static const char fragment_contents[] =
      ""
      "runtime:\n"
      "  forwarded_headers: both\n"
      "  trusted_proxy:\n"
      "    - 127.0.0.0/8\n";
  char fragments[LAGHU_RUNTIME_PATH_SIZE];
  FILE *file;
  if (mkdtemp(directory) == NULL) return false;
  if (!proxy_test_path_join(root, LAGHU_RUNTIME_PATH_SIZE, directory, "/laghu.yaml") ||
      !proxy_test_path_join(fragments, sizeof(fragments), directory, "/conf.d") ||
      !proxy_test_path_join(fragment, LAGHU_RUNTIME_PATH_SIZE, fragments, "/10-forwarding.yaml"))
    return false;
  if (mkdir(fragments, 0700) != 0) return false;
  file = fopen(root, "wb");
  if (file == NULL || fputs(root_contents, file) < 0 || fclose(file) != 0 || chmod(root, 0600) != 0) return false;
  file = fopen(fragment, "wb");
  return file != NULL && fputs(fragment_contents, file) >= 0 && fclose(file) == 0 && chmod(fragment, 0600) == 0;
}

static bool static_response_test(void) {
  char directory[] = "/tmp/laghu-static-XXXXXX";
  char path[LAGHU_RUNTIME_PATH_SIZE];
  int sockets[2];
  int file;
  char listed[LAGHU_RUNTIME_PATH_SIZE];
  proxy_request request = {0};
  proxy_access_log access = {0};
  laghu_proxy_options options;
  char output[1024];
  ssize_t received;
  if (mkdtemp(directory) == NULL) return false;
  (void)snprintf(path, sizeof(path), "%s/index.html", directory);
  file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (file < 0 || write(file, "hello", 5U) != 5 || close(file) != 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return false;
  laghu_proxy_options_init(&options);
  if (!proxy_options_append_site(&options, NULL)) return false;
  (void)snprintf(options.sites[0].host, sizeof(options.sites[0].host), "%s", "static.example.test");
  (void)snprintf(options.sites[0].document_root, sizeof(options.sites[0].document_root), "%s", directory);
  (void)snprintf(options.sites[0].index_file, sizeof(options.sites[0].index_file), "%s", "index.html");
  options.site_count = 1U;
  (void)snprintf(options.response_headers[0].name, sizeof(options.response_headers[0].name), "%s", "X-Static-Policy");
  (void)snprintf(options.response_headers[0].value, sizeof(options.response_headers[0].value), "%s", "enabled");
  options.response_header_count = 1U;
  (void)snprintf(request.method, sizeof(request.method), "%s", "GET");
  (void)snprintf(request.target, sizeof(request.target), "%s", "/");
  request.headers[request.header_count++] = (proxy_header){"Host", "STATIC.example.test:8080"};
  if (!proxy_static_serve(&options, &request, sockets[0], NULL, &access)) return false;
  received = recv(sockets[1], output, sizeof(output) - 1U, 0);
  close(sockets[0]);
  close(sockets[1]);
  if (received <= 0) return false;
  output[received] = '\0';
  if (access.status != 200U || strstr(output, "Content-Type: text/html") == NULL || strstr(output, "X-Static-Policy: enabled") == NULL ||
      strstr(output, "\r\n\r\nhello") == NULL)
    return false;
  (void)snprintf(listed, sizeof(listed), "%s/listed", directory);
  if (mkdir(listed, 0700) != 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return false;
  options.directory_listing = true;
  memset(&request, 0, sizeof(request));
  (void)snprintf(request.method, sizeof(request.method), "%s", "GET");
  (void)snprintf(request.target, sizeof(request.target), "%s", "/listed/");
  request.headers[request.header_count++] = (proxy_header){"Host", "static.example.test"};
  if (!proxy_static_serve(&options, &request, sockets[0], NULL, &access)) return false;
  received = recv(sockets[1], output, sizeof(output) - 1U, 0);
  close(sockets[0]);
  close(sockets[1]);
  rmdir(listed);
  unlink(path);
  rmdir(directory);
  if (received <= 0) return false;
  output[received] = '\0';
  {
    bool result = access.status == 200U && strstr(output, "Content-Type: text/plain") != NULL;
    laghu_proxy_options_dispose(&options);
    return result;
  }
}

static bool static_path_truncation_test(void) {
  char directory[] = "/tmp/laghu-static-long-XXXXXX";
  char target[LAGHU_RUNTIME_PATH_SIZE];
  char segments[10U][103U];
  int parents[10U];
  int current = -1;
  int sockets[2];
  int file = -1;
  size_t target_length = 1U;
  size_t index;
  bool result = false;
  bool options_ready = false;
  proxy_request request = {0};
  proxy_access_log access = {0};
  laghu_proxy_options options;
  char output[4096];
  ssize_t received;
  memset(parents, -1, sizeof(parents));
  if (mkdtemp(directory) == NULL) return false;
  current = open(directory, O_RDONLY | O_DIRECTORY);
  if (current < 0) goto done;
  target[0] = '/';
  for (index = 0U; index < 10U; ++index) {
    size_t segment_length = index == 9U ? 102U : 100U;
    memset(segments[index], 'a' + (int)index, segment_length);
    segments[index][segment_length] = '\0';
    memcpy(target + target_length, segments[index], segment_length);
    target_length += segment_length;
    target[target_length++] = '/';
    parents[index] = current;
    if (mkdirat(current, segments[index], 0700) != 0 || (current = openat(current, segments[index], O_RDONLY | O_DIRECTORY | O_NOFOLLOW)) < 0)
      goto done;
  }
  target[target_length] = '\0';
  if (target_length != 1013U) goto done;
  file = openat(current, "iiiiiiiiiii", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (file < 0 || write(file, "truncated-prefix", 16U) != 16 || close(file) != 0) goto done;
  file = -1;
  laghu_proxy_options_init(&options);
  options_ready = true;
  if (!proxy_options_append_site(&options, NULL)) goto done;
  (void)snprintf(options.sites[0].host, sizeof(options.sites[0].host), "%s", "static-long.example.test");
  (void)snprintf(options.sites[0].document_root, sizeof(options.sites[0].document_root), "%s", directory);
  memset(options.sites[0].index_file, 'i', sizeof(options.sites[0].index_file) - 1U);
  options.site_count = 1U;
  (void)snprintf(request.method, sizeof(request.method), "%s", "GET");
  memcpy(request.target, target, target_length + 1U);
  request.headers[request.header_count++] = (proxy_header){"Host", "static-long.example.test"};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 || !proxy_static_serve(&options, &request, sockets[0], NULL, &access)) goto done;
  received = recv(sockets[1], output, sizeof(output) - 1U, 0);
  close(sockets[0]);
  close(sockets[1]);
  if (received <= 0) goto done;
  output[received] = '\0';
  if (access.status != 414U || strstr(output, "414 URI Too Long") == NULL || strstr(output, "truncated-prefix") != NULL) goto done;
  memset(&request, 0, sizeof(request));
  memset(&access, 0, sizeof(access));
  (void)snprintf(options.rules.spa_fallback, sizeof(options.rules.spa_fallback), "%s", target);
  (void)snprintf(request.method, sizeof(request.method), "%s", "GET");
  (void)snprintf(request.target, sizeof(request.target), "%s", "/missing");
  request.headers[request.header_count++] = (proxy_header){"Host", "static-long.example.test"};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 || !proxy_static_serve(&options, &request, sockets[0], NULL, &access)) goto done;
  received = recv(sockets[1], output, sizeof(output) - 1U, 0);
  close(sockets[0]);
  close(sockets[1]);
  if (received <= 0) goto done;
  output[received] = '\0';
  result = access.status == 414U && strstr(output, "414 URI Too Long") != NULL && strstr(output, "truncated-prefix") == NULL;
done:
  if (options_ready) laghu_proxy_options_dispose(&options);
  if (file >= 0) (void)close(file);
  if (current >= 0) {
    (void)unlinkat(current, "iiiiiiiiiii", 0);
    (void)close(current);
  }
  for (index = 10U; index > 0U; --index) {
    if (parents[index - 1U] >= 0) {
      (void)unlinkat(parents[index - 1U], segments[index - 1U], AT_REMOVEDIR);
      (void)close(parents[index - 1U]);
    }
  }
  (void)rmdir(directory);
  return result;
}

static bool static_preopened_root_test(void) {
  char directory[] = "/tmp/laghu-static-root-XXXXXX";
  char root[LAGHU_RUNTIME_PATH_SIZE], moved[LAGHU_RUNTIME_PATH_SIZE], replacement[LAGHU_RUNTIME_PATH_SIZE];
  char outside[LAGHU_RUNTIME_PATH_SIZE], link[LAGHU_RUNTIME_PATH_SIZE], missing[LAGHU_RUNTIME_PATH_SIZE], path[LAGHU_RUNTIME_PATH_SIZE];
  int sockets[2] = {-1, -1};
  int file = -1;
  bool result = false;
  laghu_proxy_options options;
  proxy_static_roots *roots = NULL;
  proxy_queue queue = {0};
  proxy_worker worker = {0};
  proxy_connection connection = {0};
  proxy_request request = {0};
  proxy_access_log access = {0};
  char output[1024];
  ssize_t received;
  if (mkdtemp(directory) == NULL || !proxy_test_path_join(root, sizeof(root), directory, "/root") ||
      !proxy_test_path_join(moved, sizeof(moved), directory, "/moved") ||
      !proxy_test_path_join(replacement, sizeof(replacement), directory, "/root") ||
      !proxy_test_path_join(outside, sizeof(outside), directory, "/outside") || !proxy_test_path_join(link, sizeof(link), directory, "/root-link") ||
      !proxy_test_path_join(missing, sizeof(missing), directory, "/missing"))
    return false;
  if (mkdir(root, 0700) != 0) goto done;
  (void)snprintf(path, sizeof(path), "%s/index.html", root);
  file = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (file < 0 || write(file, "pinned root", 11U) != 11 || close(file) != 0) goto done;
  file = -1;
  (void)snprintf(path, sizeof(path), "%s/linked", root);
  if (symlink(outside, path) != 0) goto done;
  file = open(outside, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (file < 0 || write(file, "outside", 7U) != 7 || close(file) != 0) goto done;
  file = -1;
  laghu_proxy_options_init(&options);
  options.config.mode = LAGHU_MODE_OFF;
  (void)snprintf(options.document_root, sizeof(options.document_root), "%s", root);
  (void)snprintf(options.index_file, sizeof(options.index_file), "%s", "index.html");
  roots = proxy_static_roots_create(&options);
  if (roots == NULL || roots->global_root < 0) goto done_options;
  queue.static_roots = roots;
  worker.queue = &queue;
  if (rename(root, moved) != 0 || mkdir(replacement, 0700) != 0) goto done_options;
  (void)snprintf(path, sizeof(path), "%s/index.html", replacement);
  file = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (file < 0 || write(file, "replacement", 11U) != 11 || close(file) != 0) goto done_options;
  file = -1;
  (void)snprintf(request.method, sizeof(request.method), "%s", "GET");
  (void)snprintf(request.target, sizeof(request.target), "%s", "/");
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
      !proxy_static_serve_with_context(&options, &connection, &worker, &request, &options.config, &options.service, &options.rules, sockets[0], NULL,
                                       &access))
    goto done_options;
  received = recv(sockets[1], output, sizeof(output) - 1U, 0);
  close(sockets[0]);
  close(sockets[1]);
  sockets[0] = sockets[1] = -1;
  if (received <= 0) goto done_options;
  output[received] = '\0';
  if (access.status != 200U || strstr(output, "pinned root") == NULL || strstr(output, "replacement") != NULL) goto done_options;
  memset(&request, 0, sizeof(request));
  memset(&access, 0, sizeof(access));
  (void)snprintf(request.method, sizeof(request.method), "%s", "GET");
  (void)snprintf(request.target, sizeof(request.target), "%s", "/linked");
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
      proxy_static_serve_with_context(&options, &connection, &worker, &request, &options.config, &options.service, &options.rules, sockets[0], NULL,
                                      &access))
    goto done_options;
  close(sockets[0]);
  close(sockets[1]);
  sockets[0] = sockets[1] = -1;
  memset(&request, 0, sizeof(request));
  memset(&access, 0, sizeof(access));
  (void)snprintf(options.rules.spa_fallback, sizeof(options.rules.spa_fallback), "%s", "/index.html");
  (void)snprintf(request.method, sizeof(request.method), "%s", "GET");
  (void)snprintf(request.target, sizeof(request.target), "%s", "/missing");
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
      !proxy_static_serve_with_context(&options, &connection, &worker, &request, &options.config, &options.service, &options.rules, sockets[0], NULL,
                                       &access))
    goto done_options;
  received = recv(sockets[1], output, sizeof(output) - 1U, 0);
  close(sockets[0]);
  close(sockets[1]);
  sockets[0] = sockets[1] = -1;
  if (received <= 0) goto done_options;
  output[received] = '\0';
  if (!access.spa_fallback || strstr(output, "pinned root") == NULL) goto done_options;
  proxy_static_roots_dispose(roots);
  roots = NULL;
  queue.static_roots = NULL;
  if (symlink(moved, link) != 0) goto done_options;
  (void)snprintf(options.document_root, sizeof(options.document_root), "%s", link);
  roots = proxy_static_roots_create(&options);
  if (roots == NULL || roots->global_root >= 0) goto done_options;
  queue.static_roots = roots;
  memset(&request, 0, sizeof(request));
  (void)snprintf(request.method, sizeof(request.method), "%s", "GET");
  (void)snprintf(request.target, sizeof(request.target), "%s", "/");
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
      proxy_static_serve_with_context(&options, &connection, &worker, &request, &options.config, &options.service, &options.rules, sockets[0], NULL,
                                      &access))
    goto done_options;
  close(sockets[0]);
  close(sockets[1]);
  sockets[0] = sockets[1] = -1;
  (void)snprintf(options.document_root, sizeof(options.document_root), "%s", missing);
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
      proxy_static_serve_with_context(&options, &connection, &worker, &request, &options.config, &options.service, &options.rules, sockets[0], NULL,
                                      &access))
    goto done_options;
  close(sockets[0]);
  close(sockets[1]);
  sockets[0] = sockets[1] = -1;
  result = true;
done_options:
  if (roots != NULL) proxy_static_roots_dispose(roots);
  laghu_proxy_options_dispose(&options);
done:
  if (file >= 0) (void)close(file);
  if (sockets[0] >= 0) (void)close(sockets[0]);
  if (sockets[1] >= 0) (void)close(sockets[1]);
  (void)snprintf(path, sizeof(path), "%s/index.html", replacement);
  (void)unlink(path);
  (void)rmdir(replacement);
  (void)snprintf(path, sizeof(path), "%s/index.html", moved);
  (void)unlink(path);
  (void)snprintf(path, sizeof(path), "%s/linked", moved);
  (void)unlink(path);
  (void)rmdir(moved);
  (void)unlink(link);
  (void)unlink(outside);
  (void)rmdir(directory);
  return result;
}

static bool route_rewrite_test(void) {
  laghu_proxy_options options;
  proxy_request request = {0};
  bool result;
  laghu_proxy_options_init(&options);
  if (!proxy_options_append_route(&options, NULL)) return false;
  options.routes[0].match = LAGHU_PROXY_ROUTE_PREFIX;
  (void)snprintf(options.routes[0].pattern, sizeof(options.routes[0].pattern), "%s", "/legacy/");
  (void)snprintf(options.routes[0].rewrite, sizeof(options.routes[0].rewrite), "%s", "/index.html");
  (void)snprintf(request.target, sizeof(request.target), "%s", "/legacy/page?keep=ignored");
  result = proxy_route_rewrite(&options, &request) && !strcmp(request.target, "/index.html");
  laghu_proxy_options_dispose(&options);
  return result;
}

static bool response_header_batch_test(void) {
  proxy_response response = {0};
  laghu_http_transaction_result result = {0};
  char output[1024U];
  ssize_t received;
  int sockets[2] = {-1, -1};
  bool passed = false;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return false;
  response.status = 200U;
  (void)snprintf(response.reason, sizeof(response.reason), "%s", "OK");
  response.headers[response.header_count++] = (proxy_header){"Content-Type", "text/plain"};
  response.headers[response.header_count++] = (proxy_header){"Connection", "keep-alive"};
  result.header_operations[result.header_operation_count].kind = LAGHU_HTTP_HEADER_APPEND;
  (void)snprintf(result.header_operations[result.header_operation_count].name, sizeof(result.header_operations[result.header_operation_count].name),
                 "%s", "X-Result");
  result.header_operations[result.header_operation_count++].value = "present";
  if (!proxy_send_headers(sockets[0], NULL, &response, &result, 3U, true)) goto done;
  received = recv(sockets[1], output, sizeof(output) - 1U, 0);
  if (received <= 0) goto done;
  output[received] = '\0';
  passed = !strcmp(output, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nX-Result: present\r\nContent-Length: 3\r\nConnection: close\r\n\r\n");
done:
  if (sockets[0] >= 0) close(sockets[0]);
  if (sockets[1] >= 0) close(sockets[1]);
  return passed;
}

static bool scoped_rules_test(void) {
  static const char auth_contents[] =
      "operator:scrypt-v1:16384:8:1:00112233445566778899aabbccddeeff:f5206d570fcd120bd1f23a8cd186bd87c04ac1db00e9ac1efca589774ae6ecb8\n";
  char auth_path[] = "/tmp/laghu-basic-auth-XXXXXX";
  char auth_link[] = "/tmp/laghu-basic-auth-link-XXXXXX";
  char error[128U];
  laghu_proxy_rules global;
  laghu_proxy_rules site;
  laghu_proxy_rules route;
  laghu_proxy_rules merged;
  int file = mkstemp(auth_path);
  bool result;
  if (file < 0 || write(file, auth_contents, sizeof(auth_contents) - 1U) != (ssize_t)(sizeof(auth_contents) - 1U) || close(file) != 0) return false;
  laghu_proxy_rules_init(&global);
  laghu_proxy_rules_init(&site);
  laghu_proxy_rules_init(&route);
  result = laghu_proxy_rules_apply(&global, "compression", "gzip", error, sizeof(error)) &&
           laghu_proxy_rules_apply(&global, "rate_limit", "10", error, sizeof(error)) &&
           laghu_proxy_rules_apply(&global, "rate_burst", "8", error, sizeof(error)) &&
           laghu_proxy_rules_apply(&global, "allow", "127.0.0.1/32", error, sizeof(error)) &&
           laghu_proxy_rules_apply(&site, "basic_auth_file", auth_path, error, sizeof(error)) &&
           laghu_proxy_rules_apply(&site, "basic_auth_realm", "Private", error, sizeof(error)) &&
           laghu_proxy_rules_apply(&route, "rate_limit", "2", error, sizeof(error)) &&
           laghu_proxy_rules_merge(&merged, &global, &site, error, sizeof(error)) &&
           laghu_proxy_rules_merge(&merged, &merged, &route, error, sizeof(error));
  if (result)
    result = merged.compression == LAGHU_PROXY_COMPRESSION_GZIP && merged.rate_per_second == 2U && merged.rate_burst == 8U &&
             merged.allow_count == 1U && merged.basic_auth_user_count == 1U &&
             merged.basic_auth_users[0].password_kdf == LAGHU_PROXY_BASIC_AUTH_SCRYPT_V1 && !strcmp(merged.basic_auth_realm, "Private") &&
             !laghu_proxy_rules_apply(&route, "request_header_limit", "1023", error, sizeof(error));
  if (chmod(auth_path, 0644) != 0) result = false;
  laghu_proxy_rules_init(&route);
  if (result) result = !laghu_proxy_rules_apply(&route, "basic_auth_file", auth_path, error, sizeof(error));
  {
    int link_file = mkstemp(auth_link);
    if (link_file < 0 || close(link_file) != 0 || unlink(auth_link) != 0 || symlink(auth_path, auth_link) != 0)
      result = false;
    else {
      laghu_proxy_rules_init(&route);
      if (result) result = !laghu_proxy_rules_apply(&route, "basic_auth_file", auth_link, error, sizeof(error));
    }
  }
  (void)unlink(auth_link);
  (void)unlink(auth_path);
  return result;
}

static bool auth_fixture_write(const char *path, const char *contents) {
  int file;
  size_t length;
  ssize_t written;
  if (path == NULL || contents == NULL) return false;
  length = strlen(contents);
  file = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
  if (file < 0) return false;
  written = write(file, contents, length);
  return close(file) == 0 && written == (ssize_t)length;
}

static bool token_fixture_write(const char *path, const void *contents, size_t length) {
  int file;
  ssize_t written;
  if (path == NULL || (contents == NULL && length != 0U)) return false;
  file = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (file < 0) return false;
  written = write(file, contents, length);
  return close(file) == 0 && written == (ssize_t)length;
}

typedef struct {
  const char *moved;
  bool completed;
} token_swap_hook;

static void token_swap_before_open(const char *path, void *context) {
  static const char replacement[] = "replacement-purge-token-0123456789\n";
  token_swap_hook *swap = context;
  if (swap == NULL || rename(path, swap->moved) != 0) return;
  swap->completed = token_fixture_write(path, replacement, sizeof(replacement) - 1U);
}

static void token_delete_after_open(const char *path, void *context) {
  bool *deleted = context;
  if (deleted != NULL) *deleted = unlink(path) == 0;
}

static bool admin_token_file_test(void) {
  static const char valid[] = "standalone-purge-token-0123456789\n";
  static const char wrong[] = "standalone-purge-token-wrong\n";
  char directory[] = "/tmp/laghu-purge-token-XXXXXX";
  char token[LAGHU_RUNTIME_PATH_SIZE];
  char moved[LAGHU_RUNTIME_PATH_SIZE];
  unsigned char output[257U];
  unsigned char oversized[257U];
  laghu_service_config service;
  proxy_request request = {0};
  token_swap_hook swap;
  bool deleted = false;
  size_t length = 0U;
  bool result = false;
  if (mkdtemp(directory) == NULL || !proxy_test_path_join(token, sizeof(token), directory, "/token") ||
      !proxy_test_path_join(moved, sizeof(moved), directory, "/moved"))
    return false;
  laghu_service_config_init(&service);
  if (!token_fixture_write(token, valid, sizeof(valid) - 1U) ||
      snprintf(service.purge_token_file, sizeof(service.purge_token_file), "%s", token) < 0 || strlen(token) >= sizeof(service.purge_token_file))
    goto done;
  request.headers[0U] = (proxy_header){"X-Laghu-Purge-Token", (char *)"standalone-purge-token-0123456789"};
  request.header_count = 1U;
  if (!proxy_admin_token(&service, &request) || !proxy_admin_token_file_read(token, output, &length, NULL, NULL, NULL) ||
      length != sizeof(valid) - 1U || memcmp(output, valid, length) != 0)
    goto done;
  request.headers[0U].value = (char *)wrong;
  if (proxy_admin_token(&service, &request)) goto done;
  if (chmod(token, 0644) != 0 || proxy_admin_token_file_read(token, output, &length, NULL, NULL, NULL) || chmod(token, 0600) != 0) goto done;
  if (unlink(token) != 0 || !token_fixture_write(moved, valid, sizeof(valid) - 1U) || symlink(moved, token) != 0 ||
      proxy_admin_token_file_read(token, output, &length, NULL, NULL, NULL) || unlink(token) != 0 || unlink(moved) != 0)
    goto done;
  if (mkdir(token, 0700) != 0 || proxy_admin_token_file_read(token, output, &length, NULL, NULL, NULL) || rmdir(token) != 0) goto done;
  if (mkfifo(token, 0600) != 0 || proxy_admin_token_file_read(token, output, &length, NULL, NULL, NULL) || unlink(token) != 0) goto done;
  memset(oversized, 'x', sizeof(oversized));
  if (!token_fixture_write(token, oversized, sizeof(oversized)) || proxy_admin_token_file_read(token, output, &length, NULL, NULL, NULL)) goto done;
  if (!token_fixture_write(token, "", 0U) || proxy_admin_token_file_read(token, output, &length, NULL, NULL, NULL)) goto done;
  if (!token_fixture_write(token, valid, sizeof(valid) - 1U)) goto done;
  swap = (token_swap_hook){moved, false};
  if (proxy_admin_token_file_read(token, output, &length, token_swap_before_open, NULL, &swap) || !swap.completed) goto done;
  if (!token_fixture_write(token, valid, sizeof(valid) - 1U) ||
      !proxy_admin_token_file_read(token, output, &length, NULL, token_delete_after_open, &deleted) || !deleted || access(token, F_OK) == 0 ||
      length != sizeof(valid) - 1U || memcmp(output, valid, length) != 0)
    goto done;
  result = true;
done:
  laghu_service_config_dispose(&service);
  (void)unlink(token);
  (void)unlink(moved);
  (void)rmdir(directory);
  return result;
}

static bool basic_auth_format_test(void) {
  static const char scrypt[] =
      "operator:scrypt-v1:16384:8:1:00112233445566778899aabbccddeeff:f5206d570fcd120bd1f23a8cd186bd87c04ac1db00e9ac1efca589774ae6ecb8\n";
  static const char legacy[] = "operator:sha256:4104d36f8da2c254349f85836793ebe029e0c957063a34c91c2e9203187b5631\n";
  static const char invalid[][256U] = {
      "operator:scrypt-v1:16385:8:1:00112233445566778899aabbccddeeff:f5206d570fcd120bd1f23a8cd186bd87c04ac1db00e9ac1efca589774ae6ecb8\n",
      "operator:scrypt-v1:18446744073709551615:8:1:00112233445566778899aabbccddeeff:"
      "f5206d570fcd120bd1f23a8cd186bd87c04ac1db00e9ac1efca589774ae6ecb8\n",
      "operator:scrypt-v1:16384:9:1:00112233445566778899aabbccddeeff:f5206d570fcd120bd1f23a8cd186bd87c04ac1db00e9ac1efca589774ae6ecb8\n",
      "operator:scrypt-v1:16384:8:2:00112233445566778899aabbccddeeff:f5206d570fcd120bd1f23a8cd186bd87c04ac1db00e9ac1efca589774ae6ecb8\n",
      "operator:scrypt-v1:16384:8:1:00112233445566778899aabbccddeefg:f5206d570fcd120bd1f23a8cd186bd87c04ac1db00e9ac1efca589774ae6ecb8\n",
      "operator:scrypt-v1:16384:8:1:00112233445566778899aabbccddeeff:f5206d570fcd120bd1f23a8cd186bd87c04ac1db00e9ac1efca589774ae6ecb\n",
      "operator:sha256:not-a-hex-digest\n",
      "operator:scrypt-v1:16384:8:1:00112233445566778899aabbccddeeff:f5206d570fcd120bd1f23a8cd186bd87c04ac1db00e9ac1efca589774ae6ecb8:extra\n",
      "operator:scrypt-v1:16384:8:1:00112233445566778899aabbccddeeff:f5206d570fcd120bd1f23a8cd186bd87c04ac1db00e9ac1efca589774ae6ecb8\n"
      "operator:sha256:4104d36f8da2c254349f85836793ebe029e0c957063a34c91c2e9203187b5631\n",
  };
  static const unsigned char expected_salt[] = {0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
                                                0x88U, 0x99U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU, 0xffU};
  static const unsigned char expected_hash[] = {0xf5U, 0x20U, 0x6dU, 0x57U, 0x0fU, 0xcdU, 0x12U, 0x0bU, 0xd1U, 0xf2U, 0x3aU,
                                                0x8cU, 0xd1U, 0x86U, 0xbdU, 0x87U, 0xc0U, 0x4aU, 0xc1U, 0xdbU, 0x00U, 0xe9U,
                                                0xacU, 0x1eU, 0xfcU, 0xa5U, 0x89U, 0x77U, 0x4aU, 0xe6U, 0xecU, 0xb8U};
  char auth_path[] = "/tmp/laghu-basic-auth-format-XXXXXX";
  char error[128U];
  laghu_proxy_rules rules;
  size_t index;
  int file = mkstemp(auth_path);
  bool result;
  if (file < 0 || close(file) != 0 || chmod(auth_path, 0600) != 0 || !auth_fixture_write(auth_path, scrypt)) return false;
  laghu_proxy_rules_init(&rules);
  result = laghu_proxy_rules_apply(&rules, "basic_auth_file", auth_path, error, sizeof(error)) && rules.basic_auth_user_count == 1U &&
           rules.basic_auth_users[0].password_kdf == LAGHU_PROXY_BASIC_AUTH_SCRYPT_V1 &&
           memcmp(rules.basic_auth_users[0].password_salt, expected_salt, sizeof(expected_salt)) == 0 &&
           memcmp(rules.basic_auth_users[0].password_hash, expected_hash, sizeof(expected_hash)) == 0;
  if (result && !auth_fixture_write(auth_path, legacy)) result = false;
  laghu_proxy_rules_init(&rules);
  if (result)
    result = laghu_proxy_rules_apply(&rules, "basic_auth_file", auth_path, error, sizeof(error)) && rules.basic_auth_user_count == 1U &&
             rules.basic_auth_users[0].password_kdf == LAGHU_PROXY_BASIC_AUTH_SHA256;
  for (index = 0U; result && index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
    laghu_proxy_rules_init(&rules);
    result = auth_fixture_write(auth_path, invalid[index]) && !laghu_proxy_rules_apply(&rules, "basic_auth_file", auth_path, error, sizeof(error));
  }
  (void)unlink(auth_path);
  return result;
}

int main(void) {
  laghu_proxy_options *options_storage;
#define options (*options_storage)
  proxy_queue queue;
  proxy_worker worker;
  laghu_service_config expected_service;
  char error[128];
  char yaml_path[] = "/tmp/laghu-yaml-XXXXXX";
  char yaml_directory[] = "/tmp/laghu-yaml-dir-XXXXXX";
  char yaml_root[LAGHU_RUNTIME_PATH_SIZE];
  char yaml_fragment[LAGHU_RUNTIME_PATH_SIZE];
  char *valid[] = {"laghu",
                   "--listen",
                   "127.0.0.1:8080",
                   "--origin",
                   "http://127.0.0.1:8000",
                   "--cache",
                   "/tmp/cache",
                   "--worker-queue",
                   "/tmp/jobs",
                   "--allow-api",
                   "--critical-css-beacon",
                   "--instrumentation-beacon",
                   "--instrumentation-sample-rate",
                   "50",
                   "--optimization-profiles",
                   "on",
                   "--javascript-inline-limit",
                   "4096",
                   "--javascript-outline-threshold",
                   "16384",
                   "--drain-timeout",
                   "45",
                   "--request-header-timeout",
                   "7",
                   "--request-body-timeout",
                   "19"};
  char *backend[] = {"laghu",
                     "--listen",
                     "127.0.0.1:8080",
                     "--origin",
                     "http://127.0.0.1:8000",
                     "--file-cache-backend",
                     "file:///tmp/cache",
                     "--file-cache-size",
                     "20m",
                     "--file-cache-inode-limit",
                     "2000",
                     "--file-cache-clean-interval",
                     "2m",
                     "--file-cache-metadata-size",
                     "1m",
                     "--worker-queue",
                     "/tmp/jobs"};
  char *static_only[] = {"laghu",   "--listen",   "127.0.0.1:8080", "--document-root", "/srv/static",
                         "--cache", "/tmp/cache", "--worker-queue", "/tmp/jobs"};
  char *bare_passthrough[] = {"laghu", "--listen", "127.0.0.1:8080", "--origin", "http://127.0.0.1:8000", "--rewrite-level", "passthrough"};
  char *bare_core[] = {"laghu", "--listen", "127.0.0.1:8080", "--origin", "http://127.0.0.1:8000", "--rewrite-level", "core"};
  char *passthrough_beacon[] = {"laghu",           "--listen",    "127.0.0.1:8080",       "--origin", "http://127.0.0.1:8000",
                                "--rewrite-level", "passthrough", "--critical-css-beacon"};
  char *passthrough_beacon_cache[] = {"laghu",           "--listen",    "127.0.0.1:8080",        "--origin", "http://127.0.0.1:8000",
                                      "--rewrite-level", "passthrough", "--critical-css-beacon", "--cache",  "/tmp/cache"};
  char *backend_conflict[] = {"laghu",    "--listen",   "127.0.0.1:8080",       "--origin",          "http://127.0.0.1:8000",
                              "--cache",  "/tmp/cache", "--file-cache-backend", "file:///tmp/cache", "--worker-queue",
                              "/tmp/jobs"};
  char *budgets[] = {"laghu",
                     "--listen",
                     "127.0.0.1:8080",
                     "--origin",
                     "http://127.0.0.1:8000",
                     "--cache",
                     "/tmp/cache",
                     "--worker-queue",
                     "/tmp/jobs",
                     "--transform-memory-limit",
                     "64m",
                     "--transform-deadline-ms",
                     "125",
                     "--variants-per-source",
                     "8"};
  char *conflict[] = {"laghu",          "--listen",  "127.0.0.1:8080", "--origin", "http://127.0.0.1:8000", "--cache", "/tmp/cache",
                      "--worker-queue", "/tmp/jobs", "--preset",       "safe",     "--rewrite-level",       "core"};
  char *bad_drain[] = {"laghu",          "--listen",  "127.0.0.1:8080",  "--origin", "http://127.0.0.1:8000", "--cache", "/tmp/cache",
                       "--worker-queue", "/tmp/jobs", "--drain-timeout", "0"};
  char *bad_header_timeout[] = {"laghu", "--listen", "127.0.0.1:8080", "--origin", "http://127.0.0.1:8000", "--request-header-timeout", "0"};
  char *pool[] = {"laghu",          "--listen",  "127.0.0.1:8080",     "--origin", "http://127.0.0.1:8000", "--cache", "/tmp/cache",
                  "--worker-queue", "/tmp/jobs", "--origin-pool-size", "1",        "--origin-idle-timeout", "9"};
  char *filters[] = {"laghu",           "--listen",         "127.0.0.1:8080", "--origin",        "http://127.0.0.1:8000",
                     "--cache",         "/tmp/cache",       "--worker-queue", "/tmp/jobs",       "--enable-filter",
                     "resource_inline", "--disable-filter", "html_minify",    "--forbid-filter", "javascript_defer"};
  char *filter_conflict[] = {"laghu",          "--listen",  "127.0.0.1:8080",   "--origin",    "http://127.0.0.1:8000", "--cache",    "/tmp/cache",
                             "--worker-queue", "/tmp/jobs", "--disable-filter", "html_minify", "--enable-filter",       "html_minify"};
  char *request_policy[] = {"laghu",
                            "--listen",
                            "127.0.0.1:8080",
                            "--origin",
                            "http://127.0.0.1:8000",
                            "--cache",
                            "/tmp/cache",
                            "--worker-queue",
                            "/tmp/jobs",
                            "--allow-resources",
                            "/assets/*",
                            "--disallow",
                            "/assets/private/*",
                            "--respect-vary",
                            "off",
                            "--respect-x-forwarded-proto",
                            "on",
                            "--query-filter-overrides",
                            "on",
                            "--trusted-proxy",
                            "127.0.0.0/8"};
  char *domain_policy[] = {"laghu",
                           "--listen",
                           "127.0.0.1:8080",
                           "--origin",
                           "http://127.0.0.1:8000",
                           "--cache",
                           "/tmp/cache",
                           "--worker-queue",
                           "/tmp/jobs",
                           "--map-proxy-domain",
                           "https://proxy.example",
                           "https://origin.example",
                           "--shard-domain",
                           "https://proxy.example",
                           "https://one.example,https://two.example"};
  char *secure[] = {"laghu",      "--listen",        "127.0.0.1:8080", "--origin",         "https://example.test", "--cache",
                    "/tmp/cache", "--worker-queue",  "/tmp/jobs",      "--origin-ca-file", "/tmp/ca.pem",          "--forwarded-headers",
                    "both",       "--trusted-proxy", "127.0.0.0/8",    "--trusted-proxy",  "2001:db8::/32"};
  char *downstream_tls[] = {"laghu",           "--listen",          "127.0.0.1:8080", "--origin",  "http://127.0.0.1:8000",
                            "--cache",         "/tmp/cache",        "--worker-queue", "/tmp/jobs", "--tls-certificate",
                            "/tmp/server.pem", "--tls-private-key", "/tmp/server.key"};
  char *incomplete_downstream_tls[] = {"laghu",      "--listen",       "127.0.0.1:8080", "--origin",          "http://127.0.0.1:8000", "--cache",
                                       "/tmp/cache", "--worker-queue", "/tmp/jobs",      "--tls-certificate", "/tmp/server.pem"};
  char *bad_cidr[] = {"laghu",          "--listen",  "127.0.0.1:8080",  "--origin",   "http://127.0.0.1:8000", "--cache", "/tmp/cache",
                      "--worker-queue", "/tmp/jobs", "--trusted-proxy", "127.0.0.1/8"};
  char *bad_rum[] = {"laghu",      "--listen",       "127.0.0.1:8080", "--origin",    "http://127.0.0.1:8000",      "--cache",
                     "/tmp/cache", "--worker-queue", "/tmp/jobs",      "--rum-store", "redis://example.test:6379/0"};
  char *access_log_off[] = {"laghu",          "--listen",  "127.0.0.1:8080", "--origin", "http://127.0.0.1:8000", "--cache", "/tmp/cache",
                            "--worker-queue", "/tmp/jobs", "--access-log",   "off"};
  char *invalid_access_log[] = {"laghu",          "--listen",  "127.0.0.1:8080", "--origin", "http://127.0.0.1:8000", "--cache", "/tmp/cache",
                                "--worker-queue", "/tmp/jobs", "--access-log",   "disabled"};
  char *native_file[] = {"laghu",          "--listen",  "127.0.0.1:8080",   "--origin", "http://127.0.0.1:8000", "--cache", "/tmp/cache",
                         "--worker-queue", "/tmp/jobs", "--load-from-file", "native"};
  char *unbound_file[] = {"laghu",
                          "--listen",
                          "127.0.0.1:8080",
                          "--origin",
                          "http://127.0.0.1:8000",
                          "--cache",
                          "/tmp/cache",
                          "--worker-queue",
                          "/tmp/jobs",
                          "--load-from-file",
                          "mapped",
                          "--file-source-map",
                          "https://origin.example.test/assets/=/tmp/assets"};
  char *admin[] = {"laghu",
                   "--listen",
                   "127.0.0.1:8080",
                   "--origin",
                   "http://127.0.0.1:8000",
                   "--cache",
                   "/tmp/cache",
                   "--worker-queue",
                   "/tmp/jobs",
                   "--purge-method",
                   "PURGE",
                   "--purge-query",
                   "on",
                   "--purge-token-file",
                   "/etc/laghu/purge.token",
                   "--purge-allow",
                   "127.0.0.0/8",
                   "--statistics",
                   "on",
                   "--metrics",
                   "on",
                   "--readiness",
                   "on",
                   "--readiness-policy",
                   "strict"};
  char *rum[] = {"laghu",
                 "--listen",
                 "127.0.0.1:8080",
                 "--origin",
                 "http://127.0.0.1:8000",
                 "--cache",
                 "/tmp/cache",
                 "--worker-queue",
                 "/tmp/jobs",
                 "--rum-store",
                 "local:/tmp/rum",
                 "--rum-store-local-snapshot",
                 "/tmp/rum.snapshot",
                 "--rum-store-timeout",
                 "75",
                 "--rum-store-ttl",
                 "604800",
                 "--rum-store-retry-limit",
                 "2",
                 "--rum-store-sync-interval",
                 "5",
                 "--rum-store-memory-limit",
                 "8m",
                 "--rum-store-pending-limit",
                 "1m",
                 "--rum-store-required"};
  static const unsigned char chunked[] = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
  unsigned char decoded[16];
  unsigned char marker;
  int first[2], second[2];
  size_t decoded_length = 0U;
  options_storage = calloc(1U, sizeof(*options_storage));
  CHECK(options_storage != NULL);
  laghu_proxy_options_init(&options);
  CHECK(write_yaml_fixture(yaml_path));
  CHECK(laghu_proxy_load_yaml(yaml_path, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(!strcmp(options.listen_host, "127.0.0.1") && options.forwarded_mode == LAGHU_PROXY_FORWARDED_BOTH);
  CHECK(!options.access_log);
  CHECK(options.request_header_timeout == 7U && options.request_body_timeout == 19U);
  CHECK(options.service.trusted_proxy_count == 1U);
  CHECK(options.response_header_count == 2U && !strcmp(options.response_headers[0].name, "X-Static-Policy"));
  CHECK(options.site_count == 1U && !strcmp(options.sites[0].host, "static.example.test"));
  CHECK(options.sites[0].config.preset == LAGHU_PRESET_SAFE && !strcmp(options.sites[0].service.javascript_target, "defaults"));
  CHECK(options.route_count == 2U && options.routes[1].match == LAGHU_PROXY_ROUTE_ORDERED_REGEX);
  CHECK(sizeof(options) < 128U * 1024U && options.sites != NULL && options.routes != NULL);
  CHECK(!strcmp(options.routes[1].upstream_host, "127.0.0.1") && !strcmp(options.routes[1].upstream_port, "9000") && options.routes[1].upstream_tls &&
        options.routes[1].failover_count == 1U && options.routes[1].failovers[0].tls && options.routes[1].health_interval == 9U);
  {
    proxy_request request = {0};
    laghu_proxy_options *resolved = calloc(1U, sizeof(*resolved));
    const laghu_config *core;
    const laghu_service_config *service;
    CHECK(resolved != NULL);
    (void)snprintf(request.target, sizeof(request.target), "%s", "/site-policy");
    request.headers[request.header_count++] = (proxy_header){"Host", "static.example.test"};
    proxy_scope_for_request(&options, &request, &core, &service);
    CHECK(core == &options.routes[0].config && service == &options.routes[0].service);
    proxy_options_for_request(&options, &request, resolved);
    CHECK(resolved->config.mode == LAGHU_MODE_OFF && !strcmp(resolved->service.javascript_target, "defaults"));
    free(resolved);
  }
  CHECK(unlink(yaml_path) == 0);
  CHECK(write_yaml_fragment_fixture(yaml_directory, yaml_root, yaml_fragment));
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_load_yaml(yaml_root, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(options.forwarded_mode == LAGHU_PROXY_FORWARDED_BOTH);
  CHECK(options.service.trusted_proxy_count == 1U);
  laghu_proxy_options_dispose(&options);
  CHECK(unlink(yaml_fragment) == 0);
  {
    char fragments[LAGHU_RUNTIME_PATH_SIZE];
    (void)snprintf(fragments, sizeof(fragments), "%s/conf.d", yaml_directory);
    CHECK(unlink(yaml_root) == 0);
    CHECK(rmdir(fragments) == 0);
  }
  CHECK(rmdir(yaml_directory) == 0);
  CHECK(runtime_queue_capabilities_cache_test());
  CHECK(static_response_test());
  CHECK(static_path_truncation_test());
  CHECK(static_preopened_root_test());
  CHECK(route_rewrite_test());
  CHECK(response_header_batch_test());
  CHECK(scoped_rules_test());
  CHECK(basic_auth_format_test());
  CHECK(admin_token_file_test());
  CHECK(gateway_health_rejected_test());
  CHECK(health_interval_requires_check_test());
  CHECK(request_line_parse_test());
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(19, admin, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(options.service.purge_method && options.service.purge_query && options.service.statistics && options.service.purge_allow_count == 1U);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(26, valid, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(!strcmp(options.origin_host, "127.0.0.1"));
  CHECK(!strcmp(options.origin_port, "8000"));
  CHECK(options.config.allow_api == LAGHU_MODE_ON);
  CHECK(options.config.critical_css_beacon == LAGHU_MODE_ON);
  CHECK(options.config.instrumentation_beacon == LAGHU_MODE_ON);
  CHECK(options.config.optimization_profiles == LAGHU_MODE_ON);
  CHECK(options.config.instrumentation_sample_rate == 50U);
  CHECK(options.config.javascript_inline_limit == 4096U);
  CHECK(options.config.javascript_outline_threshold == 16384U);
  CHECK(options.drain_timeout == 45U);
  CHECK(options.request_header_timeout == 7U && options.request_body_timeout == 19U);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(7, bad_header_timeout, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(13, pool, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(options.origin_pool_size == 1U && options.origin_idle_timeout == 9U);
  memset(&queue, 0, sizeof(queue));
#undef options
  queue.options = options_storage;
#define options (*options_storage)
  queue.origins = calloc(options.origin_pool_size, sizeof(*queue.origins));
  CHECK(queue.origins != NULL);
  CHECK(pthread_mutex_init(&queue.lock, NULL) == 0);
  memset(&worker, 0, sizeof(worker));
  worker.queue = &queue;
  worker.active_origin = LAGHU_INVALID_SOCKET;
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, first) == 0);
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, second) == 0);
  {
    proxy_origin_connection first_origin = {.socket = first[0]};
    proxy_origin_connection second_origin = {.socket = second[0]};
    proxy_origin_release(&worker, &options, &first_origin, true);
    proxy_origin_release(&worker, &options, &second_origin, true);
  }
  CHECK(queue.origin_count == 1U);
  CHECK(recv(second[1], &marker, 1U, 0) == 0);
  proxy_origin_pool_close(&queue);
  free(queue.origins);
  close(first[1]);
  close(second[1]);
  CHECK(pthread_mutex_destroy(&queue.lock) == 0);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(17, backend, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(!strcmp(options.service.file_cache_backend, "file:///tmp/cache"));
  CHECK(!strcmp(options.service.image_cache, "/tmp/cache"));
  CHECK(options.service.cache_limits.size_limit == 20U * 1024U * 1024U);
  CHECK(options.service.cache_limits.inode_limit == 2000U);
  CHECK(options.service.cache_limits.clean_interval == 120U);
  CHECK(options.service.cache_limits.metadata_size == 1024U * 1024U);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(15, budgets, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(options.config.transform_memory_limit == 64U * 1024U * 1024U);
  CHECK(options.config.transform_deadline_ms == 125U);
  CHECK(options.config.variants_per_source == 8U);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(11, backend_conflict, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(17, secure, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(options.origin_tls && !strcmp(options.origin_port, "443"));
  CHECK(options.forwarded_mode == LAGHU_PROXY_FORWARDED_BOTH);
  CHECK(options.service.trusted_proxy_count == 2U);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(13, downstream_tls, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(options.downstream_tls && !strcmp(options.tls_certificate, "/tmp/server.pem") && !strcmp(options.tls_private_key, "/tmp/server.key"));
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(11, incomplete_downstream_tls, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_service_config_init(&expected_service);
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_IMAGE_CACHE, "/tmp/cache"));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_WORKER_QUEUE, "/tmp/jobs"));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_TRUSTED_PROXY, "127.0.0.0/8"));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_TRUSTED_PROXY, "2001:db8::/32"));
  CHECK(service_finalize(&expected_service));
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(9, static_only, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(!strcmp(options.document_root, "/srv/static") && options.origin_host[0] == '\0');
  CHECK(options.access_log);
  {
    proxy_lifecycle_requirements requirements = {0};
    CHECK(proxy_options_lifecycle_requirements(&options, &requirements));
    CHECK(requirements.cache && requirements.image_queue && requirements.rum && !requirements.operational);
  }
  laghu_proxy_options_dispose(&options);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(7, bare_passthrough, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(options.service.image_cache[0] == '\0' && options.service.worker_queue[0] == '\0');
  {
    proxy_lifecycle_requirements requirements = {0};
    CHECK(proxy_options_lifecycle_requirements(&options, &requirements));
    CHECK(!requirements.cache && !requirements.image_queue && !requirements.rum && !requirements.operational);
  }
  laghu_proxy_options_dispose(&options);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(7, bare_core, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_dispose(&options);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(8, passthrough_beacon, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_dispose(&options);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(10, passthrough_beacon_cache, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  {
    proxy_lifecycle_requirements requirements = {0};
    CHECK(proxy_options_lifecycle_requirements(&options, &requirements));
    CHECK(requirements.cache && !requirements.image_queue && requirements.rum && !requirements.operational);
  }
  laghu_proxy_options_dispose(&options);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(11, access_log_off, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(!options.access_log);
  laghu_proxy_options_dispose(&options);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(11, invalid_access_log, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_dispose(&options);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(17, secure, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(memcmp(&options.service, &expected_service, sizeof(options.service)) == 0);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(26, rum, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(!strcmp(options.service.rum_store, "local:/tmp/rum"));
  CHECK(!strcmp(options.service.rum_snapshot_path, "/tmp/rum.snapshot"));
  CHECK(options.service.rum_timeout_ms == 75U && options.service.rum_ttl == 604800U);
  CHECK(options.service.rum_retry_limit == 2U && options.service.rum_sync_interval == 5U);
  CHECK(options.service.rum_memory_limit == 8U * 1024U * 1024U);
  CHECK(options.service.rum_pending_limit == 1024U * 1024U && options.service.rum_store_required);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(17, backend, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  laghu_service_config_init(&expected_service);
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND, TEST_BACKEND_URI));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_FILE_CACHE_SIZE, "20m"));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_FILE_CACHE_INODE_LIMIT, "2000"));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_FILE_CACHE_CLEAN_INTERVAL, "2m"));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_FILE_CACHE_METADATA_SIZE, "1m"));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_WORKER_QUEUE, "/tmp/jobs"));
  CHECK(service_finalize(&expected_service));
  CHECK(memcmp(&options.service, &expected_service, sizeof(options.service)) == 0);
  laghu_service_config_init(&expected_service);
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_IMAGE_CACHE, "/tmp/cache"));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND, "file:///tmp/other"));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_WORKER_QUEUE, "/tmp/jobs"));
  CHECK(!service_finalize(&expected_service));
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(11, bad_cidr, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(11, bad_rum, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(11, native_file, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(13, unbound_file, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  {
    char *service[] = {"laghu", "--service"};
    laghu_proxy_options_init(&options);
    CHECK(laghu_proxy_parse_options(2, service, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  }
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(14, conflict, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(11, bad_drain, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(15, filters, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(options.config.enabled_filters == LAGHU_FILTER_RESOURCE_INLINE);
  CHECK(options.config.disabled_filters == LAGHU_FILTER_HTML_MINIFY);
  CHECK(options.config.forbidden_filters == LAGHU_FILTER_JAVASCRIPT_DEFER);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(21, request_policy, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(options.config.allow_resource_count == 1U);
  CHECK(options.config.disallow_resource_count == 1U);
  CHECK(options.config.respect_vary == LAGHU_MODE_OFF);
  CHECK(options.config.respect_x_forwarded_proto == LAGHU_MODE_ON);
  CHECK(options.config.query_filter_overrides == LAGHU_MODE_ON);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(15, domain_policy, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(options.config.domain_policy.mapping_count == 1U);
  CHECK(options.config.domain_policy.group_count == 1U);
  CHECK(options.config.domain_policy.groups[0].shard_count == 2U);
  CHECK(strcmp(options.config.domain_policy.mappings[0].source_origin, "https://origin.example") == 0);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(13, filter_conflict, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  CHECK(laghu_proxy_decode_chunked((laghu_buffer){chunked, sizeof(chunked) - 1U}, decoded, sizeof(decoded), &decoded_length));
  CHECK(decoded_length == 9U && !memcmp(decoded, "Wikipedia", 9U));
  CHECK(!laghu_proxy_decode_chunked((laghu_buffer){(const unsigned char *)"3\r\nab", 5U}, decoded, sizeof(decoded), &decoded_length));
  laghu_proxy_options_dispose(&options);
  free(options_storage);
  puts("laghu proxy tests passed");
#undef options
  return 0;
}
