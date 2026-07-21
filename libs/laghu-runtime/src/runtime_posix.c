// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "laghu/runtime.h"

#define LAGHU_QUEUE_MAGIC UINT64_C(0x4c41474855515545)
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
  uint32_t reserved;
  uint64_t payload_length;
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char request_path[LAGHU_RUNTIME_PATH_SIZE];
  char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  char content_type[LAGHU_RUNTIME_TYPE_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  uint64_t filters;
  uint32_t quality;
  uint32_t metadata_limit;
  uint32_t metadata_ttl;
  uint32_t target_count;
  uint32_t target_width[LAGHU_RUNTIME_MAX_TARGETS];
  uint32_t target_height[LAGHU_RUNTIME_MAX_TARGETS];
  uint64_t resize_filter[LAGHU_RUNTIME_MAX_TARGETS];
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

#define LAGHU_CACHE_MAGIC UINT64_C(0x4c41474855434143)
#define LAGHU_CACHE_VERSION 2U

static size_t laghu_queue_slot_size(size_t payload_size) {
  return sizeof(laghu_queue_slot) + payload_size;
}

static laghu_queue_header *laghu_queue_header_at(laghu_runtime_queue *queue) {
  return queue->mapping;
}

static laghu_queue_slot *laghu_queue_slot_at(laghu_runtime_queue *queue,
                                             unsigned int index) {
  unsigned char *base = queue->mapping;
  return (laghu_queue_slot *)(void *)(base + sizeof(laghu_queue_header) +
                                      index * laghu_queue_slot_size(
                                                  queue->slot_payload_size));
}

static unsigned char *laghu_queue_payload_at(laghu_queue_slot *slot) {
  return (unsigned char *)(void *)(slot + 1);
}

static bool laghu_lock(int platform_file, short type) {
  struct flock lock = {
      .l_type = type, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0};
  return fcntl(platform_file, F_SETLK, &lock) == 0;
}

static void laghu_unlock(int platform_file) {
  struct flock lock = {
      .l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0};
  (void)fcntl(platform_file, F_SETLK, &lock);
}

static bool laghu_string_copy(char *target, size_t target_size,
                              const char *source) {
  size_t length;
  if (target == NULL || target_size == 0U || source == NULL) {
    return false;
  }
  length = strlen(source);
  if (length >= target_size) {
    return false;
  }
  memcpy(target, source, length + 1U);
  return true;
}

static bool laghu_hash_string_valid(const char *value, size_t capacity) {
  size_t index;

  if (value == NULL || capacity != LAGHU_RUNTIME_KEY_SIZE ||
      memchr(value, '\0', capacity) != value + LAGHU_RUNTIME_KEY_SIZE - 1U) {
    return false;
  }
  for (index = 0U; index < LAGHU_RUNTIME_KEY_SIZE - 1U; ++index) {
    if (!((value[index] >= '0' && value[index] <= '9') ||
          (value[index] >= 'a' && value[index] <= 'f'))) {
      return false;
    }
  }
  return true;
}

static bool laghu_write_all(int platform_file, const unsigned char *data,
                            size_t length) {
  while (length > 0U) {
    ssize_t written = write(platform_file, data, length);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    data += (size_t)written;
    length -= (size_t)written;
  }
  return true;
}

static bool laghu_read_all(int platform_file, unsigned char *data,
                           size_t length) {
  while (length > 0U) {
    ssize_t count = read(platform_file, data, length);
    if (count <= 0) {
      if (count < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    data += (size_t)count;
    length -= (size_t)count;
  }
  return true;
}

void laghu_runtime_queue_init(laghu_runtime_queue *queue) {
  if (queue == NULL) {
    return;
  }
  memset(queue, 0, sizeof(*queue));
  queue->platform_file = -1;
  queue->platform_mapping = -1;
}

bool laghu_runtime_queue_create(laghu_runtime_queue *queue, const char *path,
                                unsigned int slot_count,
                                size_t slot_payload_size) {
  laghu_queue_header *header;
  size_t mapping_length;
  int platform_file;
  void *mapping;

  if (queue == NULL || path == NULL || slot_count == 0U ||
      slot_payload_size == 0U ||
      slot_payload_size > SIZE_MAX - sizeof(laghu_queue_slot) ||
      slot_count > (SIZE_MAX - sizeof(laghu_queue_header)) /
                       laghu_queue_slot_size(slot_payload_size)) {
    return false;
  }
  mapping_length = sizeof(laghu_queue_header) +
                   slot_count * laghu_queue_slot_size(slot_payload_size);
  platform_file = open(path, O_RDWR | O_CREAT, 0600);
  if (platform_file < 0 ||
      ftruncate(platform_file, (off_t)mapping_length) != 0) {
    if (platform_file >= 0) {
      close(platform_file);
    }
    return false;
  }
  if (fchmod(platform_file, 0660) != 0) {
    close(platform_file);
    return false;
  }
  mapping = mmap(NULL, mapping_length, PROT_READ | PROT_WRITE, MAP_SHARED,
                 platform_file, 0);
  if (mapping == MAP_FAILED) {
    close(platform_file);
    return false;
  }
  memset(mapping, 0, mapping_length);
  header = mapping;
  header->magic = LAGHU_QUEUE_MAGIC;
  header->version = LAGHU_QUEUE_VERSION;
  header->slot_count = slot_count;
  header->slot_payload_size = slot_payload_size;
  queue->platform_file = platform_file;
  queue->mapping = mapping;
  queue->mapping_length = mapping_length;
  queue->slot_count = slot_count;
  queue->slot_payload_size = slot_payload_size;
  queue->capabilities = 0U;
  queue->worker_heartbeat = 0U;
  queue->backend_id[0] = '\0';
  return msync(mapping, sizeof(*header), MS_SYNC) == 0;
}

bool laghu_runtime_queue_open(laghu_runtime_queue *queue, const char *path) {
  laghu_queue_header header;
  struct stat status;
  int platform_file;
  void *mapping;
  size_t expected;

  if (queue == NULL || path == NULL) {
    return false;
  }
  platform_file = open(path, O_RDWR);
  if (platform_file < 0 ||
      !laghu_read_all(platform_file, (unsigned char *)&header,
                      sizeof(header)) ||
      fstat(platform_file, &status) != 0 || header.magic != LAGHU_QUEUE_MAGIC ||
      header.version != LAGHU_QUEUE_VERSION || header.slot_count == 0U ||
      header.slot_payload_size == 0U ||
      header.slot_payload_size > SIZE_MAX - sizeof(laghu_queue_slot) ||
      header.slot_count >
          (SIZE_MAX - sizeof(laghu_queue_header)) /
              laghu_queue_slot_size((size_t)header.slot_payload_size) ||
      memchr(header.backend_id, '\0', sizeof(header.backend_id)) == NULL) {
    if (platform_file >= 0) {
      close(platform_file);
    }
    return false;
  }
  expected =
      sizeof(laghu_queue_header) +
      header.slot_count * laghu_queue_slot_size(header.slot_payload_size);
  if ((uintmax_t)status.st_size != (uintmax_t)expected) {
    close(platform_file);
    return false;
  }
  mapping = mmap(NULL, expected, PROT_READ | PROT_WRITE, MAP_SHARED,
                 platform_file, 0);
  if (mapping == MAP_FAILED) {
    close(platform_file);
    return false;
  }
  queue->platform_file = platform_file;
  queue->mapping = mapping;
  queue->mapping_length = expected;
  queue->slot_count = header.slot_count;
  queue->slot_payload_size = header.slot_payload_size;
  queue->capabilities = header.capabilities;
  queue->worker_heartbeat = header.worker_heartbeat;
  memcpy(queue->backend_id, header.backend_id, sizeof(queue->backend_id));
  queue->backend_id[sizeof(queue->backend_id) - 1U] = '\0';
  return true;
}

bool laghu_runtime_queue_refresh(laghu_runtime_queue *queue) {
  laghu_queue_header *header;
  bool valid;

  if (queue == NULL || queue->mapping == NULL ||
      !laghu_lock(queue->platform_file, F_RDLCK)) {
    return false;
  }
  header = laghu_queue_header_at(queue);
  valid = header->magic == LAGHU_QUEUE_MAGIC &&
          header->version == LAGHU_QUEUE_VERSION &&
          header->slot_count == queue->slot_count &&
          header->slot_payload_size == queue->slot_payload_size &&
          memchr(header->backend_id, '\0', sizeof(header->backend_id)) != NULL;
  if (valid) {
    queue->capabilities = header->capabilities;
    queue->worker_heartbeat = header->worker_heartbeat;
    memcpy(queue->backend_id, header->backend_id, sizeof(queue->backend_id));
    queue->backend_id[sizeof(queue->backend_id) - 1U] = '\0';
  }
  laghu_unlock(queue->platform_file);
  return valid;
}

bool laghu_runtime_queue_set_backend(laghu_runtime_queue *queue,
                                     uint32_t capabilities,
                                     const char *backend_id) {
  laghu_queue_header *header;
  bool success = false;

  if (queue == NULL || queue->mapping == NULL || backend_id == NULL ||
      !laghu_lock(queue->platform_file, F_WRLCK)) {
    return false;
  }
  header = laghu_queue_header_at(queue);
  if (laghu_string_copy(header->backend_id, sizeof(header->backend_id),
                        backend_id)) {
    header->capabilities = capabilities;
    queue->capabilities = capabilities;
    memcpy(queue->backend_id, header->backend_id, sizeof(queue->backend_id));
    success = msync(header, sizeof(*header), MS_ASYNC) == 0;
  }
  laghu_unlock(queue->platform_file);
  return success;
}

bool laghu_runtime_queue_heartbeat(laghu_runtime_queue *queue,
                                   uint64_t epoch_seconds) {
  laghu_queue_header *header;

  if (queue == NULL || queue->mapping == NULL || epoch_seconds == 0U ||
      !laghu_lock(queue->platform_file, F_WRLCK)) {
    return false;
  }
  header = laghu_queue_header_at(queue);
  header->worker_heartbeat = epoch_seconds;
  queue->worker_heartbeat = epoch_seconds;
  laghu_unlock(queue->platform_file);
  return true;
}

void laghu_runtime_queue_close(laghu_runtime_queue *queue) {
  if (queue == NULL) {
    return;
  }
  if (queue->mapping != NULL && queue->mapping != MAP_FAILED) {
    (void)munmap(queue->mapping, queue->mapping_length);
  }
  if (queue->platform_file >= 0) {
    (void)close(queue->platform_file);
  }
  laghu_runtime_queue_init(queue);
}

bool laghu_runtime_queue_try_publish(laghu_runtime_queue *queue,
                                     const laghu_runtime_job *job) {
  laghu_queue_header *header;
  laghu_queue_slot *slot;
  bool success = false;

  if (queue == NULL || queue->mapping == NULL || job == NULL ||
      job->target_count > LAGHU_RUNTIME_MAX_TARGETS ||
      job->payload.length > queue->slot_payload_size ||
      (job->payload.data == NULL && job->payload.length != 0U) ||
      !laghu_lock(queue->platform_file, F_WRLCK)) {
    return false;
  }
  header = laghu_queue_header_at(queue);
  slot = laghu_queue_slot_at(queue, header->next_write % queue->slot_count);
  if (slot->state == LAGHU_SLOT_EMPTY &&
      laghu_string_copy(slot->index_key, sizeof(slot->index_key),
                        job->index_key) &&
      laghu_string_copy(slot->request_path, sizeof(slot->request_path),
                        job->request_path) &&
      laghu_string_copy(slot->validator, sizeof(slot->validator),
                        job->validator) &&
      laghu_string_copy(slot->content_type, sizeof(slot->content_type),
                        job->content_type) &&
      laghu_string_copy(slot->policy_key, sizeof(slot->policy_key),
                        job->policy_key)) {
    slot->payload_length = job->payload.length;
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
    slot->allow_lossy = job->allow_lossy ? 1U : 0U;
    slot->accept_webp = job->accept_webp ? 1U : 0U;
    memcpy(laghu_queue_payload_at(slot), job->payload.data,
           job->payload.length);
    slot->state = LAGHU_SLOT_READY;
    header->next_write = (header->next_write + 1U) % queue->slot_count;
    success = true;
  }
  laghu_unlock(queue->platform_file);
  return success;
}

bool laghu_runtime_queue_try_take(laghu_runtime_queue *queue,
                                  laghu_runtime_job *job,
                                  unsigned char *payload,
                                  size_t payload_capacity) {
  laghu_queue_header *header;
  laghu_queue_slot *slot;
  bool success = false;

  if (queue == NULL || queue->mapping == NULL || job == NULL ||
      payload == NULL || !laghu_lock(queue->platform_file, F_WRLCK)) {
    return false;
  }
  header = laghu_queue_header_at(queue);
  slot = laghu_queue_slot_at(queue, header->next_read % queue->slot_count);
  if (slot->state == LAGHU_SLOT_READY &&
      slot->target_count <= LAGHU_RUNTIME_MAX_TARGETS &&
      slot->payload_length <= payload_capacity &&
      slot->payload_length <= queue->slot_payload_size &&
      memchr(slot->index_key, '\0', sizeof(slot->index_key)) != NULL &&
      memchr(slot->request_path, '\0', sizeof(slot->request_path)) != NULL &&
      memchr(slot->validator, '\0', sizeof(slot->validator)) != NULL &&
      memchr(slot->content_type, '\0', sizeof(slot->content_type)) != NULL &&
      memchr(slot->policy_key, '\0', sizeof(slot->policy_key)) != NULL) {
    memset(job, 0, sizeof(*job));
    memcpy(job->index_key, slot->index_key, sizeof(job->index_key));
    memcpy(job->request_path, slot->request_path, sizeof(job->request_path));
    memcpy(job->validator, slot->validator, sizeof(job->validator));
    memcpy(job->content_type, slot->content_type, sizeof(job->content_type));
    memcpy(job->policy_key, slot->policy_key, sizeof(job->policy_key));
    job->filters = slot->filters;
    job->quality = slot->quality;
    job->metadata_limit = slot->metadata_limit;
    job->metadata_ttl = slot->metadata_ttl;
    job->target_count = slot->target_count;
    memcpy(job->target_width, slot->target_width, sizeof(job->target_width));
    memcpy(job->target_height, slot->target_height, sizeof(job->target_height));
    memcpy(job->resize_filter, slot->resize_filter, sizeof(job->resize_filter));
    job->allow_lossy = slot->allow_lossy != 0U;
    job->accept_webp = slot->accept_webp != 0U;
    memcpy(payload, laghu_queue_payload_at(slot), (size_t)slot->payload_length);
    job->payload = (laghu_buffer){payload, (size_t)slot->payload_length};
    memset(laghu_queue_payload_at(slot), 0, (size_t)slot->payload_length);
    memset(slot, 0, sizeof(*slot));
    header->next_read = (header->next_read + 1U) % queue->slot_count;
    success = true;
  } else if (slot->state == LAGHU_SLOT_READY) {
    memset(laghu_queue_payload_at(slot), 0, queue->slot_payload_size);
    memset(slot, 0, sizeof(*slot));
    header->next_read = (header->next_read + 1U) % queue->slot_count;
  }
  laghu_unlock(queue->platform_file);
  return success;
}

bool laghu_runtime_index_key(const char *request_path, const char *validator,
                             const char *policy_key, bool accept_webp,
                             char output[LAGHU_RUNTIME_KEY_SIZE]) {
  size_t path_length;
  size_t validator_length;
  size_t policy_length;
  size_t total;
  unsigned char *canonical;
  int written;
  bool success;

  if (request_path == NULL || validator == NULL || policy_key == NULL ||
      output == NULL) {
    return false;
  }
  path_length = strlen(request_path);
  validator_length = strlen(validator);
  policy_length = strlen(policy_key);
  if (validator_length > SIZE_MAX - 5U ||
      path_length > SIZE_MAX - validator_length - 5U ||
      path_length + validator_length + 5U > SIZE_MAX - policy_length) {
    output[0] = '\0';
    return false;
  }
  total = path_length + validator_length + policy_length + 5U;
  canonical = malloc(total);
  if (canonical == NULL) {
    output[0] = '\0';
    return false;
  }
  written = snprintf((char *)canonical, total, "%s\n%s\n%s\n%c", request_path,
                     validator, policy_key, accept_webp ? '1' : '0');
  success =
      written >= 0 && (size_t)written < total &&
      laghu_sha256_hex((laghu_buffer){canonical, (size_t)written}, output);
  free(canonical);
  return success;
}

static bool laghu_cache_paths(const char *cache_path, const char *index_key,
                              const char *variant_key, char *index_path,
                              size_t index_size, char *variant_path,
                              size_t variant_size) {
  int index_length;
  int variant_length;
  if (cache_path == NULL || index_key == NULL || variant_key == NULL) {
    return false;
  }
  index_length = snprintf(index_path, index_size, "%s/index-%s.meta",
                          cache_path, index_key);
  variant_length = snprintf(variant_path, variant_size, "%s/variant-%s.bin",
                            cache_path, variant_key);
  return index_length > 0 && (size_t)index_length < index_size &&
         variant_length > 0 && (size_t)variant_length < variant_size;
}

static bool laghu_atomic_file(const char *path, const unsigned char *data,
                              size_t length) {
  char temporary[LAGHU_RUNTIME_PATH_SIZE + 64U];
  int platform_file;
  int path_length = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
                             (long)getpid());
  bool success;
  if (path_length <= 0 || (size_t)path_length >= sizeof(temporary)) {
    return false;
  }
  platform_file = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (platform_file < 0) {
    return false;
  }
  if (fchmod(platform_file, 0640) != 0) {
    (void)close(platform_file);
    (void)unlink(temporary);
    return false;
  }
  success =
      laghu_write_all(platform_file, data, length) && fsync(platform_file) == 0;
  if (close(platform_file) != 0) {
    success = false;
  }
  platform_file = -1;
  if (success) {
    success = rename(temporary, path) == 0;
  }
  if (!success) {
    if (platform_file >= 0) {
      (void)close(platform_file);
    }
    (void)unlink(temporary);
  }
  return success;
}

bool laghu_runtime_cache_publish(const char *cache_path, const char *index_key,
                                 const char *variant_key, const char *validator,
                                 const char *content_type,
                                 const char *backend_id, laghu_buffer payload,
                                 laghu_runtime_cache_entry *entry) {
  laghu_cache_metadata metadata;
  char index_path[LAGHU_RUNTIME_PATH_SIZE];
  char variant_path[LAGHU_RUNTIME_PATH_SIZE];
  char payload_hash[LAGHU_RUNTIME_KEY_SIZE];

  if (cache_path == NULL || index_key == NULL || variant_key == NULL ||
      validator == NULL || content_type == NULL || backend_id == NULL ||
      entry == NULL || payload.data == NULL || payload.length == 0U ||
      !laghu_sha256_hex(payload, payload_hash) ||
      (mkdir(cache_path, 0750) != 0 && errno != EEXIST) ||
      !laghu_cache_paths(cache_path, index_key, variant_key, index_path,
                         sizeof(index_path), variant_path,
                         sizeof(variant_path))) {
    return false;
  }
  memset(&metadata, 0, sizeof(metadata));
  metadata.magic = LAGHU_CACHE_MAGIC;
  metadata.version = LAGHU_CACHE_VERSION;
  metadata.length = payload.length;
  if (!laghu_string_copy(metadata.variant_key, sizeof(metadata.variant_key),
                         variant_key) ||
      !laghu_string_copy(metadata.payload_hash, sizeof(metadata.payload_hash),
                         payload_hash) ||
      !laghu_string_copy(metadata.validator, sizeof(metadata.validator),
                         validator) ||
      !laghu_string_copy(metadata.content_type, sizeof(metadata.content_type),
                         content_type) ||
      !laghu_string_copy(metadata.backend_id, sizeof(metadata.backend_id),
                         backend_id) ||
      !laghu_atomic_file(variant_path, payload.data, payload.length) ||
      !laghu_atomic_file(index_path, (const unsigned char *)&metadata,
                         sizeof(metadata))) {
    return false;
  }
  if (strcmp(index_key, variant_key) != 0 &&
      (!laghu_cache_paths(cache_path, variant_key, variant_key, index_path,
                          sizeof(index_path), variant_path,
                          sizeof(variant_path)) ||
       !laghu_atomic_file(index_path, (const unsigned char *)&metadata,
                          sizeof(metadata)))) {
    return false;
  }
  memset(entry, 0, sizeof(*entry));
  (void)laghu_string_copy(entry->variant_key, sizeof(entry->variant_key),
                          variant_key);
  (void)laghu_string_copy(entry->payload_hash, sizeof(entry->payload_hash),
                          payload_hash);
  (void)laghu_string_copy(entry->validator, sizeof(entry->validator),
                          validator);
  (void)laghu_string_copy(entry->content_type, sizeof(entry->content_type),
                          content_type);
  (void)laghu_string_copy(entry->backend_id, sizeof(entry->backend_id),
                          backend_id);
  (void)laghu_string_copy(entry->variant_path, sizeof(entry->variant_path),
                          variant_path);
  entry->length = payload.length;
  return true;
}

bool laghu_runtime_cache_lookup_variant(const char *cache_path,
                                        const char *variant_key,
                                        laghu_runtime_cache_entry *entry) {
  laghu_cache_metadata metadata;
  char index_path[LAGHU_RUNTIME_PATH_SIZE];
  char variant_path[LAGHU_RUNTIME_PATH_SIZE];
  int platform_file;
  struct stat status;
  bool read_success;
  if (cache_path == NULL ||
      !laghu_hash_string_valid(variant_key, LAGHU_RUNTIME_KEY_SIZE) ||
      entry == NULL ||
      !laghu_cache_paths(cache_path, variant_key, variant_key, index_path,
                         sizeof(index_path), variant_path,
                         sizeof(variant_path))) {
    return false;
  }
  platform_file = open(index_path, O_RDONLY);
  if (platform_file < 0) {
    return false;
  }
  read_success = laghu_read_all(platform_file, (unsigned char *)&metadata,
                                sizeof(metadata));
  if (close(platform_file) != 0) {
    read_success = false;
  }
  if (!read_success || metadata.magic != LAGHU_CACHE_MAGIC ||
      metadata.version != LAGHU_CACHE_VERSION ||
      !laghu_hash_string_valid(metadata.variant_key,
                               sizeof(metadata.variant_key)) ||
      !laghu_hash_string_valid(metadata.payload_hash,
                               sizeof(metadata.payload_hash)) ||
      memchr(metadata.content_type, '\0', sizeof(metadata.content_type)) ==
          NULL ||
      memchr(metadata.validator, '\0', sizeof(metadata.validator)) == NULL ||
      memchr(metadata.backend_id, '\0', sizeof(metadata.backend_id)) == NULL ||
      strcmp(metadata.variant_key, variant_key) != 0 ||
      stat(variant_path, &status) != 0 ||
      (uintmax_t)status.st_size != metadata.length) {
    return false;
  }
  memset(entry, 0, sizeof(*entry));
  memcpy(entry->variant_key, metadata.variant_key, sizeof(entry->variant_key));
  memcpy(entry->payload_hash, metadata.payload_hash,
         sizeof(entry->payload_hash));
  memcpy(entry->validator, metadata.validator, sizeof(entry->validator));
  memcpy(entry->content_type, metadata.content_type,
         sizeof(entry->content_type));
  memcpy(entry->backend_id, metadata.backend_id, sizeof(entry->backend_id));
  (void)laghu_string_copy(entry->variant_path, sizeof(entry->variant_path),
                          variant_path);
  entry->length = (size_t)metadata.length;
  return true;
}

bool laghu_runtime_cache_lookup(const char *cache_path, const char *index_key,
                                const char *validator,
                                laghu_runtime_cache_entry *entry) {
  laghu_cache_metadata metadata;
  char index_path[LAGHU_RUNTIME_PATH_SIZE];
  char variant_path[LAGHU_RUNTIME_PATH_SIZE];
  int platform_file;
  struct stat status;

  if (cache_path == NULL || index_key == NULL || validator == NULL ||
      entry == NULL ||
      !laghu_cache_paths(cache_path, index_key, index_key, index_path,
                         sizeof(index_path), variant_path,
                         sizeof(variant_path))) {
    return false;
  }
  platform_file = open(index_path, O_RDONLY);
  if (platform_file < 0) {
    return false;
  }
  if (!laghu_read_all(platform_file, (unsigned char *)&metadata,
                      sizeof(metadata)) ||
      close(platform_file) != 0) {
    (void)close(platform_file);
    return false;
  }
  platform_file = -1;
  if (metadata.magic != LAGHU_CACHE_MAGIC ||
      metadata.version != LAGHU_CACHE_VERSION ||
      !laghu_hash_string_valid(metadata.variant_key,
                               sizeof(metadata.variant_key)) ||
      !laghu_hash_string_valid(metadata.payload_hash,
                               sizeof(metadata.payload_hash)) ||
      memchr(metadata.validator, '\0', sizeof(metadata.validator)) == NULL ||
      memchr(metadata.content_type, '\0', sizeof(metadata.content_type)) ==
          NULL ||
      memchr(metadata.backend_id, '\0', sizeof(metadata.backend_id)) == NULL ||
      strcmp(metadata.validator, validator) != 0 ||
      !laghu_cache_paths(cache_path, index_key, metadata.variant_key,
                         index_path, sizeof(index_path), variant_path,
                         sizeof(variant_path)) ||
      stat(variant_path, &status) != 0 ||
      (uintmax_t)status.st_size != metadata.length) {
    return false;
  }
  memset(entry, 0, sizeof(*entry));
  memcpy(entry->variant_key, metadata.variant_key, sizeof(entry->variant_key));
  memcpy(entry->payload_hash, metadata.payload_hash,
         sizeof(entry->payload_hash));
  memcpy(entry->validator, metadata.validator, sizeof(entry->validator));
  memcpy(entry->content_type, metadata.content_type,
         sizeof(entry->content_type));
  memcpy(entry->backend_id, metadata.backend_id, sizeof(entry->backend_id));
  (void)laghu_string_copy(entry->variant_path, sizeof(entry->variant_path),
                          variant_path);
  entry->length = (size_t)metadata.length;
  return true;
}

bool laghu_runtime_cache_read(const laghu_runtime_cache_entry *entry,
                              unsigned char *output, size_t output_capacity) {
  char payload_hash[LAGHU_RUNTIME_KEY_SIZE];
  int platform_file;
  bool read_success;
  bool close_success;
  if (entry == NULL || output == NULL || entry->length > output_capacity) {
    return false;
  }
  platform_file = open(entry->variant_path, O_RDONLY);
  if (platform_file < 0) {
    return false;
  }
  read_success = laghu_read_all(platform_file, output, entry->length);
  close_success = close(platform_file) == 0;
  return read_success && close_success &&
         laghu_sha256_hex((laghu_buffer){output, entry->length},
                          payload_hash) &&
         strcmp(payload_hash, entry->payload_hash) == 0;
}
