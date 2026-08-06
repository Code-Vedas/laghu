// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_fixture.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool laghu_test_relative_path_valid(const char *relative) {
  const char *segment;
  size_t length;
  if (relative == NULL || relative[0] == '\0' || relative[0] == '/' ||
      relative[0] == '\\')
    return false;
  segment = relative;
  for (;;) {
    const char *cursor = segment;
    while (*cursor != '\0' && *cursor != '/') {
      if (*cursor == '\\' || *cursor == ':' || (unsigned char)*cursor < 32U)
        return false;
      ++cursor;
    }
    length = (size_t)(cursor - segment);
    if (length == 0U || (length == 1U && segment[0] == '.') ||
        (length == 2U && segment[0] == '.' && segment[1] == '.'))
      return false;
    if (*cursor == '\0') return true;
    segment = cursor + 1U;
  }
}

static bool laghu_test_join(const char *directory, const char *relative,
                            char *path, size_t capacity) {
  size_t directory_length, relative_length, index;
  if (directory == NULL || !laghu_test_relative_path_valid(relative) ||
      path == NULL || capacity == 0U)
    return false;
  directory_length = strlen(directory);
  relative_length = strlen(relative);
  if (directory_length == 0U || directory_length > capacity - 2U ||
      relative_length > capacity - directory_length - 2U)
    return false;
  memcpy(path, directory, directory_length);
  path[directory_length] = '/';
  for (index = 0U; index < relative_length; ++index)
    path[directory_length + 1U + index] = relative[index];
  path[directory_length + 1U + relative_length] = '\0';
  return true;
}

static bool laghu_test_make_parents(char *path) {
  char *cursor;
  if (path == NULL) return false;
  cursor = path[0] == '/' ? path + 1U : path;
  for (; *cursor != '\0'; ++cursor) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    if (path[0] != '\0' && mkdir(path, 0700) != 0 && errno != EEXIST) {
      *cursor = '/';
      return false;
    }
    *cursor = '/';
  }
  return true;
}

static bool laghu_test_remove_tree(const char *path) {
  DIR *directory;
  struct dirent *entry;
  struct stat status;
  char child[LAGHU_RUNTIME_PATH_SIZE];
  directory = opendir(path);
  if (directory == NULL) return errno == ENOENT;
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) < 0 ||
        strlen(child) >= sizeof(child) || lstat(child, &status) != 0 ||
        (S_ISDIR(status.st_mode) ? !laghu_test_remove_tree(child)
                                 : unlink(child) != 0)) {
      (void)closedir(directory);
      return false;
    }
  }
  if (closedir(directory) != 0) return false;
  return rmdir(path) == 0 || errno == ENOENT;
}

bool laghu_test_directory(char *path, size_t capacity) {
  static const char pattern[] = "/tmp/laghu-test-XXXXXX";
  if (capacity < sizeof(pattern)) return false;
  memcpy(path, pattern, sizeof(pattern));
  return mkdtemp(path) != NULL;
}

bool laghu_test_workspace_create(laghu_test_workspace *workspace) {
  if (workspace == NULL) return false;
  memset(workspace, 0, sizeof(*workspace));
  if (!laghu_test_directory(workspace->path, sizeof(workspace->path)))
    return false;
  workspace->created = true;
  return true;
}

bool laghu_test_workspace_remove(laghu_test_workspace *workspace) {
  bool removed;
  if (workspace == NULL) return false;
  if (!workspace->created) return true;
  removed = laghu_test_remove_tree(workspace->path);
  if (removed) {
    workspace->path[0] = '\0';
    workspace->created = false;
  }
  return removed;
}

bool laghu_test_workspace_path(const laghu_test_workspace *workspace,
                               const char *relative, char *path,
                               size_t capacity) {
  return workspace != NULL && workspace->created &&
         laghu_test_join(workspace->path, relative, path, capacity);
}

bool laghu_test_workspace_write(const laghu_test_workspace *workspace,
                                const char *relative, const unsigned char *data,
                                size_t length) {
  char path[LAGHU_RUNTIME_PATH_SIZE];
  FILE *file;
  bool written;
  if ((data == NULL && length != 0U) ||
      !laghu_test_workspace_path(workspace, relative, path, sizeof(path)) ||
      !laghu_test_make_parents(path))
    return false;
  file = fopen(path, "wb");
  if (file == NULL) return false;
  written = length == 0U || fwrite(data, 1U, length, file) == length;
  if (fclose(file) != 0) written = false;
  return written;
}

bool laghu_test_cache_uri(const char *path, char *uri, size_t capacity) {
  char normalized[LAGHU_RUNTIME_PATH_SIZE];
  static const char hexadecimal[] = "0123456789ABCDEF";
  size_t length, index, used;
  if (path == NULL || uri == NULL || capacity == 0U || path[0] == '\0')
    return false;
  length = strlen(path);
  if (length >= sizeof(normalized)) return false;
  for (index = 0U; index < length; ++index) normalized[index] = path[index];
  normalized[length] = '\0';
  if (normalized[0] != '/') return false;
  if (capacity < sizeof("file://")) return false;
  memcpy(uri, "file://", sizeof("file://") - 1U);
  used = sizeof("file://") - 1U;
  for (index = 0U; index < length; ++index) {
    unsigned char value = (unsigned char)normalized[index];
    if (value == '%' || value == '?' || value == '#' || value == ' ') {
      if (used > capacity - 4U) return false;
      uri[used++] = '%';
      uri[used++] = hexadecimal[value >> 4U];
      uri[used++] = hexadecimal[value & 0x0fU];
    } else {
      if (value < 0x21U || used + 1U >= capacity) return false;
      uri[used++] = (char)value;
    }
  }
  uri[used] = '\0';
  return true;
}

bool laghu_test_queue_pair_open(laghu_test_queue_pair *pair,
                                const laghu_test_workspace *workspace,
                                const char *relative, unsigned int slots,
                                size_t payload_capacity) {
  if (pair == NULL) return false;
  memset(pair, 0, sizeof(*pair));
  if (!laghu_test_workspace_path(workspace, relative, pair->path,
                                 sizeof(pair->path)) ||
      !laghu_test_make_parents(pair->path))
    return false;
  laghu_runtime_queue_init(&pair->producer);
  laghu_runtime_queue_init(&pair->consumer);
  if (!laghu_runtime_queue_create(&pair->producer, pair->path, slots,
                                  payload_capacity))
    return false;
  pair->producer_open = true;
  if (!laghu_runtime_queue_open(&pair->consumer, pair->path)) {
    laghu_test_queue_pair_close(pair);
    return false;
  }
  pair->consumer_open = true;
  return true;
}

void laghu_test_queue_pair_close(laghu_test_queue_pair *pair) {
  if (pair == NULL) return;
  if (pair->consumer_open) laghu_runtime_queue_close(&pair->consumer);
  if (pair->producer_open) laghu_runtime_queue_close(&pair->producer);
  memset(pair, 0, sizeof(*pair));
}
