// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/runtime.h"
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#define laghu_catalog_mkdir(path) _mkdir(path)
#define laghu_catalog_pid() _getpid()
#define laghu_catalog_replace(from, to)                            \
  (MoveFileExA((from), (to),                                       \
               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) \
       ? 0                                                         \
       : -1)
typedef HANDLE laghu_catalog_lock;
#define LAGHU_CATALOG_LOCK_INVALID INVALID_HANDLE_VALUE
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define laghu_catalog_mkdir(path) mkdir(path, 0750)
#define laghu_catalog_pid() getpid()
#define laghu_catalog_replace(from, to) rename((from), (to))
typedef int laghu_catalog_lock;
#define LAGHU_CATALOG_LOCK_INVALID (-1)
#endif

typedef struct {
  uint64_t magic;
  laghu_catalog_record record;
  char checksum[LAGHU_RUNTIME_KEY_SIZE];
} laghu_catalog_file;

#define LAGHU_CATALOG_MAGIC UINT64_C(0x4c41474855434154)

typedef struct {
  char path[LAGHU_RUNTIME_PATH_SIZE];
  char identity[LAGHU_RUNTIME_KEY_SIZE];
  uint64_t accessed;
} laghu_catalog_prune_item;

static int laghu_catalog_prune_compare(const void *left, const void *right) {
  const laghu_catalog_prune_item *a = left;
  const laghu_catalog_prune_item *b = right;
  return a->accessed < b->accessed ? -1 : a->accessed > b->accessed ? 1 : 0;
}

static bool laghu_catalog_hash_valid(const char *hash) {
  size_t index;
  if (hash == NULL || strlen(hash) != LAGHU_SHA256_HEX_LENGTH) {
    return false;
  }
  for (index = 0U; index < LAGHU_SHA256_HEX_LENGTH; ++index) {
    if (!((hash[index] >= '0' && hash[index] <= '9') ||
          (hash[index] >= 'a' && hash[index] <= 'f'))) {
      return false;
    }
  }
  return true;
}

static bool laghu_catalog_paths(const char *cache_path, const char *key,
                                char *directory, size_t directory_size,
                                char *path, size_t path_size) {
  int first;
  int second;
  if (cache_path == NULL || !laghu_catalog_hash_valid(key)) {
    return false;
  }
  first = snprintf(directory, directory_size, "%s/catalog", cache_path);
  second = snprintf(path, path_size, "%s/%s.meta", directory, key);
  return first > 0 && (size_t)first < directory_size && second > 0 &&
         (size_t)second < path_size;
}

static laghu_catalog_lock laghu_catalog_try_lock(const char *path) {
#ifdef _WIN32
  return CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                     FILE_ATTRIBUTE_TEMPORARY, NULL);
#else
  return open(path, O_WRONLY | O_CREAT | O_EXCL, 0640);
#endif
}

static void laghu_catalog_unlock(laghu_catalog_lock lock, const char *path) {
#ifdef _WIN32
  (void)CloseHandle(lock);
#else
  (void)close(lock);
#endif
  (void)remove(path);
}

static bool laghu_catalog_url_key(const char *normalized_url,
                                  const char *policy_key,
                                  uint32_t capability_mask,
                                  char output[LAGHU_RUNTIME_KEY_SIZE]) {
  char material[LAGHU_RUNTIME_PATH_SIZE + LAGHU_RUNTIME_KEY_SIZE + 32U];
  int length;
  if (normalized_url == NULL || normalized_url[0] != '/' ||
      !laghu_catalog_hash_valid(policy_key)) {
    return false;
  }
  length = snprintf(material, sizeof(material), "catalog-url-v1\n%s\n%s\n%u",
                    normalized_url, policy_key, capability_mask);
  return length > 0 && (size_t)length < sizeof(material) &&
         laghu_sha256_hex(
             (laghu_buffer){(const unsigned char *)material, (size_t)length},
             output);
}

bool laghu_catalog_key(const char *normalized_url, const char *source_hash,
                       const char *policy_key, uint32_t capability_mask,
                       char output[LAGHU_RUNTIME_KEY_SIZE]) {
  char material[LAGHU_RUNTIME_PATH_SIZE + LAGHU_RUNTIME_KEY_SIZE * 2U + 32U];
  int length;
  if (normalized_url == NULL || normalized_url[0] != '/' ||
      !laghu_catalog_hash_valid(source_hash) ||
      !laghu_catalog_hash_valid(policy_key) || output == NULL) {
    return false;
  }
  length = snprintf(material, sizeof(material), "catalog-v1\n%s\n%s\n%s\n%u",
                    normalized_url, source_hash, policy_key, capability_mask);
  return length > 0 && (size_t)length < sizeof(material) &&
         laghu_sha256_hex(
             (laghu_buffer){(const unsigned char *)material, (size_t)length},
             output);
}

