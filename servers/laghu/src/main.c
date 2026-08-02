// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>

#include "laghu/proxy.h"

static void usage(FILE *stream) {
  fputs(
      "Usage: laghu --listen HOST:PORT --origin http[s]://HOST[:PORT] "
      "--cache PATH --worker-queue PATH [options]\n"
      "Options:\n"
      "  --preset NAME | --rewrite-level NAME\n"
      "  --enable-filter NAME --disable-filter NAME --forbid-filter NAME\n"
      "  --allow-api --image-beacon --critical-css-beacon "
      "--instrumentation-beacon "
      "--image-quality 1..100\n"
      "  --workers N --connection-queue N\n"
      "  --connect-timeout SECONDS --io-timeout SECONDS\n"
      "  --drain-timeout SECONDS\n"
      "  --origin-ca-file PATH\n"
      "  --font-fetch-queue PATH\n"
      "  --font-provider-config PATH\n"
      "  --javascript-queue PATH\n"
      "  --javascript-target QUERY\n"
      "  --javascript-inline-limit 0..65536\n"
      "  --javascript-outline-threshold 1024..1048576\n"
      "  --instrumentation-sample-rate 0..100\n"
      "  --javascript-defer-suggestions on|off\n"
      "  --javascript-defer-config PATH\n"
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
      "  --forwarded-headers off|forwarded|x-forwarded|both\n"
      "  --trusted-proxy CIDR (repeatable, maximum 64)\n",
      stream);
#ifdef _WIN32
  fputs("  --service\n", stream);
#endif
}

int main(int argc, char **argv) {
  laghu_proxy_options options;
  char error[256];
  laghu_proxy_parse_result parsed;
  laghu_proxy_options_init(&options);
  parsed =
      laghu_proxy_parse_options(argc, argv, &options, error, sizeof(error));
  if (parsed == LAGHU_PROXY_PARSE_HELP) {
    usage(stdout);
    return 0;
  }
  if (parsed == LAGHU_PROXY_PARSE_VERSION) {
    puts("laghu " LAGHU_VERSION);
    return 0;
  }
  if (parsed != LAGHU_PROXY_PARSE_OK) {
    fprintf(stderr, "laghu: %s\n", error);
    usage(stderr);
    return 2;
  }
#ifdef _WIN32
  if (options.service_mode) return laghu_proxy_run_service(&options);
#endif
  return laghu_proxy_run(&options);
}
