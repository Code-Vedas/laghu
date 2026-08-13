// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/cache.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "laghu/types.h"
#include "runtime_platform.h"

#define LAGHU_CACHE_INDEX_MAGIC UINT64_C(0x4c414748554c5255)
#define LAGHU_CACHE_INDEX_VERSION 2U
#define LAGHU_CACHE_SLOT_EMPTY 0U
#define LAGHU_CACHE_SLOT_READY 1U
#define LAGHU_CACHE_SLOT_DELETED 2U
#define LAGHU_CACHE_SLOT_ASSOCIATION 3U
#define LAGHU_CACHE_SLOT_PUBLISHING 4U
#define LAGHU_CACHE_BACKEND_REGISTRY_SIZE 16U
#define LAGHU_CACHE_TOMBSTONE_COUNT 128U

static laghu_cache_backend laghu_cache_backends[LAGHU_CACHE_BACKEND_REGISTRY_SIZE];
static size_t laghu_cache_backend_count;
#define LAGHU_THREAD_LOCAL _Thread_local

static LAGHU_THREAD_LOCAL char laghu_cache_scoped_path[LAGHU_RUNTIME_PATH_SIZE];
static LAGHU_THREAD_LOCAL char laghu_cache_scoped_source[LAGHU_RUNTIME_KEY_SIZE];
static LAGHU_THREAD_LOCAL unsigned int laghu_cache_scoped_variant_limit = LAGHU_VARIANTS_PER_SOURCE_DEFAULT;

typedef struct {
  uint64_t magic;
  uint32_t version;
  uint32_t slot_count;
  uint64_t size_limit;
  uint64_t inode_limit;
  uint32_t clean_interval;
  uint32_t reserved;
  uint64_t bytes;
  uint64_t files;
  uint64_t hits;
  uint64_t misses;
  uint64_t rejected_publications;
  uint64_t evictions;
  uint64_t corrupt_removals;
  uint64_t publications;
  uint64_t url_purges;
  uint64_t full_purges;
  uint64_t invalidated_artifacts;
  uint64_t invalidated_bytes;
  uint64_t cache_generation;
  uint64_t last_purge;
  uint64_t last_cleanup;
  uint64_t cleaner_lease_until;
  uint32_t rebuilding;
  uint32_t padding;
} laghu_cache_index_header;

typedef struct {
  char source_hash[LAGHU_RUNTIME_KEY_SIZE];
  uint64_t generation;
  uint64_t purged_at;
} laghu_cache_tombstone;

typedef struct {
  uint32_t state;
  uint32_t file_count;
  uint64_t length;
  uint64_t accessed_at;
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char variant_key[LAGHU_RUNTIME_KEY_SIZE];
  char payload_hash[LAGHU_RUNTIME_KEY_SIZE];
  char source_hash[LAGHU_RUNTIME_KEY_SIZE];
  uint64_t generation;
} laghu_cache_index_slot;

typedef struct {
  laghu_runtime_shared_mapping mapping;
  laghu_cache_index_header *header;
  laghu_cache_index_slot *slots;
  laghu_cache_tombstone *tombstones;
  uint32_t slot_count;
} laghu_file_cache_state;

static bool laghu_cache_index_valid(laghu_cache_backend *backend, laghu_file_cache_state *state) {
  return state->header->magic == LAGHU_CACHE_INDEX_MAGIC && state->header->version == LAGHU_CACHE_INDEX_VERSION &&
         state->header->slot_count == state->slot_count && state->header->size_limit == backend->limits.size_limit &&
         state->header->inode_limit == backend->limits.inode_limit && state->header->clean_interval == backend->limits.clean_interval;
}

static void laghu_cache_index_recover(laghu_cache_backend *backend, laghu_file_cache_state *state) {
  uint64_t corrupt = state->header->corrupt_removals + 1U;
  memset(state->mapping.mapping, 0, state->mapping.mapping_length);
  state->header->magic = LAGHU_CACHE_INDEX_MAGIC;
  state->header->version = LAGHU_CACHE_INDEX_VERSION;
  state->header->slot_count = state->slot_count;
  state->header->size_limit = backend->limits.size_limit;
  state->header->inode_limit = backend->limits.inode_limit;
  state->header->clean_interval = backend->limits.clean_interval;
  state->header->cache_generation = 1U;
  state->header->corrupt_removals = corrupt;
}

