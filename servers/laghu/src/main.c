// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <string.h>

#include "laghu/proxy.h"
#include "laghu/status.h"

static void usage(FILE *stream) {
  fputs(
      "Usage: laghu [--config PATH]\n"
      "Options:\n"
      "  --preset NAME | --rewrite-level NAME\n"
      "  --enable-filter NAME --disable-filter NAME --forbid-filter NAME\n"
      "  --allow-resources PATTERN --disallow PATTERN\n"
      "  --domain HTTPS_ORIGIN\n"
      "  --map-rewrite-domain HTTPS_PUBLIC_ORIGIN HTTPS_SOURCE_ORIGIN\n"
      "  --shard-domain HTTPS_PUBLIC_ORIGIN HTTPS_SHARD_ORIGINS\n"
      "  --map-proxy-domain HTTPS_PUBLIC_ORIGIN HTTPS_SOURCE_ORIGIN\n"
      "  --respect-vary on|off --respect-x-forwarded-proto on|off\n"
      "  --query-filter-overrides on|off\n"
      "  --allow-api --image-beacon --critical-css-beacon "
      "--instrumentation-beacon "
      "--image-quality 1..100\n"
      "  --workers N --connection-queue N\n"
      "  --connect-timeout SECONDS --io-timeout SECONDS\n"
      "  --origin-pool-size 0..1024 --origin-idle-timeout SECONDS\n"
      "  --drain-timeout SECONDS\n"
      "  --transform-memory-limit 4m..256m\n"
      "  --transform-deadline-ms 5..1000\n"
      "  --variants-per-source 1..64\n"
      "  --origin-ca-file PATH\n"
      "  --tls-certificate PATH --tls-private-key PATH\n"
      "  --pid-file PATH\n"
      "  --font-fetch-queue PATH\n"
      "  --font-provider-config PATH\n"
      "  --javascript-queue PATH\n"
      "  --javascript-target QUERY\n"
      "  --javascript-inline-limit 0..65536\n"
      "  --javascript-outline-threshold 1024..1048576\n"
      "  --instrumentation-sample-rate 0..100\n"
      "  --optimization-profiles on|off\n"
      "  --javascript-defer-suggestions on|off\n"
      "  --javascript-defer-config PATH\n"
      "  --layout-reservation-config PATH\n"
      "  --otel-endpoint HTTPS_URL --otel-trace-queue PATH\n"
      "  --otel-sampling-rate 0..100 --otel-ca-file PATH\n"
      "  --include-js-source-maps\n"
      "  --rum-store URI [--rum-store-required]\n"
      "  --rum-store-local-snapshot PATH\n"
      "  --rum-store-client-library PATH\n"
      "  --rum-store-timeout MILLISECONDS --rum-store-ttl SECONDS\n"
      "  --rum-store-retry-limit N --rum-store-sync-interval SECONDS\n"
      "  --rum-store-memory-limit BYTES --rum-store-pending-limit BYTES\n"
      "  --javascript-observation-config PATH\n"
      "  --asset-offload-config PATH\n"
      "  --asset-upload-queue PATH\n"
      "  --load-from-file off|mapped\n"
      "  --file-source-map HTTPS_PREFIX=ABSOLUTE_ROOT (repeatable)\n"
      "  --forwarded-headers off|forwarded|x-forwarded|both\n"
      "  --trusted-proxy CIDR (repeatable, maximum 64)\n"
      "  --purge-method PURGE --purge-query on|off\n"
      "  --purge-token-file PATH --purge-allow CIDR\n"
      "  --cache-flush-file PATH --statistics on|off\n"
      "Commands:\n"
      "  laghu status URL --token-file PATH [--timeout SECONDS] [--ca-file "
      "PATH] [--json]\n"
      "  laghu doctor URL --token-file PATH [--timeout SECONDS] [--ca-file "
      "PATH] [--json]\n"
      "  laghu purge URL --token-file PATH [--timeout SECONDS] [--ca-file "
      "PATH] [--json]\n"
      "  laghu explain URL --token-file PATH [--timeout SECONDS] [--ca-file "
      "PATH] [--json]\n"
      "  laghu bench URL [--requests N] [--timeout SECONDS] [--ca-file PATH] "
      "[--json]\n"
      "  laghu reload [--config PATH]\n",
      stream);
}

int main(int argc, char **argv) {
  laghu_proxy_options options;
  const char *config_path = NULL;
  char error[256];
  laghu_proxy_parse_result parsed;
  if (argc > 1 && strcmp(argv[1], "status") == 0) return laghu_status_run(argc - 1, argv + 1);
  if (argc > 1 && strcmp(argv[1], "doctor") == 0) return laghu_doctor_run(argc - 1, argv + 1);
  if (argc > 1 && strcmp(argv[1], "purge") == 0) return laghu_purge_run(argc - 1, argv + 1);
  if (argc > 1 && strcmp(argv[1], "explain") == 0) return laghu_explain_run(argc - 1, argv + 1);
  if (argc > 1 && strcmp(argv[1], "bench") == 0) return laghu_bench_run(argc - 1, argv + 1);
  if (argc == 2 && strcmp(argv[1], "reload") == 0) {
    int result = laghu_proxy_reload(LAGHU_PROXY_DEFAULT_CONFIG_PATH, error, sizeof(error));
    if (result != 0) fprintf(stderr, "laghu reload: %s\n", error);
    return result;
  }
  if (argc == 4 && strcmp(argv[1], "reload") == 0 && strcmp(argv[2], "--config") == 0) {
    int result = laghu_proxy_reload(argv[3], error, sizeof(error));
    if (result != 0) fprintf(stderr, "laghu reload: %s\n", error);
    return result;
  }
  laghu_proxy_options_init(&options);
  if (argc == 1) {
    config_path = LAGHU_PROXY_DEFAULT_CONFIG_PATH;
    parsed = laghu_proxy_load_yaml(config_path, &options, error, sizeof(error));
  } else if (argc == 3 && strcmp(argv[1], "--config") == 0) {
    config_path = argv[2];
    parsed = laghu_proxy_load_yaml(config_path, &options, error, sizeof(error));
  } else if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "--version") == 0)) {
    parsed = laghu_proxy_parse_options(argc, argv, &options, error, sizeof(error));
  } else {
    (void)snprintf(error, sizeof(error), "runtime settings belong in YAML; use --config PATH only to override the main config");
    parsed = LAGHU_PROXY_PARSE_ERROR;
  }
  if (parsed == LAGHU_PROXY_PARSE_HELP) {
    usage(stdout);
    laghu_proxy_options_dispose(&options);
    return 0;
  }
  if (parsed == LAGHU_PROXY_PARSE_VERSION) {
    puts("laghu " LAGHU_BUILD_REVISION);
    laghu_proxy_options_dispose(&options);
    return 0;
  }
  if (parsed != LAGHU_PROXY_PARSE_OK) {
    fprintf(stderr, "laghu: %s\n", error);
    usage(stderr);
    laghu_proxy_options_dispose(&options);
    return 2;
  }
  {
    int result = laghu_proxy_run_with_config(&options, config_path);
    laghu_proxy_options_dispose(&options);
    return result;
  }
}