bool laghu_catalog_publish(const char *cache_path, const char *catalog_key,
                           const laghu_catalog_record *record) {
  char directory[LAGHU_RUNTIME_PATH_SIZE];
  char path[LAGHU_RUNTIME_PATH_SIZE];
  char temporary[LAGHU_RUNTIME_PATH_SIZE];
  char lock_path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_catalog_file stored = {0};
  FILE *file;
  bool written;
  int length;
  laghu_catalog_lock lock;
  if (record == NULL || record->version != LAGHU_CATALOG_VERSION ||
      record->variant_count > LAGHU_CATALOG_MAX_WIDTHS ||
      !laghu_catalog_paths(cache_path, catalog_key, directory,
                           sizeof(directory), path, sizeof(path))) {
    return false;
  }
  if (laghu_catalog_mkdir(cache_path) != 0 && errno != EEXIST) {
    return false;
  }
  if (laghu_catalog_mkdir(directory) != 0 && errno != EEXIST) {
    return false;
  }
  length = snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
  if (length <= 0 || (size_t)length >= sizeof(lock_path)) {
    return false;
  }
  lock = laghu_catalog_try_lock(lock_path);
  if (lock == LAGHU_CATALOG_LOCK_INVALID) {
    return false;
  }
  length = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld.%llu", path,
                    (long)laghu_catalog_pid(),
                    (unsigned long long)record->updated_at);
  if (length <= 0 || (size_t)length >= sizeof(temporary)) {
    laghu_catalog_unlock(lock, lock_path);
    return false;
  }
  stored.magic = LAGHU_CATALOG_MAGIC;
  stored.record = *record;
  if (!laghu_sha256_hex((laghu_buffer){(const unsigned char *)&stored.record,
                                       sizeof(stored.record)},
                        stored.checksum)) {
    laghu_catalog_unlock(lock, lock_path);
    return false;
  }
  file = fopen(temporary, "wb");
  if (file == NULL) {
    laghu_catalog_unlock(lock, lock_path);
    return false;
  }
  written =
      fwrite(&stored, sizeof(stored), 1U, file) == 1U && fflush(file) == 0;
  if (fclose(file) != 0) {
    written = false;
  }
  if (!written || laghu_catalog_replace(temporary, path) != 0) {
    (void)remove(temporary);
    laghu_catalog_unlock(lock, lock_path);
    return false;
  }
  laghu_catalog_unlock(lock, lock_path);
  {
    char url_key[LAGHU_RUNTIME_KEY_SIZE];
    if (laghu_catalog_url_key(record->normalized_url, record->policy_key,
                              record->capability_mask, url_key) &&
        strcmp(url_key, catalog_key) != 0) {
      return laghu_catalog_publish(cache_path, url_key, record);
    }
  }
  return true;
}

bool laghu_catalog_lookup(const char *cache_path, const char *catalog_key,
                          uint64_t now, unsigned int ttl_seconds,
                          laghu_catalog_record *record) {
  char directory[LAGHU_RUNTIME_PATH_SIZE];
  char path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_catalog_file stored;
  char checksum[LAGHU_RUNTIME_KEY_SIZE];
  FILE *file;
  bool read;
  if (record == NULL || ttl_seconds == 0U ||
      !laghu_catalog_paths(cache_path, catalog_key, directory,
                           sizeof(directory), path, sizeof(path))) {
    return false;
  }
  file = fopen(path, "rb");
  if (file == NULL) {
    return false;
  }
  read = fread(&stored, sizeof(stored), 1U, file) == 1U;
  if (fclose(file) != 0) {
    read = false;
  }
  if (!read || stored.magic != LAGHU_CATALOG_MAGIC ||
      stored.record.version != LAGHU_CATALOG_VERSION ||
      stored.record.variant_count > LAGHU_CATALOG_MAX_WIDTHS ||
      stored.record.updated_at > now ||
      now - stored.record.updated_at > ttl_seconds ||
      !laghu_sha256_hex((laghu_buffer){(const unsigned char *)&stored.record,
                                       sizeof(stored.record)},
                        checksum) ||
      strcmp(checksum, stored.checksum) != 0) {
    (void)remove(path);
    return false;
  }
  *record = stored.record;
  return true;
}

bool laghu_catalog_lookup_url(const char *cache_path,
                              const char *normalized_url,
                              const char *policy_key, uint32_t capability_mask,
                              uint64_t now, unsigned int ttl_seconds,
                              laghu_catalog_record *record) {
  char key[LAGHU_RUNTIME_KEY_SIZE];
  return laghu_catalog_url_key(normalized_url, policy_key, capability_mask,
                               key) &&
         laghu_catalog_lookup(cache_path, key, now, ttl_seconds, record);
}