static uint64_t laghu_cache_hash_key(const char *key) {
  uint64_t hash = UINT64_C(1469598103934665603);
  size_t index;
  for (index = 0U; key != NULL && key[index] != '\0'; ++index) {
    hash ^= (unsigned char)key[index];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static int laghu_cache_slot_compare(const void *left, const void *right) {
  const laghu_cache_index_slot *const *a = left;
  const laghu_cache_index_slot *const *b = right;
  if ((*a)->accessed_at < (*b)->accessed_at) return -1;
  if ((*a)->accessed_at > (*b)->accessed_at) return 1;
  return strcmp((*a)->variant_key, (*b)->variant_key);
}

static bool laghu_cache_file_path(char *output, size_t output_size, const char *cache_path, const char *prefix, const char *key, const char *suffix) {
  int written = snprintf(output, output_size, "%s/%s%s%s", cache_path, prefix, key, suffix);
  return written > 0 && (size_t)written < output_size;
}

static bool laghu_cache_metadata_path(char *output, size_t output_size, const char *cache_path) {
  int written = snprintf(output, output_size, "%s.laghu-metadata", cache_path);
  return written > 0 && (size_t)written < output_size;
}

static bool laghu_cache_delete_slot(laghu_cache_backend *backend, laghu_cache_index_slot *slot) {
  char path[LAGHU_RUNTIME_PATH_SIZE];
  bool success = true;
  if (slot == NULL || slot->state != LAGHU_CACHE_SLOT_READY) return false;
  if (!laghu_cache_file_path(path, sizeof(path), backend->path, "variant-", slot->variant_key, ".bin") || !laghu_runtime_file_remove(path))
    success = false;
  if (!laghu_cache_file_path(path, sizeof(path), backend->path, "index-", slot->variant_key, ".meta") || !laghu_runtime_file_remove(path))
    success = false;
  if (strcmp(slot->index_key, slot->variant_key) != 0 &&
      (!laghu_cache_file_path(path, sizeof(path), backend->path, "index-", slot->index_key, ".meta") || !laghu_runtime_file_remove(path)))
    success = false;
  return success;
}

static laghu_cache_index_slot *laghu_cache_slot(laghu_file_cache_state *state, const char *variant_key, bool empty_allowed) {
  uint64_t hash;
  unsigned int index;
  laghu_cache_index_slot *empty = NULL;
  if (state == NULL || state->header == NULL || variant_key == NULL || state->header->slot_count == 0U) return NULL;
  hash = laghu_cache_hash_key(variant_key);
  for (index = 0U; index < state->header->slot_count; ++index) {
    laghu_cache_index_slot *slot = &state->slots[(hash + index) % state->header->slot_count];
    if (slot->state == LAGHU_CACHE_SLOT_EMPTY) {
      if (empty == NULL) empty = slot;
      break;
    }
    if (slot->state == LAGHU_CACHE_SLOT_DELETED && empty == NULL) empty = slot;
    if ((slot->state == LAGHU_CACHE_SLOT_READY || slot->state == LAGHU_CACHE_SLOT_ASSOCIATION || slot->state == LAGHU_CACHE_SLOT_PUBLISHING) &&
        strcmp(slot->variant_key, variant_key) == 0)
      return slot;
  }
  return empty_allowed ? empty : NULL;
}

static bool laghu_cache_hash_valid(const char *value) {
  size_t index;
  if (value == NULL || strlen(value) != LAGHU_SHA256_HEX_LENGTH) return false;
  for (index = 0U; index < LAGHU_SHA256_HEX_LENGTH; ++index)
    if (!isxdigit((unsigned char)value[index])) return false;
  return true;
}

static uint64_t laghu_cache_source_generation(laghu_file_cache_state *state, const char *source_hash) {
  size_t index;
  uint64_t generation = state->header->cache_generation;
  if (!laghu_cache_hash_valid(source_hash)) return generation;
  for (index = 0U; index < LAGHU_CACHE_TOMBSTONE_COUNT; ++index)
    if (strcmp(state->tombstones[index].source_hash, source_hash) == 0 && state->tombstones[index].generation > generation)
      generation = state->tombstones[index].generation;
  return generation;
}

static bool laghu_cache_metadata_open(laghu_cache_backend *backend) {
  laghu_file_cache_state *state;
  char path[LAGHU_RUNTIME_PATH_SIZE];
  size_t slots;
  int written;
  bool initialize;
  if (backend == NULL || !laghu_runtime_directory_exists(backend->path)) return false;
  state = calloc(1U, sizeof(*state));
  if (state == NULL) return false;
  laghu_runtime_shared_mapping_init(&state->mapping);
  written = laghu_cache_metadata_path(path, sizeof(path), backend->path) ? 1 : 0;
  if (written == 0 || !laghu_runtime_shared_mapping_open(&state->mapping, path, backend->limits.metadata_size) ||
      !laghu_runtime_shared_mapping_try_lock(&state->mapping)) {
    laghu_runtime_shared_mapping_close(&state->mapping);
    free(state);
    return false;
  }
  state->header = state->mapping.mapping;
  if (state->mapping.mapping_length <= sizeof(*state->header) + sizeof(laghu_cache_tombstone) * LAGHU_CACHE_TOMBSTONE_COUNT) {
    laghu_runtime_shared_mapping_unlock(&state->mapping);
    laghu_runtime_shared_mapping_close(&state->mapping);
    free(state);
    return false;
  }
  state->tombstones = (laghu_cache_tombstone *)(void *)(state->header + 1);
  slots =
      (state->mapping.mapping_length - sizeof(*state->header) - sizeof(laghu_cache_tombstone) * LAGHU_CACHE_TOMBSTONE_COUNT) / sizeof(*state->slots);
  state->slots = (laghu_cache_index_slot *)(void *)(state->tombstones + LAGHU_CACHE_TOMBSTONE_COUNT);
  state->slot_count = (uint32_t)slots;
  initialize = state->header->magic == 0U && state->header->version == 0U;
  if (initialize) {
    memset(state->mapping.mapping, 0, state->mapping.mapping_length);
    state->header->magic = LAGHU_CACHE_INDEX_MAGIC;
    state->header->version = LAGHU_CACHE_INDEX_VERSION;
    state->header->slot_count = (uint32_t)slots;
    state->header->size_limit = backend->limits.size_limit;
    state->header->inode_limit = backend->limits.inode_limit;
    state->header->clean_interval = backend->limits.clean_interval;
    state->header->cache_generation = 1U;
    state->header->rebuilding = 1U;
    (void)laghu_runtime_shared_mapping_sync(&state->mapping);
    state->header->rebuilding = 0U;
  } else if (!laghu_cache_index_valid(backend, state)) {
    if (state->header->magic == LAGHU_CACHE_INDEX_MAGIC && state->header->version != LAGHU_CACHE_INDEX_VERSION) {
      laghu_cache_index_recover(backend, state);
      (void)laghu_runtime_shared_mapping_sync(&state->mapping);
    } else {
      laghu_runtime_shared_mapping_unlock(&state->mapping);
      laghu_runtime_shared_mapping_close(&state->mapping);
      free(state);
      return false;
    }
  }
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  backend->implementation = state;
  return slots > 0U;
}

static void laghu_cache_record_hit(laghu_cache_backend *backend, const laghu_runtime_cache_entry *entry, bool hit) {
  laghu_file_cache_state *state = backend->implementation;
  laghu_cache_index_slot *slot;
  uint64_t now = (uint64_t)time(NULL);
  if (state == NULL || !laghu_runtime_shared_mapping_try_lock(&state->mapping)) return;
  if (!laghu_cache_index_valid(backend, state)) laghu_cache_index_recover(backend, state);
  if (hit) {
    ++state->header->hits;
    slot = laghu_cache_slot(state, entry->variant_key, false);
    if (slot != NULL && now >= slot->accessed_at + 60U) slot->accessed_at = now;
  } else {
    ++state->header->misses;
  }
  laghu_runtime_shared_mapping_unlock(&state->mapping);
}

static bool laghu_cache_file_lookup(laghu_cache_backend *backend, const char *index_key, const char *validator, laghu_runtime_cache_entry *entry) {
  laghu_file_cache_state *state = backend->implementation;
  laghu_cache_index_slot *slot = laghu_cache_slot(state, index_key, false);
  bool visible = slot == NULL || slot->generation >= laghu_cache_source_generation(state, slot->source_hash);
  bool found = visible && laghu_runtime_file_cache_lookup(backend->path, index_key, validator, entry);
  laghu_cache_record_hit(backend, entry, found);
  return found;
}

static bool laghu_cache_file_lookup_variant(laghu_cache_backend *backend, const char *variant_key, laghu_runtime_cache_entry *entry) {
  laghu_file_cache_state *state = backend->implementation;
  laghu_cache_index_slot *slot = laghu_cache_slot(state, variant_key, false);
  bool visible = slot == NULL || slot->generation >= laghu_cache_source_generation(state, slot->source_hash);
  bool found = visible && laghu_runtime_file_cache_lookup_variant(backend->path, variant_key, entry);
  laghu_cache_record_hit(backend, entry, found);
  return found;
}

static bool laghu_cache_file_read(laghu_cache_backend *backend, const laghu_runtime_cache_entry *entry, unsigned char *output,
                                  size_t output_capacity) {
  (void)backend;
  return laghu_runtime_file_cache_read(entry, output, output_capacity);
}

static bool laghu_cache_file_publish(laghu_cache_backend *backend, const char *index_key, const char *variant_key, const char *validator,
                                     const char *content_type, const char *backend_id, laghu_buffer payload, laghu_runtime_cache_entry *entry) {
  laghu_file_cache_state *state = backend->implementation;
  laghu_cache_index_slot *slot;
  bool existing;
  bool published;
  uint32_t files = strcmp(index_key, variant_key) == 0 ? 2U : 3U;
  uint64_t now = (uint64_t)time(NULL);
  char source_hash[LAGHU_RUNTIME_KEY_SIZE] = "";
  uint64_t publication_generation;
  if (state == NULL || !laghu_runtime_shared_mapping_try_lock(&state->mapping)) return false;
  if (!laghu_cache_index_valid(backend, state)) laghu_cache_index_recover(backend, state);
  slot = laghu_cache_slot(state, variant_key, true);
  {
    laghu_cache_index_slot *association = laghu_cache_slot(state, index_key, false);
    if (association != NULL) (void)snprintf(source_hash, sizeof(source_hash), "%s", association->source_hash);
    if (source_hash[0] == '\0' && strcmp(backend->path, laghu_cache_scoped_path) == 0)
      (void)snprintf(source_hash, sizeof(source_hash), "%s", laghu_cache_scoped_source);
  }
  publication_generation = laghu_cache_source_generation(state, source_hash);
  existing = slot != NULL && slot->state == LAGHU_CACHE_SLOT_READY;
  if (slot != NULL && slot->state == LAGHU_CACHE_SLOT_PUBLISHING) slot = NULL;
  if (!existing && source_hash[0] != '\0') {
    unsigned int index, variants = 0U;
    for (index = 0U; index < state->slot_count; ++index)
      if ((state->slots[index].state == LAGHU_CACHE_SLOT_READY || state->slots[index].state == LAGHU_CACHE_SLOT_PUBLISHING) &&
          strcmp(state->slots[index].source_hash, source_hash) == 0)
        ++variants;
    if (variants >= laghu_cache_scoped_variant_limit) slot = NULL;
  }
  if (slot == NULL ||
      (!existing && (state->header->bytes > state->header->size_limit || payload.length > state->header->size_limit - state->header->bytes ||
                     state->header->files + files > state->header->inode_limit))) {
    ++state->header->rejected_publications;
    laghu_runtime_shared_mapping_unlock(&state->mapping);
    return false;
  }
  if (!existing) {
    memset(slot, 0, sizeof(*slot));
    slot->state = LAGHU_CACHE_SLOT_PUBLISHING;
    slot->accessed_at = now;
    (void)snprintf(slot->variant_key, sizeof(slot->variant_key), "%s", variant_key);
    (void)snprintf(slot->source_hash, sizeof(slot->source_hash), "%s", source_hash);
  }
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  published = laghu_runtime_file_cache_publish(backend->path, index_key, variant_key, validator, content_type, backend_id, payload, entry);
  if (!laghu_runtime_shared_mapping_try_lock(&state->mapping)) return published;
  if (!published) {
    slot = laghu_cache_slot(state, variant_key, false);
    if (slot != NULL && slot->state == LAGHU_CACHE_SLOT_PUBLISHING) slot->state = LAGHU_CACHE_SLOT_DELETED;
    laghu_runtime_shared_mapping_unlock(&state->mapping);
    return false;
  }
  if (publication_generation != laghu_cache_source_generation(state, source_hash)) {
    laghu_runtime_shared_mapping_unlock(&state->mapping);
    (void)laghu_runtime_file_remove(entry->variant_path);
    return false;
  }
  slot = laghu_cache_slot(state, variant_key, true);
  if (slot != NULL) {
    if (slot->state != LAGHU_CACHE_SLOT_READY) {
      state->header->bytes += payload.length;
      state->header->files += files;
    }
    memset(slot, 0, sizeof(*slot));
    slot->state = LAGHU_CACHE_SLOT_READY;
    slot->file_count = files;
    slot->length = payload.length;
    slot->accessed_at = now;
    (void)snprintf(slot->index_key, sizeof(slot->index_key), "%s", index_key);
    (void)snprintf(slot->variant_key, sizeof(slot->variant_key), "%s", variant_key);
    (void)snprintf(slot->payload_hash, sizeof(slot->payload_hash), "%s", entry->payload_hash);
    (void)snprintf(slot->source_hash, sizeof(slot->source_hash), "%s", source_hash);
    slot->generation = publication_generation;
    ++state->header->publications;
  }
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  return true;
}

static bool laghu_cache_file_touch(laghu_cache_backend *backend, const char *variant_key, uint64_t now) {
  laghu_file_cache_state *state = backend->implementation;
  laghu_cache_index_slot *slot;
  if (state == NULL || !laghu_runtime_shared_mapping_try_lock(&state->mapping)) return false;
  if (!laghu_cache_index_valid(backend, state)) laghu_cache_index_recover(backend, state);
  slot = laghu_cache_slot(state, variant_key, false);
  if (slot != NULL && now >= slot->accessed_at + 60U) slot->accessed_at = now;
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  return slot != NULL;
}

static bool laghu_cache_file_remove(laghu_cache_backend *backend, const char *variant_key) {
  laghu_file_cache_state *state = backend->implementation;
  laghu_cache_index_slot *slot;
  bool removed;
  if (state == NULL || !laghu_runtime_shared_mapping_try_lock(&state->mapping)) return false;
  if (!laghu_cache_index_valid(backend, state)) laghu_cache_index_recover(backend, state);
  slot = laghu_cache_slot(state, variant_key, false);
  removed = laghu_cache_delete_slot(backend, slot);
  if (removed) {
    state->header->bytes = state->header->bytes >= slot->length ? state->header->bytes - slot->length : 0U;
    state->header->files = state->header->files >= slot->file_count ? state->header->files - slot->file_count : 0U;
    memset(slot, 0, sizeof(*slot));
    slot->state = LAGHU_CACHE_SLOT_DELETED;
  }
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  return removed;
}

static bool laghu_cache_file_maintain(laghu_cache_backend *backend, uint64_t now) {
  laghu_file_cache_state *state = backend->implementation;
  laghu_cache_index_slot **candidates;
  uint64_t target_bytes;
  uint64_t target_files;
  size_t count = 0U;
  size_t index;
  if (state == NULL || !laghu_runtime_shared_mapping_try_lock(&state->mapping)) return false;
  if (!laghu_cache_index_valid(backend, state)) laghu_cache_index_recover(backend, state);
  if (state->header->cleaner_lease_until > now ||
      (state->header->last_cleanup != 0U && now < state->header->last_cleanup + backend->limits.clean_interval)) {
    laghu_runtime_shared_mapping_unlock(&state->mapping);
    return true;
  }
  state->header->cleaner_lease_until = now + backend->limits.clean_interval;
  target_bytes = backend->limits.size_limit * LAGHU_CACHE_LOW_WATER_PERCENT / 100U;
  target_files = backend->limits.inode_limit * LAGHU_CACHE_LOW_WATER_PERCENT / 100U;
  candidates = calloc(state->header->slot_count, sizeof(*candidates));
  if (candidates == NULL) {
    state->header->cleaner_lease_until = 0U;
    laghu_runtime_shared_mapping_unlock(&state->mapping);
    return false;
  }
  for (index = 0U; index < state->header->slot_count; ++index)
    if (state->slots[index].state == LAGHU_CACHE_SLOT_PUBLISHING && state->slots[index].accessed_at < now &&
        now - state->slots[index].accessed_at > backend->limits.clean_interval) {
      memset(&state->slots[index], 0, sizeof(state->slots[index]));
      state->slots[index].state = LAGHU_CACHE_SLOT_DELETED;
    } else if (state->slots[index].state == LAGHU_CACHE_SLOT_READY &&
               state->slots[index].generation < laghu_cache_source_generation(state, state->slots[index].source_hash)) {
      laghu_cache_index_slot *slot = &state->slots[index];
      if (laghu_cache_delete_slot(backend, slot)) {
        state->header->bytes = state->header->bytes >= slot->length ? state->header->bytes - slot->length : 0U;
        state->header->files = state->header->files >= slot->file_count ? state->header->files - slot->file_count : 0U;
        memset(slot, 0, sizeof(*slot));
        slot->state = LAGHU_CACHE_SLOT_DELETED;
      }
    } else if (state->slots[index].state == LAGHU_CACHE_SLOT_READY)
      candidates[count++] = &state->slots[index];
  qsort(candidates, count, sizeof(*candidates), laghu_cache_slot_compare);
  for (index = 0U; index < count && (state->header->bytes > target_bytes || state->header->files > target_files); ++index) {
    laghu_cache_index_slot *slot = candidates[index];
    if (!laghu_cache_delete_slot(backend, slot)) continue;
    state->header->bytes = state->header->bytes >= slot->length ? state->header->bytes - slot->length : 0U;
    state->header->files = state->header->files >= slot->file_count ? state->header->files - slot->file_count : 0U;
    ++state->header->evictions;
    memset(slot, 0, sizeof(*slot));
    slot->state = LAGHU_CACHE_SLOT_DELETED;
  }
  free(candidates);
  state->header->last_cleanup = now;
  state->header->cleaner_lease_until = 0U;
  (void)laghu_runtime_shared_mapping_sync(&state->mapping);
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  return true;
}

static bool laghu_cache_file_health(laghu_cache_backend *backend, laghu_cache_stats *stats) {
  laghu_file_cache_state *state;
  if (backend == NULL || stats == NULL || backend->implementation == NULL) return false;
  state = backend->implementation;
  if (!laghu_runtime_shared_mapping_try_lock(&state->mapping)) return false;
  if (!laghu_cache_index_valid(backend, state)) laghu_cache_index_recover(backend, state);
  stats->bytes = state->header->bytes;
  stats->files = state->header->files;
  stats->hits = state->header->hits;
  stats->misses = state->header->misses;
  stats->rejected_publications = state->header->rejected_publications;
  {
    unsigned int index;
    uint64_t occupancy = 0U;
    for (index = 0U; index < state->slot_count; ++index)
      if (state->slots[index].state == LAGHU_CACHE_SLOT_READY || state->slots[index].state == LAGHU_CACHE_SLOT_PUBLISHING) ++occupancy;
    stats->variant_occupancy = occupancy;
  }
  stats->evictions = state->header->evictions;
  stats->corrupt_removals = state->header->corrupt_removals;
  stats->publications = state->header->publications;
  stats->url_purges = state->header->url_purges;
  stats->full_purges = state->header->full_purges;
  stats->invalidated_artifacts = state->header->invalidated_artifacts;
  stats->invalidated_bytes = state->header->invalidated_bytes;
  stats->cache_generation = state->header->cache_generation;
  stats->last_purge = state->header->last_purge;
  stats->last_cleanup = state->header->last_cleanup;
  stats->cleaner_active = state->header->cleaner_lease_until != 0U;
  stats->rebuilding = state->header->rebuilding != 0U;
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  return true;
}

static void laghu_cache_file_close(laghu_cache_backend *backend) {
  laghu_file_cache_state *state;
  if (backend == NULL || backend->implementation == NULL) return;
  state = backend->implementation;
  laghu_runtime_shared_mapping_close(&state->mapping);
  free(state);
  backend->implementation = NULL;
}

static const laghu_cache_backend_contract laghu_cache_file_contract = {
    laghu_cache_file_lookup, laghu_cache_file_lookup_variant, laghu_cache_file_read,   laghu_cache_file_publish, laghu_cache_file_touch,
    laghu_cache_file_remove, laghu_cache_file_maintain,       laghu_cache_file_health, laghu_cache_file_close};

static int laghu_hex(unsigned char value) {
  if (value >= '0' && value <= '9') return (int)(value - '0');
  value = (unsigned char)tolower(value);
  if (value >= 'a' && value <= 'f') return (int)(value - 'a') + 10;
  return -1;
}

void laghu_cache_limits_init(laghu_cache_limits *limits) {
  if (limits == NULL) return;
  limits->size_limit = LAGHU_CACHE_DEFAULT_SIZE_BYTES;
  limits->inode_limit = LAGHU_CACHE_DEFAULT_INODE_LIMIT;
  limits->metadata_size = LAGHU_CACHE_DEFAULT_METADATA_BYTES;
  limits->clean_interval = LAGHU_CACHE_DEFAULT_CLEAN_INTERVAL;
}

bool laghu_cache_size_parse(const char *value, uint64_t minimum, uint64_t maximum, uint64_t *result) {
  char *end = NULL;
  unsigned long long parsed;
  uint64_t multiplier = 1U;
  if (value == NULL || value[0] == '\0' || value[0] == '-' || result == NULL) return false;
  errno = 0;
  parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value) return false;
  if (*end != '\0') {
    if (end[1] != '\0') return false;
    switch (tolower((unsigned char)*end)) {
      case 'k':
        multiplier = 1024U;
        break;
      case 'm':
        multiplier = 1024U * 1024U;
        break;
      case 'g':
        multiplier = UINT64_C(1024) * 1024U * 1024U;
        break;
      default:
        return false;
    }
  }
  if ((uint64_t)parsed > UINT64_MAX / multiplier) return false;
  *result = (uint64_t)parsed * multiplier;
  return *result >= minimum && *result <= maximum;
}

bool laghu_cache_count_parse(const char *value, uint64_t minimum, uint64_t maximum, uint64_t *result) {
  char *end = NULL;
  unsigned long long parsed;
  if (value == NULL || value[0] == '\0' || value[0] == '-' || result == NULL) return false;
  errno = 0;
  parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < minimum || parsed > maximum) return false;
  *result = (uint64_t)parsed;
  return true;
}

