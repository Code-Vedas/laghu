// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/proxy.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value)                                          \
  do {                                                        \
    if (!(value)) {                                           \
      fprintf(stderr, "check failed at line %d\n", __LINE__); \
      return 1;                                               \
    }                                                         \
  } while (0)

#define TEST_BACKEND_URI "file:///tmp/cache"

static bool service_apply(laghu_service_config *config,
                          laghu_service_setting setting, const char *value) {
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

int main(void) {
  laghu_proxy_options options;
  laghu_service_config expected_service;
  char error[128];
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
  char *backend_conflict[] = {"laghu",
                              "--listen",
                              "127.0.0.1:8080",
                              "--origin",
                              "http://127.0.0.1:8000",
                              "--cache",
                              "/tmp/cache",
                              "--file-cache-backend",
                              "file:///tmp/cache",
                              "--worker-queue",
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
  char *conflict[] = {"laghu",
                      "--listen",
                      "127.0.0.1:8080",
                      "--origin",
                      "http://127.0.0.1:8000",
                      "--cache",
                      "/tmp/cache",
                      "--worker-queue",
                      "/tmp/jobs",
                      "--preset",
                      "safe",
                      "--rewrite-level",
                      "core"};
  char *bad_drain[] = {"laghu",
                       "--listen",
                       "127.0.0.1:8080",
                       "--origin",
                       "http://127.0.0.1:8000",
                       "--cache",
                       "/tmp/cache",
                       "--worker-queue",
                       "/tmp/jobs",
                       "--drain-timeout",
                       "0"};
  char *filters[] = {"laghu",
                     "--listen",
                     "127.0.0.1:8080",
                     "--origin",
                     "http://127.0.0.1:8000",
                     "--cache",
                     "/tmp/cache",
                     "--worker-queue",
                     "/tmp/jobs",
                     "--enable-filter",
                     "resource_inline",
                     "--disable-filter",
                     "html_minify",
                     "--forbid-filter",
                     "javascript_defer"};
  char *filter_conflict[] = {"laghu",
                             "--listen",
                             "127.0.0.1:8080",
                             "--origin",
                             "http://127.0.0.1:8000",
                             "--cache",
                             "/tmp/cache",
                             "--worker-queue",
                             "/tmp/jobs",
                             "--disable-filter",
                             "html_minify",
                             "--enable-filter",
                             "html_minify"};
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
  char *secure[] = {"laghu",
                    "--listen",
                    "127.0.0.1:8080",
                    "--origin",
                    "https://example.test",
                    "--cache",
                    "/tmp/cache",
                    "--worker-queue",
                    "/tmp/jobs",
                    "--origin-ca-file",
                    "/tmp/ca.pem",
                    "--forwarded-headers",
                    "both",
                    "--trusted-proxy",
                    "127.0.0.0/8",
                    "--trusted-proxy",
                    "2001:db8::/32"};
  char *bad_cidr[] = {"laghu",
                      "--listen",
                      "127.0.0.1:8080",
                      "--origin",
                      "http://127.0.0.1:8000",
                      "--cache",
                      "/tmp/cache",
                      "--worker-queue",
                      "/tmp/jobs",
                      "--trusted-proxy",
                      "127.0.0.1/8"};
  char *bad_rum[] = {"laghu",
                     "--listen",
                     "127.0.0.1:8080",
                     "--origin",
                     "http://127.0.0.1:8000",
                     "--cache",
                     "/tmp/cache",
                     "--worker-queue",
                     "/tmp/jobs",
                     "--rum-store",
                     "redis://example.test:6379/0"};
  char *native_file[] = {"laghu",
                         "--listen",
                         "127.0.0.1:8080",
                         "--origin",
                         "http://127.0.0.1:8000",
                         "--cache",
                         "/tmp/cache",
                         "--worker-queue",
                         "/tmp/jobs",
                         "--load-from-file",
                         "native"};
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
  size_t decoded_length = 0U;
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(19, admin, &options, error, sizeof(error)) ==
        LAGHU_PROXY_PARSE_OK);
  CHECK(options.service.purge_method && options.service.purge_query &&
        options.service.statistics && options.service.purge_allow_count == 1U);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(20, valid, &options, error, sizeof(error)) ==
        LAGHU_PROXY_PARSE_OK);
  CHECK(!strcmp(options.origin_host, "127.0.0.1"));
  CHECK(!strcmp(options.origin_port, "8000"));
  CHECK(options.config.allow_api == LAGHU_MODE_ON);
  CHECK(options.config.critical_css_beacon == LAGHU_MODE_ON);
  CHECK(options.config.instrumentation_beacon == LAGHU_MODE_ON);
  CHECK(options.config.instrumentation_sample_rate == 50U);
  CHECK(options.config.javascript_inline_limit == 4096U);
  CHECK(options.config.javascript_outline_threshold == 16384U);
  CHECK(options.drain_timeout == 45U);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(17, backend, &options, error,
                                  sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(!strcmp(options.service.file_cache_backend, "file:///tmp/cache"));
  CHECK(!strcmp(options.service.image_cache, "/tmp/cache"));
  CHECK(options.service.cache_limits.size_limit == 20U * 1024U * 1024U);
  CHECK(options.service.cache_limits.inode_limit == 2000U);
  CHECK(options.service.cache_limits.clean_interval == 120U);
  CHECK(options.service.cache_limits.metadata_size == 1024U * 1024U);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(15, budgets, &options, error,
                                  sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(options.config.transform_memory_limit == 64U * 1024U * 1024U);
  CHECK(options.config.transform_deadline_ms == 125U);
  CHECK(options.config.variants_per_source == 8U);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(11, backend_conflict, &options, error,
                                  sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(17, secure, &options, error, sizeof(error)) ==
        LAGHU_PROXY_PARSE_OK);
  CHECK(options.origin_tls && !strcmp(options.origin_port, "443"));
  CHECK(options.forwarded_mode == LAGHU_PROXY_FORWARDED_BOTH);
  CHECK(options.service.trusted_proxy_count == 2U);
  laghu_service_config_init(&expected_service);
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_IMAGE_CACHE,
                      "/tmp/cache"));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_WORKER_QUEUE,
                      "/tmp/jobs"));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_TRUSTED_PROXY,
                      "127.0.0.0/8"));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_TRUSTED_PROXY,
                      "2001:db8::/32"));
  CHECK(service_finalize(&expected_service));
  CHECK(memcmp(&options.service, &expected_service, sizeof(options.service)) ==
        0);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(26, rum, &options, error, sizeof(error)) ==
        LAGHU_PROXY_PARSE_OK);
  CHECK(!strcmp(options.service.rum_store, "local:/tmp/rum"));
  CHECK(!strcmp(options.service.rum_snapshot_path, "/tmp/rum.snapshot"));
  CHECK(options.service.rum_timeout_ms == 75U &&
        options.service.rum_ttl == 604800U);
  CHECK(options.service.rum_retry_limit == 2U &&
        options.service.rum_sync_interval == 5U);
  CHECK(options.service.rum_memory_limit == 8U * 1024U * 1024U);
  CHECK(options.service.rum_pending_limit == 1024U * 1024U &&
        options.service.rum_store_required);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(17, backend, &options, error,
                                  sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  laghu_service_config_init(&expected_service);
  CHECK(service_apply(&expected_service,
                      LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND,
                      TEST_BACKEND_URI));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_FILE_CACHE_SIZE,
                      "20m"));
  CHECK(service_apply(&expected_service,
                      LAGHU_SERVICE_SETTING_FILE_CACHE_INODE_LIMIT, "2000"));
  CHECK(service_apply(&expected_service,
                      LAGHU_SERVICE_SETTING_FILE_CACHE_CLEAN_INTERVAL, "2m"));
  CHECK(service_apply(&expected_service,
                      LAGHU_SERVICE_SETTING_FILE_CACHE_METADATA_SIZE, "1m"));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_WORKER_QUEUE,
                      "/tmp/jobs"));
  CHECK(service_finalize(&expected_service));
  CHECK(memcmp(&options.service, &expected_service, sizeof(options.service)) ==
        0);
  laghu_service_config_init(&expected_service);
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_IMAGE_CACHE,
                      "/tmp/cache"));
  CHECK(service_apply(&expected_service,
                      LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND,
                      "file:///tmp/other"));
  CHECK(service_apply(&expected_service, LAGHU_SERVICE_SETTING_WORKER_QUEUE,
                      "/tmp/jobs"));
  CHECK(!service_finalize(&expected_service));
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(11, bad_cidr, &options, error,
                                  sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(11, bad_rum, &options, error,
                                  sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(11, native_file, &options, error,
                                  sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(13, unbound_file, &options, error,
                                  sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  {
    char *service[] = {"laghu", "--service"};
    laghu_proxy_options_init(&options);
    CHECK(laghu_proxy_parse_options(2, service, &options, error,
                                    sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  }
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(14, conflict, &options, error,
                                  sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(11, bad_drain, &options, error,
                                  sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(15, filters, &options, error,
                                  sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(options.config.enabled_filters == LAGHU_FILTER_RESOURCE_INLINE);
  CHECK(options.config.disabled_filters == LAGHU_FILTER_HTML_MINIFY);
  CHECK(options.config.forbidden_filters == LAGHU_FILTER_JAVASCRIPT_DEFER);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(21, request_policy, &options, error,
                                  sizeof(error)) == LAGHU_PROXY_PARSE_OK);
  CHECK(options.config.allow_resource_count == 1U);
  CHECK(options.config.disallow_resource_count == 1U);
  CHECK(options.config.respect_vary == LAGHU_MODE_OFF);
  CHECK(options.config.respect_x_forwarded_proto == LAGHU_MODE_ON);
  CHECK(options.config.query_filter_overrides == LAGHU_MODE_ON);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(13, filter_conflict, &options, error,
                                  sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  CHECK(
      laghu_proxy_decode_chunked((laghu_buffer){chunked, sizeof(chunked) - 1U},
                                 decoded, sizeof(decoded), &decoded_length));
  CHECK(decoded_length == 9U && !memcmp(decoded, "Wikipedia", 9U));
  CHECK(!laghu_proxy_decode_chunked(
      (laghu_buffer){(const unsigned char *)"3\r\nab", 5U}, decoded,
      sizeof(decoded), &decoded_length));
  puts("laghu proxy tests passed");
  return 0;
}
