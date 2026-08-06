// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "runtime_platform.h"

static bool laghu_runtime_read_all(int file, unsigned char *data,
                                   size_t length) {
  while (length != 0U) {
    ssize_t count = read(file, data, length);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    data += (size_t)count;
    length -= (size_t)count;
  }
  return true;
}

static bool laghu_runtime_write_all(int file, const unsigned char *data,
                                    size_t length) {
  while (length != 0U) {
    ssize_t count = write(file, data, length);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    data += (size_t)count;
    length -= (size_t)count;
  }
  return true;
}

static bool laghu_runtime_lock(int file) {
  return file >= 0 && flock(file, LOCK_EX | LOCK_NB) == 0;
}

static void laghu_runtime_unlock(int file) {
  if (file >= 0) (void)flock(file, LOCK_UN);
}

void laghu_runtime_shared_mapping_init(laghu_runtime_shared_mapping *mapping) {
  if (mapping == NULL) return;
  memset(mapping, 0, sizeof(*mapping));
  mapping->platform_file = -1;
  mapping->platform_mapping = -1;
}

bool laghu_runtime_directory_ensure(const char *path) {
  struct stat status;
  return path != NULL && ((mkdir(path, 0750) == 0) ||
                          (errno == EEXIST && stat(path, &status) == 0 &&
                           S_ISDIR(status.st_mode)));
}

bool laghu_runtime_directory_exists(const char *path) {
  struct stat status;
  return path != NULL && stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

bool laghu_runtime_file_remove(const char *path) {
  return path != NULL && (unlink(path) == 0 || errno == ENOENT);
}

bool laghu_runtime_file_size(const char *path, uint64_t *size) {
  struct stat status;
  if (path == NULL || size == NULL || stat(path, &status) != 0 ||
      status.st_size < 0)
    return false;
  *size = (uint64_t)status.st_size;
  return true;
}

bool laghu_runtime_file_read_exact(const char *path, unsigned char *data,
                                   size_t length) {
  int file;
  bool success;
  if (path == NULL || data == NULL) return false;
  file = open(path, O_RDONLY);
  if (file < 0) return false;
  success = laghu_runtime_read_all(file, data, length);
  if (close(file) != 0) success = false;
  return success;
}

bool laghu_runtime_file_write_atomic(const char *path,
                                     const unsigned char *data, size_t length) {
  char temporary[4096U];
  int file;
  int written;
  bool success;
  if (path == NULL || data == NULL) return false;
  written = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
                     (long)getpid());
  if (written <= 0 || (size_t)written >= sizeof(temporary)) return false;
  file = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0640);
  if (file < 0) return false;
  success = laghu_runtime_write_all(file, data, length) && fsync(file) == 0;
  if (close(file) != 0) success = false;
  file = -1;
  if (success) success = rename(temporary, path) == 0;
  if (!success) {
    (void)close(file);
    (void)unlink(temporary);
  }
  return success;
}

bool laghu_runtime_shared_mapping_open(laghu_runtime_shared_mapping *mapping,
                                       const char *path, size_t size) {
  int file;
  struct stat status;
  void *address;
  if (mapping == NULL || path == NULL || size < 4096U ||
      size > (size_t)INT64_MAX)
    return false;
  laghu_runtime_shared_mapping_init(mapping);
  file = open(path, O_RDWR | O_CREAT, 0640);
  if (file < 0 || !laghu_runtime_lock(file) || fstat(file, &status) != 0 ||
      (status.st_size != 0 && (uintmax_t)status.st_size != (uintmax_t)size) ||
      (status.st_size == 0 && ftruncate(file, (off_t)size) != 0)) {
    if (file >= 0) {
      laghu_runtime_unlock(file);
      (void)close(file);
    }
    return false;
  }
  address = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
  laghu_runtime_unlock(file);
  if (address == MAP_FAILED) {
    (void)close(file);
    return false;
  }
  mapping->platform_file = file;
  mapping->mapping = address;
  mapping->mapping_length = size;
  return true;
}

bool laghu_runtime_shared_mapping_try_lock(
    laghu_runtime_shared_mapping *mapping) {
  return mapping != NULL && mapping->mapping != NULL &&
         laghu_runtime_lock((int)mapping->platform_file);
}

void laghu_runtime_shared_mapping_unlock(
    laghu_runtime_shared_mapping *mapping) {
  if (mapping != NULL) laghu_runtime_unlock((int)mapping->platform_file);
}

bool laghu_runtime_shared_mapping_sync(laghu_runtime_shared_mapping *mapping) {
  return mapping != NULL && mapping->mapping != NULL &&
         msync(mapping->mapping, mapping->mapping_length, MS_ASYNC) == 0;
}

void laghu_runtime_shared_mapping_close(laghu_runtime_shared_mapping *mapping) {
  if (mapping == NULL) return;
  if (mapping->mapping != NULL)
    (void)munmap(mapping->mapping, mapping->mapping_length);
  if (mapping->platform_file >= 0) (void)close((int)mapping->platform_file);
  laghu_runtime_shared_mapping_init(mapping);
}