bool laghu_cache_duration_parse(const char *value, unsigned int minimum, unsigned int maximum, unsigned int *result) {
  uint64_t parsed;
  uint64_t multiplier = 1U;
  char *end = NULL;
  if (value == NULL || value[0] == '\0' || value[0] == '-' || result == NULL) return false;
  errno = 0;
  parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value) return false;
  if (*end != '\0') {
    if (end[1] != '\0') return false;
    switch (tolower((unsigned char)*end)) {
      case 's':
        break;
      case 'm':
        multiplier = 60U;
        break;
      case 'h':
        multiplier = 3600U;
        break;
      default:
        return false;
    }
  }
  if (parsed > UINT64_MAX / multiplier) return false;
  parsed *= multiplier;
  if (parsed < minimum || parsed > maximum || parsed > UINT_MAX) return false;
  *result = (unsigned int)parsed;
  return true;
}

bool laghu_cache_backend_uri_parse(const char *uri, char *path, size_t path_size) {
  const char *input;
  size_t used = 0U;
  if (uri == NULL || path == NULL || path_size == 0U || strncmp(uri, "file:///", 8U) != 0 || uri[8] == '\0' || strchr(uri + 8U, '?') != NULL ||
      strchr(uri + 8U, '#') != NULL) {
    return false;
  }
  input = uri + 7U;
  while (*input != '\0') {
    unsigned char value = (unsigned char)*input++;
    if (value == '%') {
      int high;
      int low;
      if (input[0] == '\0' || input[1] == '\0' || (high = laghu_hex((unsigned char)input[0])) < 0 || (low = laghu_hex((unsigned char)input[1])) < 0) {
        return false;
      }
      value = (unsigned char)((high << 4) | low);
      input += 2;
      if (value == '\0' || value == '/' || value == '\\') return false;
    }
    if (value < 0x20U || used + 1U >= path_size) return false;
    path[used++] = (char)value;
  }
  path[used] = '\0';
  return used > 1U && path[0] == '/';
}

