// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>

#include "laghu/proxy.h"

static void usage(FILE *stream) {
  fputs(
      "Usage: laghu --listen HOST:PORT --origin http://HOST[:PORT] "
      "--cache PATH --worker-queue PATH [options]\n"
      "Options:\n"
      "  --preset NAME | --rewrite-level NAME\n"
      "  --allow-api --image-beacon --image-quality 1..100\n"
      "  --workers N --connection-queue N\n"
      "  --connect-timeout SECONDS --io-timeout SECONDS\n",
      stream);
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
  return laghu_proxy_run(&options);
}
