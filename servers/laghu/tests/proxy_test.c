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

int main(void) {
  laghu_proxy_options options;
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
                   "--drain-timeout",
                   "45"};
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
  static const unsigned char chunked[] = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
  unsigned char decoded[16];
  size_t decoded_length = 0U;
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(12, valid, &options, error, sizeof(error)) ==
        LAGHU_PROXY_PARSE_OK);
  CHECK(!strcmp(options.origin_host, "127.0.0.1"));
  CHECK(!strcmp(options.origin_port, "8000"));
  CHECK(options.config.allow_api == LAGHU_MODE_ON);
  CHECK(options.drain_timeout == 45U);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(17, secure, &options, error, sizeof(error)) ==
        LAGHU_PROXY_PARSE_OK);
  CHECK(options.origin_tls && !strcmp(options.origin_port, "443"));
  CHECK(options.forwarded_mode == LAGHU_PROXY_FORWARDED_BOTH);
  CHECK(options.trusted_proxy_count == 2U);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(11, bad_cidr, &options, error,
                                  sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
#ifndef _WIN32
  {
    char *service[] = {"laghu", "--service"};
    laghu_proxy_options_init(&options);
    CHECK(laghu_proxy_parse_options(2, service, &options, error,
                                    sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  }
#endif
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(14, conflict, &options, error,
                                  sizeof(error)) == LAGHU_PROXY_PARSE_ERROR);
  laghu_proxy_options_init(&options);
  CHECK(laghu_proxy_parse_options(11, bad_drain, &options, error,
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