bool laghu_cache_backend_open(laghu_cache_backend *backend, const char *uri, const laghu_cache_limits *limits) {
  laghu_cache_limits defaults;
  if (backend == NULL || uri == NULL || strlen(uri) >= sizeof(backend->uri)) return false;
  memset(backend, 0, sizeof(*backend));
  if (!laghu_cache_backend_uri_parse(uri, backend->path, sizeof(backend->path))) return false;
  laghu_cache_limits_init(&defaults);
  backend->limits = limits != NULL ? *limits : defaults;
  if (backend->limits.size_limit == 0U || backend->limits.inode_limit == 0U || backend->limits.metadata_size < 16384U ||
      backend->limits.clean_interval == 0U)
    return false;
  (void)snprintf(backend->uri, sizeof(backend->uri), "%s", uri);
  backend->contract = &laghu_cache_file_contract;
  if (!laghu_cache_metadata_open(backend)) {
    memset(backend, 0, sizeof(*backend));
    return false;
  }
  return true;
}

bool laghu_cache_backend_open_path(laghu_cache_backend *backend, const char *path, const laghu_cache_limits *limits) {
  char uri[LAGHU_RUNTIME_PATH_SIZE];
  int written;
  if (backend == NULL || path == NULL || path[0] == '\0') return false;
  if (path[0] != '/') return false;
  written = snprintf(uri, sizeof(uri), "file://%s", path);
  return written > 0 && (size_t)written < sizeof(uri) && laghu_cache_backend_open(backend, uri, limits);
}

