// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "runtime_platform.h"

static bool laghu_runtime_wide(const char *input, wchar_t *output,
                               size_t capacity) {
  int length;
  if (input == NULL || output == NULL || capacity == 0U) return false;
  length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, output,
                               (int)capacity);
  return length > 0 && (size_t)length <= capacity;
}

static bool laghu_runtime_open(const char *path, DWORD access, DWORD creation,
                               HANDLE *file) {
  wchar_t wide[4096U];
  if (!laghu_runtime_wide(path, wide, sizeof(wide) / sizeof(wide[0])))
    return false;
  *file = CreateFileW(wide, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                      creation, FILE_ATTRIBUTE_NORMAL, NULL);
  return *file != INVALID_HANDLE_VALUE;
}

static bool laghu_runtime_lock(HANDLE file) {
  OVERLAPPED overlapped;
  memset(&overlapped, 0, sizeof(overlapped));
  return LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0U, MAXDWORD, MAXDWORD, &overlapped) != 0;
}

static void laghu_runtime_unlock(HANDLE file) {
  OVERLAPPED overlapped;
  memset(&overlapped, 0, sizeof(overlapped));
  (void)UnlockFileEx(file, 0U, MAXDWORD, MAXDWORD, &overlapped);
}

static bool laghu_runtime_read_all(HANDLE file, unsigned char *data,
                                   size_t length) {
  while (length != 0U) {
    DWORD count = 0U;
    DWORD request = length > MAXDWORD ? MAXDWORD : (DWORD)length;
    if (!ReadFile(file, data, request, &count, NULL) || count == 0U)
      return false;
    data += count;
    length -= count;
  }
  return true;
}

static bool laghu_runtime_write_all(HANDLE file, const unsigned char *data,
                                    size_t length) {
  while (length != 0U) {
    DWORD count = 0U;
    DWORD request = length > MAXDWORD ? MAXDWORD : (DWORD)length;
    if (!WriteFile(file, data, request, &count, NULL) || count == 0U)
      return false;
    data += count;
    length -= count;
  }
  return true;
}

void laghu_runtime_shared_mapping_init(laghu_runtime_shared_mapping *mapping) {
  if (mapping == NULL) return;
  memset(mapping, 0, sizeof(*mapping));
  mapping->platform_file = (intptr_t)INVALID_HANDLE_VALUE;
}

bool laghu_runtime_directory_ensure(const char *path) {
  wchar_t wide[4096U];
  DWORD attributes;
  if (!laghu_runtime_wide(path, wide, sizeof(wide) / sizeof(wide[0])))
    return false;
  if (CreateDirectoryW(wide, NULL)) return true;
  if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
  attributes = GetFileAttributesW(wide);
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
}

bool laghu_runtime_directory_exists(const char *path) {
  wchar_t wide[4096U];
  DWORD attributes;
  if (!laghu_runtime_wide(path, wide, sizeof(wide) / sizeof(wide[0])))
    return false;
  attributes = GetFileAttributesW(wide);
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
}

bool laghu_runtime_file_remove(const char *path) {
  wchar_t wide[4096U];
  return laghu_runtime_wide(path, wide, sizeof(wide) / sizeof(wide[0])) &&
         (DeleteFileW(wide) || GetLastError() == ERROR_FILE_NOT_FOUND);
}

bool laghu_runtime_file_size(const char *path, uint64_t *size) {
  wchar_t wide[4096U];
  WIN32_FILE_ATTRIBUTE_DATA attributes;
  if (size == NULL ||
      !laghu_runtime_wide(path, wide, sizeof(wide) / sizeof(wide[0])) ||
      !GetFileAttributesExW(wide, GetFileExInfoStandard, &attributes))
    return false;
  *size = ((uint64_t)attributes.nFileSizeHigh << 32U) | attributes.nFileSizeLow;
  return true;
}

bool laghu_runtime_file_read_exact(const char *path, unsigned char *data,
                                   size_t length) {
  HANDLE file;
  bool success;
  if (data == NULL ||
      !laghu_runtime_open(path, GENERIC_READ, OPEN_EXISTING, &file))
    return false;
  success = laghu_runtime_read_all(file, data, length);
  if (CloseHandle(file) == 0) success = false;
  return success;
}

