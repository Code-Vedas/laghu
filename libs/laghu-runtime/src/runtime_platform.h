// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_RUNTIME_PLATFORM_H
#define LAGHU_RUNTIME_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  intptr_t platform_file;
  intptr_t platform_mapping;
  void *mapping;
  size_t mapping_length;
} laghu_runtime_shared_mapping;

void laghu_runtime_shared_mapping_init(laghu_runtime_shared_mapping *mapping);
bool laghu_runtime_directory_ensure(const char *path);
bool laghu_runtime_directory_exists(const char *path);
bool laghu_runtime_file_remove(const char *path);
bool laghu_runtime_file_size(const char *path, uint64_t *size);
bool laghu_runtime_file_read_exact(const char *path, unsigned char *data,
                                   size_t length);
bool laghu_runtime_file_write_atomic(const char *path,
                                     const unsigned char *data, size_t length);
bool laghu_runtime_shared_mapping_open(laghu_runtime_shared_mapping *mapping,
                                       const char *path, size_t size);
/* Opens a fixed, already-validated mapping without taking its writer lock. */
bool laghu_runtime_shared_mapping_open_existing(
    laghu_runtime_shared_mapping *mapping, const char *path, size_t size);
bool laghu_runtime_shared_mapping_open_prefix(
    laghu_runtime_shared_mapping *mapping, const char *path, size_t size);
bool laghu_runtime_shared_mapping_try_lock(
    laghu_runtime_shared_mapping *mapping);
void laghu_runtime_shared_mapping_unlock(laghu_runtime_shared_mapping *mapping);
bool laghu_runtime_shared_mapping_sync(laghu_runtime_shared_mapping *mapping);
void laghu_runtime_shared_mapping_close(laghu_runtime_shared_mapping *mapping);

#endif