bool laghu_cache_backend_register_path(const char *path, const laghu_cache_limits *limits) {
  size_t index;
  laghu_cache_limits expected;
  laghu_cache_limits_init(&expected);
  if (limits != NULL) {
    expected = *limits;
  } else {
    char metadata_path[LAGHU_RUNTIME_PATH_SIZE];
    uint64_t metadata_size;
    laghu_cache_index_header header;
    FILE *file;
    if (laghu_cache_metadata_path(metadata_path, sizeof(metadata_path), path) && laghu_runtime_file_size(metadata_path, &metadata_size) &&
        metadata_size >= 16384U && metadata_size <= SIZE_MAX && (file = fopen(metadata_path, "rb")) != NULL) {
      bool read = fread(&header, sizeof(header), 1U, file) == 1U;
      (void)fclose(file);
      if (read && header.magic == LAGHU_CACHE_INDEX_MAGIC && header.version == LAGHU_CACHE_INDEX_VERSION && header.size_limit != 0U &&
          header.inode_limit != 0U && header.clean_interval != 0U) {
        expected.size_limit = header.size_limit;
        expected.inode_limit = header.inode_limit;
        expected.clean_interval = header.clean_interval;
        expected.metadata_size = (size_t)metadata_size;
      }
    }
  }
  if (path == NULL || path[0] == '\0') return false;
  for (index = 0U; index < laghu_cache_backend_count; ++index) {
    laghu_cache_backend *backend = &laghu_cache_backends[index];
    if (strcmp(backend->path, path) == 0)
      return backend->limits.size_limit == expected.size_limit && backend->limits.inode_limit == expected.inode_limit &&
             backend->limits.metadata_size == expected.metadata_size && backend->limits.clean_interval == expected.clean_interval;
  }
  if (laghu_cache_backend_count >= LAGHU_CACHE_BACKEND_REGISTRY_SIZE ||
      !laghu_cache_backend_open_path(&laghu_cache_backends[laghu_cache_backend_count], path, &expected))
    return false;
  ++laghu_cache_backend_count;
  return true;
}

