// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#define WIN32_LEAN_AND_MEAN
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "laghu/runtime.h"

#define LAGHU_QUEUE_MAGIC UINT64_C(0x4c41474855515545)
#define LAGHU_CACHE_MAGIC UINT64_C(0x4c41474855434143)
#define LAGHU_CACHE_VERSION 2U
#define LAGHU_SLOT_EMPTY 0U
#define LAGHU_SLOT_READY 1U

typedef struct {
  uint64_t magic;
  uint32_t version;
  uint32_t slot_count;
  uint64_t slot_payload_size;
  uint32_t next_write;
  uint32_t next_read;
  uint32_t capabilities;
  uint32_t reserved;
  uint64_t worker_heartbeat;
  char backend_id[LAGHU_RUNTIME_BACKEND_SIZE];
} laghu_queue_header;

typedef struct {
  uint32_t state;
  uint32_t kind;
  uint64_t payload_length;
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char request_path[LAGHU_RUNTIME_PATH_SIZE];
  char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  char content_type[LAGHU_RUNTIME_TYPE_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  char provider_id[LAGHU_FONT_PROVIDER_ID_SIZE];
  char provider_digest[LAGHU_RUNTIME_KEY_SIZE];
  char javascript_target[LAGHU_JAVASCRIPT_TARGET_SIZE];
  uint64_t filters;
  uint32_t quality;
  uint32_t metadata_limit;
  uint32_t metadata_ttl;
  uint32_t target_count;
  uint32_t target_width[LAGHU_RUNTIME_MAX_TARGETS];
  uint32_t target_height[LAGHU_RUNTIME_MAX_TARGETS];
  uint64_t resize_filter[LAGHU_RUNTIME_MAX_TARGETS];
  uint32_t sprite_count;
  char sprite_variant_keys[LAGHU_RUNTIME_MAX_SPRITE_INPUTS]
                          [LAGHU_RUNTIME_KEY_SIZE];
  uint32_t sprite_width[LAGHU_RUNTIME_MAX_SPRITE_INPUTS];
  uint32_t sprite_height[LAGHU_RUNTIME_MAX_SPRITE_INPUTS];
  uint8_t allow_lossy;
  uint8_t accept_webp;
  uint8_t padding[6];
} laghu_queue_slot;

typedef struct {
  uint64_t magic;
  uint32_t version;
  uint32_t reserved;
  uint64_t length;
  char variant_key[LAGHU_RUNTIME_KEY_SIZE];
  char payload_hash[LAGHU_RUNTIME_KEY_SIZE];
  char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  char content_type[LAGHU_RUNTIME_TYPE_SIZE];
  char backend_id[LAGHU_RUNTIME_BACKEND_SIZE];
} laghu_cache_metadata;

_Static_assert(sizeof(laghu_queue_header) == 176U,
               "queue header wire layout changed");
_Static_assert(sizeof(laghu_queue_slot) == 4544U,
               "queue slot wire layout changed");
_Static_assert(sizeof(laghu_cache_metadata) == 608U,
               "cache metadata wire layout changed");

static HANDLE laghu_file(const laghu_runtime_queue *queue) {
  return (HANDLE)(uintptr_t)queue->platform_file;
}

static HANDLE laghu_mapping(const laghu_runtime_queue *queue) {
  return (HANDLE)(uintptr_t)queue->platform_mapping;
}

static bool laghu_wide(const char *input, wchar_t *output, size_t capacity) {
  int count;
  if (input == NULL || output == NULL || capacity > INT_MAX) {
    return false;
  }
  count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, output,
                              (int)capacity);
  return count > 0;
}

static bool laghu_copy(char *target, size_t capacity, const char *source) {
  size_t length;
  if (target == NULL || source == NULL || capacity == 0U) {
    return false;
  }
  length = strlen(source);
  if (length >= capacity) {
    return false;
  }
  memcpy(target, source, length + 1U);
  return true;
}

static bool laghu_hash_valid(const char *value) {
  size_t index;
  if (value == NULL || value[LAGHU_RUNTIME_KEY_SIZE - 1U] != '\0') {
    return false;
  }
  for (index = 0U; index + 1U < LAGHU_RUNTIME_KEY_SIZE; ++index) {
    if (!((value[index] >= '0' && value[index] <= '9') ||
          (value[index] >= 'a' && value[index] <= 'f'))) {
      return false;
    }
  }
  return true;
}

