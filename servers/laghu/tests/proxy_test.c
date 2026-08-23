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

static bool write_yaml_fixture(char path[]) {
  static const char fixture[] =
      ""
      "runtime:\n"
      "  listen: 127.0.0.1:8080\n"
      "  origin: http://127.0.0.1:8000\n"
      "  cache: /tmp/cache\n"
      "  worker_queue: /tmp/jobs\n"
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

static bool passthrough_queue_requirements_test(void) {
  static const char global_passthrough[] =
      "runtime:\n"
      "  listen: 127.0.0.1:8080\n"
      "  origin: http://127.0.0.1:8000\n"
      "  cache: /tmp/cache\n"
      "  rewrite_level: passthrough\n"
      "sites:\n"
      "  - host: static.example.test\n"
      "    document_root: /srv/static\n"
      "routes:\n"
      "  - match: exact\n"
      "    pattern: /redirect\n"
      "    redirect: /result\n";
  static const char global_active[] =
      "runtime:\n"
      "  listen: 127.0.0.1:8080\n"
      "  origin: http://127.0.0.1:8000\n"
      "  cache: /tmp/cache\n"
      "  rewrite_level: core\n";
  static const char nested_passthrough[] =
      "runtime:\n"
      "  listen: 127.0.0.1:8080\n"
      "  origin: http://127.0.0.1:8000\n"
      "  cache: /tmp/cache\n"
      "  worker_queue: /tmp/jobs\n"
      "  rewrite_level: core\n"
      "sites:\n"
      "  - host: static.example.test\n"
      "    document_root: /srv/static\n"
      "    laghu:\n"
      "      rewrite_level: passthrough\n"
      "routes:\n"
      "  - match: exact\n"
      "    pattern: /redirect\n"
      "    redirect: /result\n"
      "    laghu:\n"
      "      rewrite_level: passthrough\n";
  static const char site_active[] =
      "runtime:\n"
      "  listen: 127.0.0.1:8080\n"
      "  origin: http://127.0.0.1:8000\n"
      "  cache: /tmp/cache\n"
      "  rewrite_level: passthrough\n"
      "sites:\n"
      "  - host: static.example.test\n"
      "    document_root: /srv/static\n"
      "    laghu:\n"
      "      rewrite_level: core\n";
  static const char route_active[] =
      "runtime:\n"
      "  listen: 127.0.0.1:8080\n"
      "  origin: http://127.0.0.1:8000\n"
      "  cache: /tmp/cache\n"
      "  rewrite_level: passthrough\n"
      "routes:\n"
      "  - match: exact\n"
      "    pattern: /redirect\n"
      "    redirect: /result\n"
      "    laghu:\n"
      "      rewrite_level: core\n";
  const char *fixtures[] = {global_passthrough, nested_passthrough, global_active, site_active, route_active};
  bool expected[] = {true, true, false, false, false};
  size_t index;
  for (index = 0U; index < sizeof(fixtures) / sizeof(fixtures[0]); ++index) {
    char path[] = "/tmp/laghu-passthrough-queue-XXXXXX";
    laghu_proxy_options options;
    char error[128U];
    int file = mkstemp(path);
    bool loaded;
    if (file < 0 || write(file, fixtures[index], strlen(fixtures[index])) != (ssize_t)strlen(fixtures[index]) || close(file) != 0) return false;
    laghu_proxy_options_init(&options);
    loaded = laghu_proxy_load_yaml(path, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK;
    if (loaded && index == 0U)
      loaded = options.service.worker_queue[0] == '\0' && options.sites[0].service.worker_queue[0] == '\0' &&
               options.routes[0].service.worker_queue[0] == '\0';
    if (loaded && index == 1U)
      loaded = options.service.worker_queue[0] != '\0' && options.sites[0].service.worker_queue[0] == '\0' &&
               options.routes[0].service.worker_queue[0] == '\0';
    laghu_proxy_options_dispose(&options);
    (void)unlink(path);
    if (loaded != expected[index]) return false;
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
  return access.status == 200U && strstr(output, "Content-Type: text/plain") != NULL;
}

static bool route_rewrite_test(void) {
  laghu_proxy_options options;
  proxy_request request = {0};
  laghu_proxy_options_init(&options);
  options.route_count = 1U;
  options.routes[0].match = LAGHU_PROXY_ROUTE_PREFIX;
  (void)snprintf(options.routes[0].pattern, sizeof(options.routes[0].pattern), "%s", "/legacy/");
  (void)snprintf(options.routes[0].rewrite, sizeof(options.routes[0].rewrite), "%s", "/index.html");
  (void)snprintf(request.target, sizeof(request.target), "%s", "/legacy/page?keep=ignored");
  return proxy_route_rewrite(&options, &request) && !strcmp(request.target, "/index.html");
}

static bool scoped_rules_test(void) {
  static const char auth_contents[] = "operator:sha256:0000000000000000000000000000000000000000000000000000000000000000\n";
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
             merged.allow_count == 1U && merged.basic_auth_user_count == 1U && !strcmp(merged.basic_auth_realm, "Private") &&
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
                   "45"};
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
  CHECK(options.service.trusted_proxy_count == 1U);
  CHECK(options.response_header_count == 2U && !strcmp(options.response_headers[0].name, "X-Static-Policy"));
  CHECK(options.site_count == 1U && !strcmp(options.sites[0].host, "static.example.test"));
  CHECK(options.sites[0].config.preset == LAGHU_PRESET_SAFE && !strcmp(options.sites[0].service.javascript_target, "defaults"));
  CHECK(options.route_count == 2U && options.routes[1].match == LAGHU_PROXY_ROUTE_ORDERED_REGEX);
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
    laghu_proxy_options_dispose(resolved);
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
  CHECK(static_response_test());
  CHECK(route_rewrite_test());
  CHECK(scoped_rules_test());
  CHECK(gateway_health_rejected_test());
  CHECK(health_interval_requires_check_test());
  CHECK(passthrough_queue_requirements_test());
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(19, admin, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(options.service.purge_method && options.service.purge_query && options.service.statistics && options.service.purge_allow_count == 1U);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(22, valid, &options, error, sizeof(error)) == LAGHU_PROXY_PARSE_OK);
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
    proxy_origin_release(&worker, &first_origin, true);
    proxy_origin_release(&worker, &second_origin, true);
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