static laghu_cache_backend *laghu_cache_backend_registered(const char *cache_path) {
  size_t index;
  for (index = 0U; cache_path != NULL && index < laghu_cache_backend_count; ++index)
    if (strcmp(laghu_cache_backends[index].path, cache_path) == 0) return &laghu_cache_backends[index];
  return NULL;
}

bool laghu_cache_backend_maintain_path(const char *path, uint64_t now) {
  laghu_cache_backend *backend = laghu_cache_backend_registered(path);
  return backend != NULL && laghu_cache_backend_maintain(backend, now);
}

bool laghu_cache_backend_health_path(const char *path, laghu_cache_stats *stats) {
  laghu_cache_backend *backend = laghu_cache_backend_registered(path);
  return backend != NULL && laghu_cache_backend_health(backend, stats);
}

bool laghu_cache_source_normalize(const char *target, char *normalized, size_t normalized_size, bool *purge_control) {
  const char *fragment;
  const char *query;
  size_t path_length;
  size_t used = 0U;
  bool found = false;
  if (purge_control != NULL) *purge_control = false;
  if (target == NULL || normalized == NULL || normalized_size < 2U || target[0] != '/' || target[1] == '/' || strchr(target, '\\') != NULL ||
      strchr(target, '@') != NULL)
    return false;
  fragment = strchr(target, '#');
  query = strchr(target, '?');
  if (query != NULL && fragment != NULL && fragment < query) return false;
  path_length = (size_t)((query != NULL ? query : (fragment != NULL ? fragment : target + strlen(target))) - target);
  if (path_length == 0U || path_length >= normalized_size) return false;
  memcpy(normalized, target, path_length);
  used = path_length;
  if (query != NULL) {
    const char *cursor = query + 1U;
    const char *end = fragment != NULL ? fragment : target + strlen(target);
    bool first = true;
    while (cursor <= end) {
      const char *amp = memchr(cursor, '&', (size_t)(end - cursor));
      const char *pair_end = amp != NULL ? amp : end;
      size_t pair_length = (size_t)(pair_end - cursor);
      const char *percent;
      for (percent = cursor; percent < pair_end; ++percent)
        if (*percent == '%' && (percent + 2U >= pair_end || !isxdigit((unsigned char)percent[1]) || !isxdigit((unsigned char)percent[2])))
          return false;
      if (pair_length == sizeof("laghu=purge") - 1U && memcmp(cursor, "laghu=purge", pair_length) == 0) {
        if (found) return false;
        found = true;
      } else if (pair_length != 0U) {
        if (used + pair_length + 2U > normalized_size) return false;
        normalized[used++] = first ? '?' : '&';
        memcpy(normalized + used, cursor, pair_length);
        used += pair_length;
        first = false;
      }
      if (amp == NULL) break;
      cursor = amp + 1U;
    }
  }
  normalized[used] = '\0';
  if (purge_control != NULL) *purge_control = found;
  return true;
}

bool laghu_cache_source_hash(const char *target, char output[LAGHU_RUNTIME_KEY_SIZE]) {
  char normalized[LAGHU_RUNTIME_PATH_SIZE];
  return laghu_cache_source_normalize(target, normalized, sizeof(normalized), NULL) &&
         laghu_sha256_hex((laghu_buffer){(const unsigned char *)normalized, strlen(normalized)}, output);
}