static bool laghu_sprite_job_valid(const laghu_runtime_job *job) {
  unsigned int index;
  if (job->kind != LAGHU_RUNTIME_JOB_SPRITE) {
    return job->sprite_count == 0U;
  }
  if (job->sprite_count < 2U ||
      job->sprite_count > LAGHU_RUNTIME_MAX_SPRITE_INPUTS ||
      job->payload.length != 0U) {
    return false;
  }
  for (index = 0U; index < job->sprite_count; ++index) {
    if (!laghu_hash_valid(job->sprite_variant_keys[index])) {
      return false;
    }
  }
  return true;
}

static bool laghu_font_job_valid(const laghu_runtime_job *job) {
  if (job->kind != LAGHU_RUNTIME_JOB_FONT_CSS)
    return job->provider_id[0] == '\0' && job->provider_digest[0] == '\0';
  return job->payload.length == 0U && job->request_path[0] != '\0' &&
         job->provider_id[0] != '\0' &&
         memchr(job->provider_id, '\0', sizeof(job->provider_id)) != NULL &&
         laghu_hash_valid(job->provider_digest);
}

static bool laghu_javascript_job_valid(const laghu_runtime_job *job) {
  if (job->kind != LAGHU_RUNTIME_JOB_JAVASCRIPT)
    return job->javascript_target[0] == '\0';
  return job->payload.length > 0U &&
         job->payload.length <= LAGHU_JAVASCRIPT_MAX_BYTES &&
         job->javascript_target[0] != '\0' &&
         memchr(job->javascript_target, '\0', sizeof(job->javascript_target)) !=
             NULL;
}

static size_t laghu_slot_size(size_t payload_size) {
  return sizeof(laghu_queue_slot) + payload_size;
}

static laghu_queue_header *laghu_header(laghu_runtime_queue *queue) {
  return (laghu_queue_header *)queue->mapping;
}

static laghu_queue_slot *laghu_slot(laghu_runtime_queue *queue,
                                    unsigned int index) {
  unsigned char *base = (unsigned char *)queue->mapping;
  return (laghu_queue_slot *)(void *)(base + sizeof(laghu_queue_header) +
                                      index * laghu_slot_size(
                                                  queue->slot_payload_size));
}

static unsigned char *laghu_payload(laghu_queue_slot *slot) {
  return (unsigned char *)(void *)(slot + 1);
}

static bool laghu_lock(HANDLE file) {
  OVERLAPPED overlap = {0};
  return LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 1, 0, &overlap) != 0;
}

static void laghu_unlock(HANDLE file) {
  OVERLAPPED overlap = {0};
  (void)UnlockFileEx(file, 0, 1, 0, &overlap);
}

static bool laghu_write(HANDLE file, const void *data, size_t length) {
  const unsigned char *cursor = (const unsigned char *)data;
  while (length > 0U) {
    DWORD chunk = length > MAXDWORD ? MAXDWORD : (DWORD)length;
    DWORD written = 0;
    if (!WriteFile(file, cursor, chunk, &written, NULL) || written == 0U) {
      return false;
    }
    cursor += written;
    length -= written;
  }
  return true;
}

static bool laghu_read(HANDLE file, void *data, size_t length) {
  unsigned char *cursor = (unsigned char *)data;
  while (length > 0U) {
    DWORD chunk = length > MAXDWORD ? MAXDWORD : (DWORD)length;
    DWORD count = 0;
    if (!ReadFile(file, cursor, chunk, &count, NULL) || count == 0U) {
      return false;
    }
    cursor += count;
    length -= count;
  }
  return true;
}

static bool laghu_path(char output[LAGHU_RUNTIME_PATH_SIZE], const char *root,
                       const char *prefix, const char *name,
                       const char *suffix) {
  int count;
  if (output == NULL || root == NULL || prefix == NULL || name == NULL ||
      suffix == NULL) {
    return false;
  }
  count = snprintf(output, LAGHU_RUNTIME_PATH_SIZE, "%s/%s%s%s", root, prefix,
                   name, suffix);
  return count > 0 && (size_t)count < LAGHU_RUNTIME_PATH_SIZE;
}

