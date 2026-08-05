// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef _WIN32
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

bool laghu_test_directory(char *path, size_t capacity) {
#ifdef _WIN32
  char base[MAX_PATH];
  char unique[MAX_PATH];
  if (GetTempPathA(sizeof(base), base) == 0U ||
      GetTempFileNameA(base, "lgt", 0U, unique) == 0U || !DeleteFileA(unique) ||
      strlen(unique) >= capacity)
    return false;
  memcpy(path, unique, strlen(unique) + 1U);
  return _mkdir(path) == 0;
#else
  static const char pattern[] = "/tmp/laghu-test-XXXXXX";
  if (capacity < sizeof(pattern)) return false;
  memcpy(path, pattern, sizeof(pattern));
  return mkdtemp(path) != NULL;
#endif
}