bool laghu_cache_backend_associate(laghu_cache_backend *backend, const char *index_key, const char *source_hash) {
  laghu_file_cache_state *state;
  laghu_cache_index_slot *slot;
  if (backend == NULL || backend->implementation == NULL || !laghu_cache_hash_valid(index_key) || !laghu_cache_hash_valid(source_hash)) return false;
  state = backend->implementation;
  if (!laghu_runtime_shared_mapping_try_lock(&state->mapping)) return false;
  if (!laghu_cache_index_valid(backend, state)) laghu_cache_index_recover(backend, state);
  slot = laghu_cache_slot(state, index_key, true);
  if (slot != NULL) {
    if (slot->state != LAGHU_CACHE_SLOT_READY) {
      memset(slot, 0, sizeof(*slot));
      slot->state = LAGHU_CACHE_SLOT_ASSOCIATION;
      (void)snprintf(slot->variant_key, sizeof(slot->variant_key), "%s", index_key);
      (void)snprintf(slot->index_key, sizeof(slot->index_key), "%s", index_key);
    }
    (void)snprintf(slot->source_hash, sizeof(slot->source_hash), "%s", source_hash);
    slot->generation = laghu_cache_source_generation(state, source_hash);
  }
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  return slot != NULL;
}

bool laghu_cache_backend_associate_path(const char *path, const char *index_key, const char *source_target) {
  char source_hash[LAGHU_RUNTIME_KEY_SIZE];
  laghu_cache_backend *backend = laghu_cache_backend_registered(path);
  return backend != NULL && laghu_cache_source_hash(source_target, source_hash) && laghu_cache_backend_associate(backend, index_key, source_hash);
}

void laghu_cache_source_scope(const char *path, const char *source_target) {
  laghu_cache_scoped_path[0] = '\0';
  laghu_cache_scoped_source[0] = '\0';
  if (path == NULL || strlen(path) >= sizeof(laghu_cache_scoped_path) || !laghu_cache_source_hash(source_target, laghu_cache_scoped_source)) return;
  (void)snprintf(laghu_cache_scoped_path, sizeof(laghu_cache_scoped_path), "%s", path);
}

void laghu_cache_variant_limit_scope(const char *path, unsigned int limit) {
  if (path != NULL && strcmp(path, laghu_cache_scoped_path) == 0 && limit >= LAGHU_VARIANTS_PER_SOURCE_MIN && limit <= LAGHU_VARIANTS_PER_SOURCE_MAX)
    laghu_cache_scoped_variant_limit = limit;
  else
    laghu_cache_scoped_variant_limit = LAGHU_VARIANTS_PER_SOURCE_DEFAULT;
}

laghu_cache_purge_result laghu_cache_backend_purge_url(laghu_cache_backend *backend, const char *source_target, uint64_t now,
                                                       uint64_t *matched_artifacts) {
  laghu_file_cache_state *state;
  char source_hash[LAGHU_RUNTIME_KEY_SIZE];
  laghu_cache_tombstone *available = NULL;
  uint64_t matched = 0U, bytes = 0U, next_generation;
  size_t index;
  if (matched_artifacts != NULL) *matched_artifacts = 0U;
  if (backend == NULL || backend->implementation == NULL || !laghu_cache_source_hash(source_target, source_hash)) return LAGHU_CACHE_PURGE_INVALID;
  state = backend->implementation;
  if (!laghu_runtime_shared_mapping_try_lock(&state->mapping)) return LAGHU_CACHE_PURGE_UNAVAILABLE;
  if (!laghu_cache_index_valid(backend, state)) laghu_cache_index_recover(backend, state);
  for (index = 0U; index < LAGHU_CACHE_TOMBSTONE_COUNT; ++index) {
    if (strcmp(state->tombstones[index].source_hash, source_hash) == 0) {
      available = &state->tombstones[index];
      break;
    }
    if (available == NULL && state->tombstones[index].source_hash[0] == '\0') available = &state->tombstones[index];
  }
  if (available == NULL) {
    laghu_runtime_shared_mapping_unlock(&state->mapping);
    return LAGHU_CACHE_PURGE_SATURATED;
  }
  next_generation = laghu_cache_source_generation(state, source_hash) + 1U;
  (void)snprintf(available->source_hash, sizeof(available->source_hash), "%s", source_hash);
  available->generation = next_generation;
  available->purged_at = now;
  for (index = 0U; index < state->slot_count; ++index)
    if (state->slots[index].state == LAGHU_CACHE_SLOT_READY && strcmp(state->slots[index].source_hash, source_hash) == 0) {
      ++matched;
      bytes += state->slots[index].length;
    }
  ++state->header->url_purges;
  state->header->invalidated_artifacts += matched;
  state->header->invalidated_bytes += bytes;
  state->header->last_purge = now;
  (void)laghu_runtime_shared_mapping_sync(&state->mapping);
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  if (matched_artifacts != NULL) *matched_artifacts = matched;
  return LAGHU_CACHE_PURGE_ACCEPTED;
}

laghu_cache_purge_result laghu_cache_backend_purge_url_path(const char *path, const char *source_target, uint64_t now, uint64_t *matched_artifacts) {
  laghu_cache_backend *backend = laghu_cache_backend_registered(path);
  return backend == NULL ? LAGHU_CACHE_PURGE_UNAVAILABLE : laghu_cache_backend_purge_url(backend, source_target, now, matched_artifacts);
}

bool laghu_cache_backend_flush(laghu_cache_backend *backend, uint64_t requested_generation, uint64_t now, uint64_t *generation) {
  laghu_file_cache_state *state;
  size_t index;
  uint64_t matched = 0U, bytes = 0U;
  if (backend == NULL || backend->implementation == NULL) return false;
  state = backend->implementation;
  if (!laghu_runtime_shared_mapping_try_lock(&state->mapping)) return false;
  if (!laghu_cache_index_valid(backend, state)) laghu_cache_index_recover(backend, state);
  if (requested_generation != 0U && requested_generation <= state->header->cache_generation) {
    laghu_runtime_shared_mapping_unlock(&state->mapping);
    return false;
  }
  state->header->cache_generation = requested_generation != 0U ? requested_generation : state->header->cache_generation + 1U;
  for (index = 0U; index < state->slot_count; ++index)
    if (state->slots[index].state == LAGHU_CACHE_SLOT_READY) {
      ++matched;
      bytes += state->slots[index].length;
    }
  ++state->header->full_purges;
  state->header->invalidated_artifacts += matched;
  state->header->invalidated_bytes += bytes;
  state->header->last_purge = now;
  (void)laghu_runtime_shared_mapping_sync(&state->mapping);
  if (generation != NULL) *generation = state->header->cache_generation;
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  return true;
}

