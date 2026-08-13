// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_RUNTIME_TEST_FIXTURE_H
#define LAGHU_RUNTIME_TEST_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>

#include "laghu/queue.h"
#include "laghu/types.h"

typedef struct {
  char path[LAGHU_RUNTIME_PATH_SIZE];
  bool created;
} laghu_test_workspace;

typedef struct {
  laghu_runtime_queue producer;
  laghu_runtime_queue consumer;
  char path[LAGHU_RUNTIME_PATH_SIZE];
  bool producer_open;
  bool consumer_open;
} laghu_test_queue_pair;

bool laghu_test_directory(char *path, size_t capacity);
bool laghu_test_workspace_create(laghu_test_workspace *workspace);
bool laghu_test_workspace_remove(laghu_test_workspace *workspace);
bool laghu_test_workspace_path(const laghu_test_workspace *workspace, const char *relative, char *path, size_t capacity);
bool laghu_test_workspace_write(const laghu_test_workspace *workspace, const char *relative, const unsigned char *data, size_t length);
bool laghu_test_cache_uri(const char *path, char *uri, size_t capacity);
bool laghu_test_queue_pair_open(laghu_test_queue_pair *pair, const laghu_test_workspace *workspace, const char *relative, unsigned int slots,
                                size_t payload_capacity);
void laghu_test_queue_pair_close(laghu_test_queue_pair *pair);

#endif