bool laghu_catalog_publish_url(const char *cache_path,
                               const laghu_catalog_record *record) {
  char key[LAGHU_RUNTIME_KEY_SIZE];
  return record != NULL &&
         laghu_catalog_url_key(record->normalized_url, record->policy_key,
                               record->capability_mask, key) &&
         laghu_catalog_publish(cache_path, key, record);
}

static bool laghu_catalog_prune_add(const char *path, uint64_t now,
                                    unsigned int ttl_seconds,
                                    laghu_catalog_prune_item **items,
                                    size_t *count, size_t *capacity) {
  laghu_catalog_file stored;
  laghu_catalog_prune_item *grown;
  FILE *file = fopen(path, "rb");
  bool read;
  if (file == NULL) {
    return true;
  }
  read = fread(&stored, sizeof(stored), 1U, file) == 1U;
  (void)fclose(file);
  if (!read || stored.magic != LAGHU_CATALOG_MAGIC ||
      stored.record.updated_at > now ||
      now - stored.record.updated_at > ttl_seconds) {
    (void)remove(path);
    return true;
  }
  if (*count == *capacity) {
    size_t next = *capacity == 0U ? 64U : *capacity * 2U;
    grown = realloc(*items, next * sizeof(**items));
    if (grown == NULL) {
      return false;
    }
    *items = grown;
    *capacity = next;
  }
  if (snprintf((*items)[*count].path, sizeof((*items)[*count].path), "%s",
               path) <= 0) {
    return false;
  }
  (*items)[*count].accessed = stored.record.last_accessed_at;
  if (!laghu_catalog_url_key(
          stored.record.normalized_url, stored.record.policy_key,
          stored.record.capability_mask, (*items)[*count].identity)) {
    (void)remove(path);
    return true;
  }
  ++*count;
  return true;
}

bool laghu_catalog_prune(const char *cache_path, uint64_t now,
                         unsigned int metadata_limit,
                         unsigned int ttl_seconds) {
  char directory[LAGHU_RUNTIME_PATH_SIZE];
  laghu_catalog_prune_item *items = NULL;
  size_t count = 0U;
  size_t capacity = 0U;
  size_t index;
  char (*kept)[LAGHU_RUNTIME_KEY_SIZE] = NULL;
  size_t kept_count = 0U;
  int length;
  bool success = true;
  if (cache_path == NULL || metadata_limit == 0U || ttl_seconds == 0U) {
    return false;
  }
  length = snprintf(directory, sizeof(directory), "%s/catalog", cache_path);
  if (length <= 0 || (size_t)length >= sizeof(directory)) {
    return false;
  }
#ifdef _WIN32
  {
    WIN32_FIND_DATAA data;
    char pattern[LAGHU_RUNTIME_PATH_SIZE];
    HANDLE search;
    if (snprintf(pattern, sizeof(pattern), "%s/*.meta", directory) <= 0) {
      return false;
    }
    search = FindFirstFileA(pattern, &data);
    if (search == INVALID_HANDLE_VALUE) {
      return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    do {
      char path[LAGHU_RUNTIME_PATH_SIZE];
      if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
          snprintf(path, sizeof(path), "%s/%s", directory, data.cFileName) >
              0 &&
          !laghu_catalog_prune_add(path, now, ttl_seconds, &items, &count,
                                   &capacity)) {
        success = false;
        break;
      }
    } while (FindNextFileA(search, &data));
    (void)FindClose(search);
  }
#else
  {
    DIR *stream = opendir(directory);
    struct dirent *entry;
    if (stream == NULL) {
      return errno == ENOENT;
    }
    while ((entry = readdir(stream)) != NULL) {
      char path[LAGHU_RUNTIME_PATH_SIZE];
      size_t name_length = strlen(entry->d_name);
      if (name_length < 5U ||
          strcmp(entry->d_name + name_length - 5U, ".meta") != 0) {
        continue;
      }
      if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) <=
              0 ||
          !laghu_catalog_prune_add(path, now, ttl_seconds, &items, &count,
                                   &capacity)) {
        success = false;
        break;
      }
    }
    (void)closedir(stream);
  }
#endif
  if (!success) {
    free(items);
    return false;
  }
  if (count > metadata_limit) {
    qsort(items, count, sizeof(*items), laghu_catalog_prune_compare);
    kept = calloc(metadata_limit, sizeof(*kept));
    if (kept == NULL) {
      free(items);
      return false;
    }
    for (index = count; index > 0U; --index) {
      size_t kept_index;
      bool found = false;
      for (kept_index = 0U; kept_index < kept_count; ++kept_index) {
        if (strcmp(kept[kept_index], items[index - 1U].identity) == 0) {
          found = true;
          break;
        }
      }
      if (!found && kept_count < metadata_limit) {
        memcpy(kept[kept_count++], items[index - 1U].identity,
               LAGHU_RUNTIME_KEY_SIZE);
        found = true;
      }
      if (!found) {
        (void)remove(items[index - 1U].path);
      }
    }
  }
  free(kept);
  free(items);
  return true;
}