static bool laghu_open(const char *path, DWORD access, DWORD creation,
                       HANDLE *file) {
  wchar_t wide[LAGHU_RUNTIME_PATH_SIZE];
  if (!laghu_wide(path, wide, LAGHU_RUNTIME_PATH_SIZE)) {
    return false;
  }
  *file = CreateFileW(wide, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                      creation, FILE_ATTRIBUTE_NORMAL, NULL);
  return *file != INVALID_HANDLE_VALUE;
}

void laghu_runtime_queue_init(laghu_runtime_queue *queue) {
  if (queue == NULL) {
    return;
  }
  memset(queue, 0, sizeof(*queue));
  queue->platform_file = (intptr_t)INVALID_HANDLE_VALUE;
}

bool laghu_runtime_queue_create(laghu_runtime_queue *queue, const char *path,
                                unsigned int slot_count,
                                size_t slot_payload_size) {
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE mapping;
  LARGE_INTEGER size;
  void *view;
  size_t length;
  laghu_queue_header *header;
  if (queue == NULL || path == NULL || slot_count == 0U ||
      slot_payload_size == 0U ||
      slot_count >
          (SIZE_MAX - sizeof(*header)) / laghu_slot_size(slot_payload_size)) {
    return false;
  }
  laghu_runtime_queue_close(queue);
  length = sizeof(*header) + slot_count * laghu_slot_size(slot_payload_size);
  if (!laghu_open(path, GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS, &file)) {
    return false;
  }
  size.QuadPart = (LONGLONG)length;
  if (!SetFilePointerEx(file, size, NULL, FILE_BEGIN) || !SetEndOfFile(file)) {
    CloseHandle(file);
    return false;
  }
  mapping = CreateFileMappingW(file, NULL, PAGE_READWRITE, size.HighPart,
                               size.LowPart, NULL);
  if (mapping == NULL) {
    CloseHandle(file);
    return false;
  }
  view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, length);
  if (view == NULL) {
    CloseHandle(mapping);
    CloseHandle(file);
    return false;
  }
  memset(view, 0, length);
  header = (laghu_queue_header *)view;
  header->magic = LAGHU_QUEUE_MAGIC;
  header->version = LAGHU_QUEUE_VERSION;
  header->slot_count = slot_count;
  header->slot_payload_size = slot_payload_size;
  queue->platform_file = (intptr_t)(uintptr_t)file;
  queue->platform_mapping = (intptr_t)(uintptr_t)mapping;
  queue->mapping = view;
  queue->mapping_length = length;
  queue->slot_count = slot_count;
  queue->slot_payload_size = slot_payload_size;
  return FlushViewOfFile(view, sizeof(*header)) != 0;
}

bool laghu_runtime_queue_open(laghu_runtime_queue *queue, const char *path) {
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE mapping;
  LARGE_INTEGER file_size;
  laghu_queue_header header;
  DWORD count;
  size_t expected;
  void *view;
  if (queue == NULL || path == NULL ||
      !laghu_open(path, GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, &file) ||
      !ReadFile(file, &header, sizeof(header), &count, NULL) ||
      count != sizeof(header) || !GetFileSizeEx(file, &file_size) ||
      header.magic != LAGHU_QUEUE_MAGIC ||
      header.version != LAGHU_QUEUE_VERSION || header.slot_count == 0U ||
      header.slot_payload_size == 0U) {
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
    }
    return false;
  }
  expected =
      sizeof(header) +
      header.slot_count * laghu_slot_size((size_t)header.slot_payload_size);
  if (file_size.QuadPart != (LONGLONG)expected) {
    CloseHandle(file);
    return false;
  }
  mapping = CreateFileMappingW(file, NULL, PAGE_READWRITE, 0, 0, NULL);
  view = mapping == NULL
             ? NULL
             : MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, expected);
  if (view == NULL) {
    if (mapping != NULL) {
      CloseHandle(mapping);
    }
    CloseHandle(file);
    return false;
  }
  laghu_runtime_queue_close(queue);
  queue->platform_file = (intptr_t)(uintptr_t)file;
  queue->platform_mapping = (intptr_t)(uintptr_t)mapping;
  queue->mapping = view;
  queue->mapping_length = expected;
  queue->slot_count = header.slot_count;
  queue->slot_payload_size = (size_t)header.slot_payload_size;
  return laghu_runtime_queue_refresh(queue);
}

