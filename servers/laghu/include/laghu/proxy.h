// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_PROXY_H
#define LAGHU_PROXY_H

#include <stdbool.h>
#include <stddef.h>

#include "laghu/http.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_PROXY_HEADER_BYTES 65536U
#define LAGHU_PROXY_LINE_BYTES 8192U
#define LAGHU_PROXY_REQUEST_BODY_BYTES 1048576U
#define LAGHU_PROXY_DEFAULT_WORKERS 4U
#define LAGHU_PROXY_DEFAULT_QUEUE 64U
#define LAGHU_PROXY_DEFAULT_CONNECT_TIMEOUT 5U
#define LAGHU_PROXY_DEFAULT_IO_TIMEOUT 30U

typedef struct {
  char listen_host[256];
  char listen_port[6];
  char origin_host[256];
  char origin_port[6];
  char origin_authority[264];
  char cache_path[LAGHU_RUNTIME_PATH_SIZE];
  char worker_queue_path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_config config;
  unsigned int workers;
  unsigned int connection_queue;
  unsigned int connect_timeout;
  unsigned int io_timeout;
} laghu_proxy_options;

typedef enum {
  LAGHU_PROXY_PARSE_OK = 0,
  LAGHU_PROXY_PARSE_HELP,
  LAGHU_PROXY_PARSE_VERSION,
  LAGHU_PROXY_PARSE_ERROR
} laghu_proxy_parse_result;

void laghu_proxy_options_init(laghu_proxy_options *options);
laghu_proxy_parse_result laghu_proxy_parse_options(int argc, char **argv,
                                                   laghu_proxy_options *options,
                                                   char *error,
                                                   size_t error_size);
bool laghu_proxy_decode_chunked(laghu_buffer encoded, unsigned char *decoded,
                                size_t capacity, size_t *decoded_length);
int laghu_proxy_run(const laghu_proxy_options *options);

#ifdef __cplusplus
}
#endif

#endif