bool laghu_cache_backend_flush_path(const char *path, uint64_t requested_generation, uint64_t now, uint64_t *generation) {
  laghu_cache_backend *backend = laghu_cache_backend_registered(path);
  return backend != NULL && laghu_cache_backend_flush(backend, requested_generation, now, generation);
}

bool laghu_cache_flush_file_poll(const char *path, const char *flush_file, uint64_t now, uint64_t *generation) {
  FILE *file;
  char line[128U];
  size_t line_length;
  unsigned long long requested;
  char trailing;
  if (path == NULL || flush_file == NULL || flush_file[0] == '\0') return false;
  {
    struct stat status;
    if (lstat(flush_file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & (S_IWGRP | S_IWOTH)) != 0U)
      return false;
  }
  file = fopen(flush_file, "rb");
  if (file == NULL || fgets(line, sizeof(line), file) == NULL) {
    if (file != NULL) (void)fclose(file);
    return false;
  }
  line_length = strlen(line);
  if (line_length == 0U || line[line_length - 1U] != '\n' || fgetc(file) != EOF || fclose(file) != 0) return false;
  line[--line_length] = '\0';
  if (line_length > 0U && line[line_length - 1U] == '\r') line[--line_length] = '\0';
  if (line_length == 0U || sscanf(line, "laghu-cache-flush-v1 %llu%c", &requested, &trailing) != 1 || requested == 0U) return false;
  return laghu_cache_backend_flush_path(path, (uint64_t)requested, now, generation);
}

bool laghu_runtime_cache_publish(const char *cache_path, const char *index_key, const char *variant_key, const char *validator,
                                 const char *content_type, const char *backend_id, laghu_buffer payload, laghu_runtime_cache_entry *entry) {
  laghu_cache_backend *backend = laghu_cache_backend_registered(cache_path);
  return backend != NULL ? laghu_cache_backend_publish(backend, index_key, variant_key, validator, content_type, backend_id, payload, entry)
                         : laghu_runtime_file_cache_publish(cache_path, index_key, variant_key, validator, content_type, backend_id, payload, entry);
}

bool laghu_runtime_cache_lookup(const char *cache_path, const char *index_key, const char *validator, laghu_runtime_cache_entry *entry) {
  laghu_cache_backend *backend = laghu_cache_backend_registered(cache_path);
  return backend != NULL ? laghu_cache_backend_lookup(backend, index_key, validator, entry)
                         : laghu_runtime_file_cache_lookup(cache_path, index_key, validator, entry);
}

bool laghu_runtime_cache_lookup_variant(const char *cache_path, const char *variant_key, laghu_runtime_cache_entry *entry) {
  laghu_cache_backend *backend = laghu_cache_backend_registered(cache_path);
  return backend != NULL ? laghu_cache_backend_lookup_variant(backend, variant_key, entry)
                         : laghu_runtime_file_cache_lookup_variant(cache_path, variant_key, entry);
}

bool laghu_runtime_cache_read(const laghu_runtime_cache_entry *entry, unsigned char *output, size_t output_capacity) {
  size_t index;
  if (entry != NULL) {
    for (index = 0U; index < laghu_cache_backend_count; ++index) {
      size_t length = strlen(laghu_cache_backends[index].path);
      if (strncmp(entry->variant_path, laghu_cache_backends[index].path, length) == 0 &&
          (entry->variant_path[length] == '/' || entry->variant_path[length] == '\\'))
        return laghu_cache_backend_read(&laghu_cache_backends[index], entry, output, output_capacity);
    }
  }
  return laghu_runtime_file_cache_read(entry, output, output_capacity);
}

void laghu_cache_backend_close(laghu_cache_backend *backend) {
  if (backend == NULL) return;
  if (backend->contract != NULL && backend->contract->close != NULL) backend->contract->close(backend);
  memset(backend, 0, sizeof(*backend));
}

bool laghu_cache_backend_lookup(laghu_cache_backend *backend, const char *index_key, const char *validator, laghu_runtime_cache_entry *entry) {
  return backend != NULL && backend->contract != NULL && backend->contract->lookup != NULL &&
         backend->contract->lookup(backend, index_key, validator, entry);
}

bool laghu_cache_backend_lookup_variant(laghu_cache_backend *backend, const char *variant_key, laghu_runtime_cache_entry *entry) {
  return backend != NULL && backend->contract != NULL && backend->contract->lookup_variant != NULL &&
         backend->contract->lookup_variant(backend, variant_key, entry);
}

bool laghu_cache_backend_read(laghu_cache_backend *backend, const laghu_runtime_cache_entry *entry, unsigned char *output, size_t output_capacity) {
  return backend != NULL && backend->contract != NULL && backend->contract->read != NULL &&
         backend->contract->read(backend, entry, output, output_capacity);
}

bool laghu_cache_backend_publish(laghu_cache_backend *backend, const char *index_key, const char *variant_key, const char *validator,
                                 const char *content_type, const char *backend_id, laghu_buffer payload, laghu_runtime_cache_entry *entry) {
  return backend != NULL && backend->contract != NULL && backend->contract->publish != NULL &&
         backend->contract->publish(backend, index_key, variant_key, validator, content_type, backend_id, payload, entry);
}

bool laghu_cache_backend_maintain(laghu_cache_backend *backend, uint64_t now) {
  return backend != NULL && backend->contract != NULL && backend->contract->maintain != NULL && backend->contract->maintain(backend, now);
}

bool laghu_cache_backend_health(laghu_cache_backend *backend, laghu_cache_stats *stats) {
  return backend != NULL && backend->contract != NULL && backend->contract->health != NULL && backend->contract->health(backend, stats);
}