bool laghu_runtime_queue_refresh(laghu_runtime_queue *queue) {
  laghu_queue_header *header;
  if (queue == NULL || queue->mapping == NULL) {
    return false;
  }
  header = laghu_header(queue);
  if (header->magic != LAGHU_QUEUE_MAGIC ||
      header->version != LAGHU_QUEUE_VERSION) {
    return false;
  }
  queue->capabilities = header->capabilities;
  queue->worker_heartbeat = header->worker_heartbeat;
  memcpy(queue->backend_id, header->backend_id, sizeof(queue->backend_id));
  queue->backend_id[sizeof(queue->backend_id) - 1U] = '\0';
  return true;
}

bool laghu_runtime_queue_set_backend(laghu_runtime_queue *queue,
                                     uint32_t capabilities,
                                     const char *backend_id) {
  laghu_queue_header *header;
  HANDLE file;
  if (queue == NULL || queue->mapping == NULL || backend_id == NULL ||
      strlen(backend_id) >= LAGHU_RUNTIME_BACKEND_SIZE) {
    return false;
  }
  file = laghu_file(queue);
  if (!laghu_lock(file)) {
    return false;
  }
  header = laghu_header(queue);
  header->capabilities = capabilities;
  (void)laghu_copy(header->backend_id, sizeof(header->backend_id), backend_id);
  queue->capabilities = capabilities;
  (void)laghu_copy(queue->backend_id, sizeof(queue->backend_id), backend_id);
  laghu_unlock(file);
  return true;
}

bool laghu_runtime_queue_heartbeat(laghu_runtime_queue *queue,
                                   uint64_t epoch_seconds) {
  HANDLE file;
  if (queue == NULL || queue->mapping == NULL || epoch_seconds == 0U) {
    return false;
  }
  file = laghu_file(queue);
  if (!laghu_lock(file)) {
    return false;
  }
  laghu_header(queue)->worker_heartbeat = epoch_seconds;
  queue->worker_heartbeat = epoch_seconds;
  laghu_unlock(file);
  return true;
}

void laghu_runtime_queue_close(laghu_runtime_queue *queue) {
  if (queue == NULL) {
    return;
  }
  if (queue->mapping != NULL) {
    (void)UnmapViewOfFile(queue->mapping);
  }
  if (queue->platform_mapping != 0) {
    (void)CloseHandle(laghu_mapping(queue));
  }
  if ((HANDLE)(uintptr_t)queue->platform_file != INVALID_HANDLE_VALUE &&
      queue->platform_file != 0) {
    (void)CloseHandle(laghu_file(queue));
  }
  laghu_runtime_queue_init(queue);
}