bool laghu_runtime_file_write_atomic(const char *path,
                                     const unsigned char *data, size_t length) {
  char temporary[4096U];
  wchar_t wide_temporary[4096U] = {0}, wide_target[4096U] = {0};
  HANDLE file;
  int written;
  bool success;
  if (path == NULL || data == NULL) return false;
  written = snprintf(temporary, sizeof(temporary), "%s.tmp.%lu", path,
                     (unsigned long)GetCurrentProcessId());
  if (written <= 0 || (size_t)written >= sizeof(temporary) ||
      !laghu_runtime_open(temporary, GENERIC_WRITE, CREATE_ALWAYS, &file))
    return false;
  success =
      laghu_runtime_write_all(file, data, length) && FlushFileBuffers(file);
  if (CloseHandle(file) == 0) success = false;
  file = INVALID_HANDLE_VALUE;
  success =
      success &&
      laghu_runtime_wide(temporary, wide_temporary,
                         sizeof(wide_temporary) / sizeof(wide_temporary[0])) &&
      laghu_runtime_wide(path, wide_target,
                         sizeof(wide_target) / sizeof(wide_target[0])) &&
      MoveFileExW(wide_temporary, wide_target,
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  if (!success) {
    if (file != INVALID_HANDLE_VALUE) (void)CloseHandle(file);
    (void)DeleteFileW(wide_temporary);
  }
  return success;
}

bool laghu_runtime_shared_mapping_open(laghu_runtime_shared_mapping *mapping,
                                       const char *path, size_t size) {
  HANDLE file = INVALID_HANDLE_VALUE, object = NULL;
  LARGE_INTEGER current, target;
  void *address;
  if (mapping == NULL || path == NULL || size < 4096U) return false;
  laghu_runtime_shared_mapping_init(mapping);
  if (!laghu_runtime_open(path, GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS,
                          &file) ||
      !laghu_runtime_lock(file) || !GetFileSizeEx(file, &current) ||
      (current.QuadPart != 0 && current.QuadPart != (LONGLONG)size)) {
    if (file != INVALID_HANDLE_VALUE) {
      laghu_runtime_unlock(file);
      (void)CloseHandle(file);
    }
    return false;
  }
  target.QuadPart = (LONGLONG)size;
  if (current.QuadPart == 0 &&
      (!SetFilePointerEx(file, target, NULL, FILE_BEGIN) ||
       !SetEndOfFile(file))) {
    laghu_runtime_unlock(file);
    (void)CloseHandle(file);
    return false;
  }
  object =
      CreateFileMappingW(file, NULL, PAGE_READWRITE,
                         (DWORD)((uint64_t)size >> 32U), (DWORD)size, NULL);
  address = object != NULL
                ? MapViewOfFile(object, FILE_MAP_ALL_ACCESS, 0, 0, size)
                : NULL;
  laghu_runtime_unlock(file);
  if (address == NULL) {
    if (object != NULL) (void)CloseHandle(object);
    (void)CloseHandle(file);
    return false;
  }
  mapping->platform_file = (intptr_t)(uintptr_t)file;
  mapping->platform_mapping = (intptr_t)(uintptr_t)object;
  mapping->mapping = address;
  mapping->mapping_length = size;
  return true;
}

bool laghu_runtime_shared_mapping_try_lock(
    laghu_runtime_shared_mapping *mapping) {
  return mapping != NULL && mapping->mapping != NULL &&
         laghu_runtime_lock((HANDLE)(uintptr_t)mapping->platform_file);
}

void laghu_runtime_shared_mapping_unlock(
    laghu_runtime_shared_mapping *mapping) {
  if (mapping != NULL && mapping->mapping != NULL)
    laghu_runtime_unlock((HANDLE)(uintptr_t)mapping->platform_file);
}

bool laghu_runtime_shared_mapping_sync(laghu_runtime_shared_mapping *mapping) {
  return mapping != NULL && mapping->mapping != NULL &&
         FlushViewOfFile(mapping->mapping, mapping->mapping_length) != 0;
}

void laghu_runtime_shared_mapping_close(laghu_runtime_shared_mapping *mapping) {
  if (mapping == NULL) return;
  if (mapping->mapping != NULL) (void)UnmapViewOfFile(mapping->mapping);
  if (mapping->platform_mapping != 0)
    (void)CloseHandle((HANDLE)(uintptr_t)mapping->platform_mapping);
  if ((HANDLE)(uintptr_t)mapping->platform_file != INVALID_HANDLE_VALUE)
    (void)CloseHandle((HANDLE)(uintptr_t)mapping->platform_file);
  laghu_runtime_shared_mapping_init(mapping);
}