bool laghu_runtime_queue_try_publish(laghu_runtime_queue *queue,
                                     const laghu_runtime_job *job) {
  HANDLE file;
  laghu_queue_header *header;
  laghu_queue_slot *slot;
  bool success = false;
  if (queue == NULL || job == NULL || queue->mapping == NULL ||
      job->target_count > LAGHU_RUNTIME_MAX_TARGETS ||
      job->sprite_count > LAGHU_RUNTIME_MAX_SPRITE_INPUTS ||
      (job->payload.data == NULL && job->payload.length != 0U) ||
      job->payload.length > queue->slot_payload_size ||
      !laghu_hash_valid(job->index_key) || !laghu_hash_valid(job->policy_key) ||
      !laghu_sprite_job_valid(job) || !laghu_font_job_valid(job) ||
      !laghu_javascript_job_valid(job)) {
    return false;
  }
  file = laghu_file(queue);
  if (!laghu_lock(file)) {
    return false;
  }
  header = laghu_header(queue);
  slot = laghu_slot(queue, header->next_write % queue->slot_count);
  if (slot->state == LAGHU_SLOT_EMPTY) {
    memset(slot, 0, sizeof(*slot));
    slot->payload_length = job->payload.length;
    slot->kind = (uint32_t)job->kind;
    memcpy(slot->index_key, job->index_key, sizeof(slot->index_key));
    memcpy(slot->request_path, job->request_path, sizeof(slot->request_path));
    memcpy(slot->validator, job->validator, sizeof(slot->validator));
    memcpy(slot->content_type, job->content_type, sizeof(slot->content_type));
    memcpy(slot->policy_key, job->policy_key, sizeof(slot->policy_key));
    memcpy(slot->provider_id, job->provider_id, sizeof(slot->provider_id));
    memcpy(slot->provider_digest, job->provider_digest,
           sizeof(slot->provider_digest));
    memcpy(slot->javascript_target, job->javascript_target,
           sizeof(slot->javascript_target));
    slot->filters = job->filters;
    slot->quality = job->quality;
    slot->metadata_limit = job->metadata_limit;
    slot->metadata_ttl = job->metadata_ttl;
    slot->target_count = job->target_count;
    memcpy(slot->target_width, job->target_width, sizeof(slot->target_width));
    memcpy(slot->target_height, job->target_height,
           sizeof(slot->target_height));
    memcpy(slot->resize_filter, job->resize_filter,
           sizeof(slot->resize_filter));
    slot->sprite_count = job->sprite_count;
    memcpy(slot->sprite_variant_keys, job->sprite_variant_keys,
           sizeof(slot->sprite_variant_keys));
    memcpy(slot->sprite_width, job->sprite_width, sizeof(slot->sprite_width));
    memcpy(slot->sprite_height, job->sprite_height,
           sizeof(slot->sprite_height));
    slot->allow_lossy = job->allow_lossy ? 1U : 0U;
    slot->accept_webp = job->accept_webp ? 1U : 0U;
    if (job->payload.length != 0U) {
      memcpy(laghu_payload(slot), job->payload.data, job->payload.length);
    }
    slot->state = LAGHU_SLOT_READY;
    header->next_write = (header->next_write + 1U) % queue->slot_count;
    success = true;
  }
  laghu_unlock(file);
  return success;
}

bool laghu_runtime_queue_try_take(laghu_runtime_queue *queue,
                                  laghu_runtime_job *job,
                                  unsigned char *payload,
                                  size_t payload_capacity) {
  HANDLE file;
  laghu_queue_header *header;
  laghu_queue_slot *slot;
  bool success = false;
  if (queue == NULL || job == NULL || payload == NULL ||
      queue->mapping == NULL) {
    return false;
  }
  file = laghu_file(queue);
  if (!laghu_lock(file)) {
    return false;
  }
  header = laghu_header(queue);
  slot = laghu_slot(queue, header->next_read % queue->slot_count);
  if (slot->state == LAGHU_SLOT_READY &&
      slot->target_count <= LAGHU_RUNTIME_MAX_TARGETS &&
      slot->sprite_count <= LAGHU_RUNTIME_MAX_SPRITE_INPUTS &&
      slot->payload_length <= payload_capacity &&
      memchr(slot->provider_id, '\0', sizeof(slot->provider_id)) != NULL &&
      memchr(slot->provider_digest, '\0', sizeof(slot->provider_digest)) !=
          NULL &&
      memchr(slot->javascript_target, '\0', sizeof(slot->javascript_target)) !=
          NULL) {
    memset(job, 0, sizeof(*job));
    job->kind = (laghu_runtime_job_kind)slot->kind;
    memcpy(job->index_key, slot->index_key, sizeof(job->index_key));
    memcpy(job->request_path, slot->request_path, sizeof(job->request_path));
    memcpy(job->validator, slot->validator, sizeof(job->validator));
    memcpy(job->content_type, slot->content_type, sizeof(job->content_type));
    memcpy(job->policy_key, slot->policy_key, sizeof(job->policy_key));
    memcpy(job->provider_id, slot->provider_id, sizeof(job->provider_id));
    memcpy(job->provider_digest, slot->provider_digest,
           sizeof(job->provider_digest));
    memcpy(job->javascript_target, slot->javascript_target,
           sizeof(job->javascript_target));
    job->filters = slot->filters;
    job->quality = slot->quality;
    job->metadata_limit = slot->metadata_limit;
    job->metadata_ttl = slot->metadata_ttl;
    job->target_count = slot->target_count;
    memcpy(job->target_width, slot->target_width, sizeof(job->target_width));
    memcpy(job->target_height, slot->target_height, sizeof(job->target_height));
    memcpy(job->resize_filter, slot->resize_filter, sizeof(job->resize_filter));
    job->sprite_count = slot->sprite_count;
    memcpy(job->sprite_variant_keys, slot->sprite_variant_keys,
           sizeof(job->sprite_variant_keys));
    memcpy(job->sprite_width, slot->sprite_width, sizeof(job->sprite_width));
    memcpy(job->sprite_height, slot->sprite_height, sizeof(job->sprite_height));
    job->allow_lossy = slot->allow_lossy != 0U;
    job->accept_webp = slot->accept_webp != 0U;
    if (slot->payload_length != 0U) {
      memcpy(payload, laghu_payload(slot), (size_t)slot->payload_length);
    }
    job->payload = (laghu_buffer){payload, (size_t)slot->payload_length};
    memset(slot, 0, sizeof(*slot));
    header->next_read = (header->next_read + 1U) % queue->slot_count;
    success = true;
  }
  laghu_unlock(file);
  return success;
}

bool laghu_runtime_index_key(const char *request_path, const char *validator,
                             const char *policy_key, bool accept_webp,
                             char output[LAGHU_RUNTIME_KEY_SIZE]) {
  size_t total;
  char *canonical;
  int written;
  bool success;
  if (request_path == NULL || validator == NULL || output == NULL ||
      !laghu_hash_valid(policy_key)) {
    return false;
  }
  total = strlen(request_path) + strlen(validator) + strlen(policy_key) + 5U;
  canonical = (char *)malloc(total);
  if (canonical == NULL) {
    return false;
  }
  written = snprintf(canonical, total, "%s\n%s\n%s\n%c", request_path,
                     validator, policy_key, accept_webp ? '1' : '0');
  success = written >= 0 && (size_t)written < total &&
            laghu_sha256_hex((laghu_buffer){(const unsigned char *)canonical,
                                            (size_t)written},
                             output);
  free(canonical);
  return success;
}

static bool laghu_replace(const char *temporary, const char *target) {
  wchar_t wide_temporary[LAGHU_RUNTIME_PATH_SIZE];
  wchar_t wide_target[LAGHU_RUNTIME_PATH_SIZE];
  return laghu_wide(temporary, wide_temporary, LAGHU_RUNTIME_PATH_SIZE) &&
         laghu_wide(target, wide_target, LAGHU_RUNTIME_PATH_SIZE) &&
         MoveFileExW(wide_temporary, wide_target,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

static bool laghu_write_atomic(const char *path, const void *data,
                               size_t length) {
  char temporary[LAGHU_RUNTIME_PATH_SIZE];
  HANDLE file = INVALID_HANDLE_VALUE;
  int count = snprintf(temporary, sizeof(temporary), "%s.%lu.tmp", path,
                       (unsigned long)GetCurrentProcessId());
  bool success;
  if (count <= 0 || (size_t)count >= sizeof(temporary) ||
      !laghu_open(temporary, GENERIC_WRITE, CREATE_ALWAYS, &file)) {
    return false;
  }
  success = laghu_write(file, data, length) && FlushFileBuffers(file) != 0;
  CloseHandle(file);
  if (success) {
    success = laghu_replace(temporary, path);
  }
  if (!success) {
    wchar_t wide[LAGHU_RUNTIME_PATH_SIZE];
    if (laghu_wide(temporary, wide, LAGHU_RUNTIME_PATH_SIZE)) {
      (void)DeleteFileW(wide);
    }
  }
  return success;
}

bool laghu_runtime_cache_publish(const char *cache_path, const char *index_key,
                                 const char *variant_key, const char *validator,
                                 const char *content_type,
                                 const char *backend_id, laghu_buffer payload,
                                 laghu_runtime_cache_entry *entry) {
  char body_path[LAGHU_RUNTIME_PATH_SIZE];
  char metadata_path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_cache_metadata metadata;
  wchar_t wide_cache[LAGHU_RUNTIME_PATH_SIZE];
  if (cache_path == NULL || !laghu_hash_valid(index_key) ||
      !laghu_hash_valid(variant_key) || validator == NULL ||
      content_type == NULL || backend_id == NULL || payload.data == NULL ||
      payload.length == 0U || entry == NULL ||
      !laghu_wide(cache_path, wide_cache, LAGHU_RUNTIME_PATH_SIZE)) {
    return false;
  }
  if (!CreateDirectoryW(wide_cache, NULL) &&
      GetLastError() != ERROR_ALREADY_EXISTS) {
    return false;
  }
  if (!laghu_path(body_path, cache_path, "variant-", variant_key, ".bin") ||
      !laghu_path(metadata_path, cache_path, "index-", index_key, ".meta")) {
    return false;
  }
  memset(&metadata, 0, sizeof(metadata));
  metadata.magic = LAGHU_CACHE_MAGIC;
  metadata.version = LAGHU_CACHE_VERSION;
  metadata.length = payload.length;
  if (!laghu_copy(metadata.variant_key, sizeof(metadata.variant_key),
                  variant_key) ||
      !laghu_sha256_hex(payload, metadata.payload_hash) ||
      !laghu_copy(metadata.validator, sizeof(metadata.validator), validator) ||
      !laghu_copy(metadata.content_type, sizeof(metadata.content_type),
                  content_type) ||
      !laghu_copy(metadata.backend_id, sizeof(metadata.backend_id),
                  backend_id) ||
      !laghu_write_atomic(body_path, payload.data, payload.length) ||
      !laghu_write_atomic(metadata_path, &metadata, sizeof(metadata))) {
    return false;
  }
  if (strcmp(index_key, variant_key) != 0 &&
      (!laghu_path(metadata_path, cache_path, "index-", variant_key, ".meta") ||
       !laghu_write_atomic(metadata_path, &metadata, sizeof(metadata)))) {
    return false;
  }
  memset(entry, 0, sizeof(*entry));
  (void)laghu_copy(entry->variant_key, sizeof(entry->variant_key), variant_key);
  (void)laghu_copy(entry->payload_hash, sizeof(entry->payload_hash),
                   metadata.payload_hash);
  (void)laghu_copy(entry->validator, sizeof(entry->validator), validator);
  (void)laghu_copy(entry->content_type, sizeof(entry->content_type),
                   content_type);
  (void)laghu_copy(entry->backend_id, sizeof(entry->backend_id), backend_id);
  (void)laghu_copy(entry->variant_path, sizeof(entry->variant_path), body_path);
  entry->length = payload.length;
  return true;
}

bool laghu_runtime_cache_lookup_variant(const char *cache_path,
                                        const char *variant_key,
                                        laghu_runtime_cache_entry *entry) {
  char metadata_path[LAGHU_RUNTIME_PATH_SIZE];
  char body_path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_cache_metadata metadata;
  HANDLE file = INVALID_HANDLE_VALUE;
  LARGE_INTEGER size;
  if (cache_path == NULL || !laghu_hash_valid(variant_key) || entry == NULL ||
      !laghu_path(metadata_path, cache_path, "index-", variant_key, ".meta") ||
      !laghu_open(metadata_path, GENERIC_READ, OPEN_EXISTING, &file)) {
    return false;
  }
  if (!laghu_read(file, &metadata, sizeof(metadata))) {
    CloseHandle(file);
    return false;
  }
  CloseHandle(file);
  file = INVALID_HANDLE_VALUE;
  if (metadata.magic != LAGHU_CACHE_MAGIC ||
      metadata.version != LAGHU_CACHE_VERSION || metadata.length == 0U ||
      !laghu_hash_valid(metadata.variant_key) ||
      !laghu_hash_valid(metadata.payload_hash) ||
      strcmp(metadata.variant_key, variant_key) != 0 ||
      !laghu_path(body_path, cache_path, "variant-", metadata.variant_key,
                  ".bin") ||
      !laghu_open(body_path, GENERIC_READ, OPEN_EXISTING, &file) ||
      !GetFileSizeEx(file, &size) ||
      size.QuadPart != (LONGLONG)metadata.length) {
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
    }
    return false;
  }
  CloseHandle(file);
  memset(entry, 0, sizeof(*entry));
  (void)laghu_copy(entry->variant_key, sizeof(entry->variant_key), variant_key);
  (void)laghu_copy(entry->payload_hash, sizeof(entry->payload_hash),
                   metadata.payload_hash);
  (void)laghu_copy(entry->validator, sizeof(entry->validator),
                   metadata.validator);
  (void)laghu_copy(entry->content_type, sizeof(entry->content_type),
                   metadata.content_type);
  (void)laghu_copy(entry->backend_id, sizeof(entry->backend_id),
                   metadata.backend_id);
  (void)laghu_copy(entry->variant_path, sizeof(entry->variant_path), body_path);
  entry->length = (size_t)metadata.length;
  return true;
}

bool laghu_runtime_cache_lookup(const char *cache_path, const char *index_key,
                                const char *validator,
                                laghu_runtime_cache_entry *entry) {
  char metadata_path[LAGHU_RUNTIME_PATH_SIZE];
  char body_path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_cache_metadata metadata;
  HANDLE file = INVALID_HANDLE_VALUE;
  LARGE_INTEGER size;
  if (cache_path == NULL || !laghu_hash_valid(index_key) || validator == NULL ||
      entry == NULL ||
      !laghu_path(metadata_path, cache_path, "index-", index_key, ".meta") ||
      !laghu_open(metadata_path, GENERIC_READ, OPEN_EXISTING, &file)) {
    return false;
  }
  if (!laghu_read(file, &metadata, sizeof(metadata))) {
    CloseHandle(file);
    return false;
  }
  CloseHandle(file);
  file = INVALID_HANDLE_VALUE;
  if (metadata.magic != LAGHU_CACHE_MAGIC ||
      metadata.version != LAGHU_CACHE_VERSION || metadata.length == 0U ||
      !laghu_hash_valid(metadata.variant_key) ||
      !laghu_hash_valid(metadata.payload_hash) ||
      strcmp(metadata.validator, validator) != 0 ||
      !laghu_path(body_path, cache_path, "variant-", metadata.variant_key,
                  ".bin") ||
      !laghu_open(body_path, GENERIC_READ, OPEN_EXISTING, &file) ||
      !GetFileSizeEx(file, &size) ||
      size.QuadPart != (LONGLONG)metadata.length) {
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
    }
    return false;
  }
  CloseHandle(file);
  memset(entry, 0, sizeof(*entry));
  (void)laghu_copy(entry->variant_key, sizeof(entry->variant_key),
                   metadata.variant_key);
  (void)laghu_copy(entry->payload_hash, sizeof(entry->payload_hash),
                   metadata.payload_hash);
  (void)laghu_copy(entry->validator, sizeof(entry->validator),
                   metadata.validator);
  (void)laghu_copy(entry->content_type, sizeof(entry->content_type),
                   metadata.content_type);
  (void)laghu_copy(entry->backend_id, sizeof(entry->backend_id),
                   metadata.backend_id);
  (void)laghu_copy(entry->variant_path, sizeof(entry->variant_path), body_path);
  entry->length = (size_t)metadata.length;
  return true;
}

bool laghu_runtime_cache_read(const laghu_runtime_cache_entry *entry,
                              unsigned char *output, size_t output_capacity) {
  HANDLE file;
  char hash[LAGHU_RUNTIME_KEY_SIZE];
  if (entry == NULL || output == NULL || entry->length == 0U ||
      entry->length > output_capacity ||
      !laghu_hash_valid(entry->payload_hash) ||
      !laghu_open(entry->variant_path, GENERIC_READ, OPEN_EXISTING, &file)) {
    return false;
  }
  if (!laghu_read(file, output, entry->length)) {
    CloseHandle(file);
    return false;
  }
  CloseHandle(file);
  return laghu_sha256_hex((laghu_buffer){output, entry->length}, hash) &&
         strcmp(hash, entry->payload_hash) == 0;
}
