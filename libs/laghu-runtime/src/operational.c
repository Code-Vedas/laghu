// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/operational.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/budget.h"
#include "laghu/cache.h"
#include "laghu/lcp.h"
#include "laghu/types.h"
#include "persisted_state_wire.h"
#include "runtime_platform.h"

typedef uint64_t laghu_local_atomic;
typedef unsigned int laghu_local_flag;

typedef struct {
  laghu_runtime_shared_mapping mapping;
  unsigned char *bytes;
  unsigned int slot;
  uint64_t generation;
  laghu_local_flag healthy;
  laghu_local_flag publishing;
  laghu_local_atomic values[LAGHU_WIRE_OPERATIONAL_SLOT_LATENCY_SUM_OFFSET / 8U + 1U];
} laghu_operational_state;

#define LAGHU_OPERATIONAL_STATE(registry) ((laghu_operational_state *)(registry)->implementation)

static void laghu_operational_state_release(laghu_operational_registry *registry) {
  laghu_operational_state *state;
  if (registry == NULL || registry->implementation == NULL) return;
  state = LAGHU_OPERATIONAL_STATE(registry);
  laghu_runtime_shared_mapping_close(&state->mapping);
  free(state);
  registry->implementation = NULL;
}

static const uint64_t laghu_latency_limits[] = {1000U, 5000U, 10000U, 25000U, 50000U, 100000U, 250000U, 500000U, 1000000U, 2500000U, 5000000U};

static size_t laghu_local_index(size_t wire_offset) { return wire_offset / 8U; }

static uint64_t laghu_local_atomic_load(const laghu_local_atomic *value) { return __atomic_load_n(value, __ATOMIC_RELAXED); }

static void laghu_local_atomic_store(laghu_local_atomic *value, uint64_t replacement) { __atomic_store_n(value, replacement, __ATOMIC_RELAXED); }

static bool laghu_local_atomic_compare_exchange(laghu_local_atomic *value, uint64_t expected, uint64_t replacement) {
  return __atomic_compare_exchange_n(value, &expected, replacement, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

static bool laghu_local_flag_try_set(laghu_local_flag *value) {
  unsigned int expected = 0U;
  return __atomic_compare_exchange_n(value, &expected, 1U, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void laghu_local_flag_clear(laghu_local_flag *value) { __atomic_store_n(value, 0U, __ATOMIC_RELEASE); }

static void laghu_local_flag_store(laghu_local_flag *value, bool replacement) { __atomic_store_n(value, replacement ? 1U : 0U, __ATOMIC_RELAXED); }

static bool laghu_local_flag_load(const laghu_local_flag *value) { return __atomic_load_n(value, __ATOMIC_RELAXED) != 0U; }

static uint64_t laghu_local_load(const laghu_operational_state *state, size_t wire_offset) {
  return laghu_local_atomic_load(&state->values[laghu_local_index(wire_offset)]);
}

static void laghu_local_store(laghu_operational_state *state, size_t wire_offset, uint64_t value) {
  laghu_local_atomic_store(&state->values[laghu_local_index(wire_offset)], value);
}

static void laghu_local_add(laghu_operational_state *state, size_t wire_offset, uint64_t increment) {
  laghu_local_atomic *value = &state->values[laghu_local_index(wire_offset)];
  uint64_t current = laghu_local_atomic_load(value);
  while (!laghu_local_atomic_compare_exchange(value, current, UINT64_MAX - current < increment ? UINT64_MAX : current + increment)) {
    current = laghu_local_atomic_load(value);
  }
}

static void laghu_local_max(laghu_operational_state *state, size_t wire_offset, uint64_t candidate) {
  laghu_local_atomic *value = &state->values[laghu_local_index(wire_offset)];
  uint64_t current = laghu_local_atomic_load(value);
  while (candidate > current && !laghu_local_atomic_compare_exchange(value, current, candidate)) {
    current = laghu_local_atomic_load(value);
  }
}

static uint64_t laghu_load(const unsigned char *value) { return laghu_wire_u64_read(value); }

static void laghu_store(unsigned char *value, uint64_t replacement) { laghu_wire_u64_write(value, replacement); }

static unsigned char *laghu_header_generation(unsigned char *bytes) { return bytes + LAGHU_WIRE_OPERATIONAL_HEADER_GENERATION_OFFSET; }

static unsigned char *laghu_slot_at(unsigned char *bytes, unsigned int index) {
  return bytes + LAGHU_WIRE_OPERATIONAL_HEADER_SIZE + (size_t)index * LAGHU_WIRE_OPERATIONAL_SLOT_SIZE;
}

static unsigned char *laghu_slot_field(unsigned char *slot, size_t offset) { return slot + offset; }

static bool laghu_slot_reserved_valid(const unsigned char *slot) {
  size_t index;
  for (index = LAGHU_WIRE_OPERATIONAL_SLOT_LATENCY_SUM_OFFSET + 8U; index < LAGHU_WIRE_OPERATIONAL_SLOT_SIZE; ++index)
    if (slot[index] != 0U) return false;
  return true;
}

static bool laghu_slot_values_valid(const unsigned char *slot) {
  uint32_t active = laghu_wire_u32_read(slot + LAGHU_WIRE_OPERATIONAL_SLOT_ACTIVE_OFFSET);
  uint32_t surface = laghu_wire_u32_read(slot + LAGHU_WIRE_OPERATIONAL_SLOT_SURFACE_OFFSET);
  uint32_t kind = laghu_wire_u32_read(slot + LAGHU_WIRE_OPERATIONAL_SLOT_PROCESS_KIND_OFFSET);
  uint64_t capacity = laghu_wire_u64_read(slot + LAGHU_WIRE_OPERATIONAL_SLOT_QUEUE_CAPACITY_OFFSET);
  uint64_t occupied = laghu_wire_u64_read(slot + LAGHU_WIRE_OPERATIONAL_SLOT_QUEUE_OCCUPIED_OFFSET);
  if (!laghu_slot_reserved_valid(slot) || active > 1U || slot[LAGHU_WIRE_OPERATIONAL_SLOT_REQUIRED_OFFSET] > 1U ||
      slot[LAGHU_WIRE_OPERATIONAL_SLOT_HEALTHY_OFFSET] > 1U || occupied > capacity)
    return false;
  return active == 0U || (surface < LAGHU_OPERATIONAL_SURFACE_COUNT && kind < LAGHU_OPERATIONAL_PROCESS_COUNT);
}

static bool laghu_header_valid(const laghu_operational_state *state) {
  const unsigned char *bytes;
  if (state == NULL || state->bytes == NULL ||
      state->mapping.mapping_length !=
          (size_t)LAGHU_WIRE_OPERATIONAL_HEADER_SIZE + (size_t)LAGHU_WIRE_OPERATIONAL_SLOT_COUNT * LAGHU_WIRE_OPERATIONAL_SLOT_SIZE)
    return false;
  bytes = state->bytes;
  return laghu_wire_u64_read(bytes + LAGHU_WIRE_OPERATIONAL_HEADER_MAGIC_OFFSET) == LAGHU_WIRE_OPERATIONAL_MAGIC &&
         laghu_wire_u32_read(bytes + LAGHU_WIRE_OPERATIONAL_HEADER_VERSION_OFFSET) == LAGHU_WIRE_OPERATIONAL_VERSION &&
         laghu_wire_u32_read(bytes + LAGHU_WIRE_OPERATIONAL_HEADER_SLOT_COUNT_OFFSET) == LAGHU_WIRE_OPERATIONAL_SLOT_COUNT;
}

static bool laghu_bytes_zero(const unsigned char *bytes, size_t length) {
  size_t index;
  for (index = 0U; index < length; ++index)
    if (bytes[index] != 0U) return false;
  return true;
}

static bool laghu_slot_update_begin(unsigned char *slot, uint64_t *sequence) {
  if (slot == NULL || sequence == NULL) return false;
  *sequence = laghu_wire_u64_read(laghu_slot_field(slot, LAGHU_WIRE_OPERATIONAL_SLOT_SEQUENCE_OFFSET));
  if ((*sequence & 1U) != 0U || *sequence > UINT64_MAX - 2U) return false;
  laghu_wire_u64_write(laghu_slot_field(slot, LAGHU_WIRE_OPERATIONAL_SLOT_SEQUENCE_OFFSET), *sequence + 1U);
  return true;
}

static void laghu_slot_update_finish(unsigned char *slot, uint64_t sequence) {
  laghu_wire_u64_write(laghu_slot_field(slot, LAGHU_WIRE_OPERATIONAL_SLOT_SEQUENCE_OFFSET), sequence + 2U);
}

static unsigned char *laghu_slot(laghu_operational_registry *registry) {
  laghu_operational_state *state = registry != NULL ? LAGHU_OPERATIONAL_STATE(registry) : NULL;
  return state != NULL && state->bytes != NULL && state->slot < LAGHU_WIRE_OPERATIONAL_SLOT_COUNT ? laghu_slot_at(state->bytes, state->slot) : NULL;
}

static bool laghu_slot_owned_locked(laghu_operational_state *state, unsigned char **slot) {
  unsigned char *value =
      state != NULL && state->bytes != NULL && state->slot < LAGHU_WIRE_OPERATIONAL_SLOT_COUNT ? laghu_slot_at(state->bytes, state->slot) : NULL;
  if (slot != NULL) *slot = NULL;
  if (state == NULL || !laghu_header_valid(state) || value == NULL ||
      laghu_wire_u64_read(value + LAGHU_WIRE_OPERATIONAL_SLOT_GENERATION_OFFSET) != state->generation ||
      laghu_wire_u32_read(value + LAGHU_WIRE_OPERATIONAL_SLOT_ACTIVE_OFFSET) == 0U)
    return false;
  if (slot != NULL) *slot = value;
  return true;
}

static void laghu_slot_decode(const unsigned char *source, laghu_operational_slot_snapshot *target) {
  unsigned int index;
  memset(target, 0, sizeof(*target));
  target->generation = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_GENERATION_OFFSET);
  target->heartbeat = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_HEARTBEAT_OFFSET);
  target->active = laghu_wire_u32_read(source + LAGHU_WIRE_OPERATIONAL_SLOT_ACTIVE_OFFSET);
  target->surface = laghu_wire_u32_read(source + LAGHU_WIRE_OPERATIONAL_SLOT_SURFACE_OFFSET);
  target->process_kind = laghu_wire_u32_read(source + LAGHU_WIRE_OPERATIONAL_SLOT_PROCESS_KIND_OFFSET);
  target->required = source[LAGHU_WIRE_OPERATIONAL_SLOT_REQUIRED_OFFSET];
  target->healthy = source[LAGHU_WIRE_OPERATIONAL_SLOT_HEALTHY_OFFSET];
  target->queue_capacity = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_QUEUE_CAPACITY_OFFSET);
  target->queue_occupied = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_QUEUE_OCCUPIED_OFFSET);
  target->requests = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_REQUESTS_OFFSET);
  for (index = 0U; index < LAGHU_OPERATIONAL_DECISION_COUNT; ++index)
    target->decisions[index] = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_DECISIONS_OFFSET + index * 8U);
  target->cache_hits = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_CACHE_HITS_OFFSET);
  target->cache_misses = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_CACHE_MISSES_OFFSET);
  target->cache_publications = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_CACHE_PUBLICATIONS_OFFSET);
  target->cache_evictions = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_CACHE_EVICTIONS_OFFSET);
  target->cache_bytes = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_CACHE_BYTES_OFFSET);
  target->cache_files = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_CACHE_FILES_OFFSET);
  target->original_bytes = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_ORIGINAL_BYTES_OFFSET);
  target->selected_bytes = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_SELECTED_BYTES_OFFSET);
  target->saved_bytes = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_SAVED_BYTES_OFFSET);
  for (index = 0U; index < LAGHU_OPERATIONAL_FAILURE_COUNT; ++index)
    target->failures[index] = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_FAILURES_OFFSET + index * 8U);
  for (index = 0U; index < LAGHU_BUDGET_REJECTION_COUNT; ++index)
    target->budget_rejections[index] = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_BUDGET_REJECTIONS_OFFSET + index * 8U);
  target->transform_memory_current = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_MEMORY_CURRENT_OFFSET);
  target->transform_memory_peak = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_MEMORY_PEAK_OFFSET);
  target->transform_memory_limit = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_MEMORY_LIMIT_OFFSET);
  target->transform_deadline_limit_ms = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_DEADLINE_LIMIT_OFFSET);
  target->cache_rejected_writes = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_CACHE_REJECTED_WRITES_OFFSET);
  target->variant_occupancy = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_VARIANT_OCCUPANCY_OFFSET);
  target->variant_limit = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_VARIANT_LIMIT_OFFSET);
  for (index = 0U; index < 6U; ++index)
    target->lcp_decisions[index] = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_LCP_DECISIONS_OFFSET + index * 8U);
  target->lcp_applied = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_LCP_APPLIED_OFFSET);
  for (index = 0U; index < 4U; ++index) {
    target->lcp_profile_observations[index] = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_LCP_OBSERVATIONS_OFFSET + index * 8U);
    target->lcp_profile_ready[index] = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_LCP_READY_OFFSET + index * 8U);
  }
  for (index = 0U; index < LAGHU_OPERATIONAL_LATENCY_BUCKETS; ++index)
    target->latency_buckets[index] = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_LATENCY_BUCKETS_OFFSET + index * 8U);
  target->latency_count = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_LATENCY_COUNT_OFFSET);
  target->latency_sum_microseconds = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_LATENCY_SUM_OFFSET);
}

static void laghu_local_init(laghu_operational_state *state, uint64_t now) {
  size_t index;
  for (index = 0U; index < sizeof(state->values) / sizeof(state->values[0]); ++index) laghu_local_atomic_store(&state->values[index], 0U);
  laghu_local_flag_store(&state->healthy, true);
  laghu_local_flag_clear(&state->publishing);
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_HEARTBEAT_OFFSET, now);
}

static bool laghu_publish_locked(laghu_operational_state *state) {
  unsigned char *slot;
  uint64_t sequence;
  size_t offset;
  if (!laghu_slot_owned_locked(state, &slot) || !laghu_slot_update_begin(slot, &sequence)) return false;
  slot[LAGHU_WIRE_OPERATIONAL_SLOT_HEALTHY_OFFSET] = laghu_local_flag_load(&state->healthy) ? 1U : 0U;
  laghu_store(slot + LAGHU_WIRE_OPERATIONAL_SLOT_HEARTBEAT_OFFSET, laghu_local_load(state, LAGHU_WIRE_OPERATIONAL_SLOT_HEARTBEAT_OFFSET));
  for (offset = LAGHU_WIRE_OPERATIONAL_SLOT_QUEUE_CAPACITY_OFFSET; offset <= LAGHU_WIRE_OPERATIONAL_SLOT_LATENCY_SUM_OFFSET; offset += 8U)
    laghu_store(slot + offset, laghu_local_load(state, offset));
  laghu_slot_update_finish(slot, sequence);
  return true;
}

static bool laghu_publish(laghu_operational_state *state) {
  bool published = false;
  if (state == NULL || !laghu_local_flag_try_set(&state->publishing)) return false;
  if (laghu_runtime_shared_mapping_try_lock(&state->mapping)) {
    published = laghu_publish_locked(state);
    laghu_runtime_shared_mapping_unlock(&state->mapping);
  }
  laghu_local_flag_clear(&state->publishing);
  return published;
}

static bool laghu_path(const char *cache_path, char *output, size_t capacity) {
  size_t length;
  if (cache_path == NULL || output == NULL || capacity == 0U) return false;
  length = strlen(cache_path);
  if (length == 0U || length + sizeof(".laghu-operations-v4") > capacity) return false;
  return snprintf(output, capacity, "%s.laghu-operations-v4", cache_path) > 0;
}

void laghu_operational_registry_init(laghu_operational_registry *registry) {
  if (registry == NULL) return;
  memset(registry, 0, sizeof(*registry));
}

bool laghu_operational_registry_open(laghu_operational_registry *registry, const char *cache_path, laghu_operational_surface surface,
                                     laghu_operational_process_kind kind, bool required, uint64_t now) {
  laghu_operational_state *state;
  char path[LAGHU_RUNTIME_PATH_SIZE];
  unsigned int index, selected = LAGHU_WIRE_OPERATIONAL_SLOT_COUNT;
  uint64_t generation;
  const size_t size = (size_t)LAGHU_WIRE_OPERATIONAL_HEADER_SIZE + (size_t)LAGHU_WIRE_OPERATIONAL_SLOT_COUNT * LAGHU_WIRE_OPERATIONAL_SLOT_SIZE;
  if (registry == NULL || surface >= LAGHU_OPERATIONAL_SURFACE_COUNT || kind >= LAGHU_OPERATIONAL_PROCESS_COUNT || now == 0U ||
      !laghu_path(cache_path, path, sizeof(path)))
    return false;
  laghu_operational_registry_init(registry);
  registry->implementation = calloc(1U, sizeof(laghu_operational_state));
  if (registry->implementation == NULL) return false;
  state = LAGHU_OPERATIONAL_STATE(registry);
  laghu_runtime_shared_mapping_init(&state->mapping);
  laghu_local_init(state, now);
  state->slot = LAGHU_WIRE_OPERATIONAL_SLOT_COUNT;
  if (!laghu_runtime_shared_mapping_open(&state->mapping, path, size) || !laghu_runtime_shared_mapping_try_lock(&state->mapping)) {
    laghu_operational_state_release(registry);
    return false;
  }
  state->bytes = state->mapping.mapping;
  if (laghu_bytes_zero(state->bytes, size)) {
    laghu_wire_u64_write(state->bytes + LAGHU_WIRE_OPERATIONAL_HEADER_MAGIC_OFFSET, LAGHU_WIRE_OPERATIONAL_MAGIC);
    laghu_wire_u32_write(state->bytes + LAGHU_WIRE_OPERATIONAL_HEADER_VERSION_OFFSET, LAGHU_WIRE_OPERATIONAL_VERSION);
    laghu_wire_u32_write(state->bytes + LAGHU_WIRE_OPERATIONAL_HEADER_SLOT_COUNT_OFFSET, LAGHU_WIRE_OPERATIONAL_SLOT_COUNT);
    laghu_store(laghu_header_generation(state->bytes), now);
  }
  if (!laghu_header_valid(state)) goto fail;
  for (index = 0U; index < LAGHU_WIRE_OPERATIONAL_SLOT_COUNT; ++index) {
    unsigned char *slot = laghu_slot_at(state->bytes, index);
    uint64_t sequence = laghu_load(slot + LAGHU_WIRE_OPERATIONAL_SLOT_SEQUENCE_OFFSET);
    uint64_t heartbeat;
    uint32_t active;
    if ((sequence & 1U) != 0U) continue;
    if (!laghu_slot_values_valid(slot)) goto fail;
    if (laghu_load(slot + LAGHU_WIRE_OPERATIONAL_SLOT_SEQUENCE_OFFSET) != sequence) continue;
    active = laghu_wire_u32_read(slot + LAGHU_WIRE_OPERATIONAL_SLOT_ACTIVE_OFFSET);
    heartbeat = laghu_load(slot + LAGHU_WIRE_OPERATIONAL_SLOT_HEARTBEAT_OFFSET);
    if (active == 0U || heartbeat == 0U || heartbeat > now || now - heartbeat > LAGHU_OPERATIONAL_STALE_SECONDS) {
      selected = index;
      break;
    }
  }
  if (selected == LAGHU_WIRE_OPERATIONAL_SLOT_COUNT) goto fail;
  generation = laghu_load(laghu_header_generation(state->bytes));
  if (generation == UINT64_MAX) goto fail;
  ++generation;
  {
    unsigned char *slot = laghu_slot_at(state->bytes, selected);
    uint64_t sequence;
    if (!laghu_slot_update_begin(slot, &sequence)) goto fail;
    memset(slot + 8U, 0, LAGHU_WIRE_OPERATIONAL_SLOT_SIZE - 8U);
    laghu_store(laghu_header_generation(state->bytes), generation);
    laghu_store(slot + LAGHU_WIRE_OPERATIONAL_SLOT_GENERATION_OFFSET, generation);
    laghu_store(slot + LAGHU_WIRE_OPERATIONAL_SLOT_HEARTBEAT_OFFSET, now);
    laghu_wire_u32_write(slot + LAGHU_WIRE_OPERATIONAL_SLOT_SURFACE_OFFSET, (uint32_t)surface);
    laghu_wire_u32_write(slot + LAGHU_WIRE_OPERATIONAL_SLOT_PROCESS_KIND_OFFSET, (uint32_t)kind);
    slot[LAGHU_WIRE_OPERATIONAL_SLOT_REQUIRED_OFFSET] = required ? 1U : 0U;
    slot[LAGHU_WIRE_OPERATIONAL_SLOT_HEALTHY_OFFSET] = 1U;
    laghu_wire_u32_write(slot + LAGHU_WIRE_OPERATIONAL_SLOT_ACTIVE_OFFSET, 1U);
    laghu_slot_update_finish(slot, sequence);
  }
  state->slot = selected;
  state->generation = generation;
  (void)laghu_runtime_shared_mapping_sync(&state->mapping);
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  return true;
fail:
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  laghu_operational_state_release(registry);
  return false;
}

void laghu_operational_registry_close(laghu_operational_registry *registry) {
  laghu_operational_state *state = registry != NULL ? LAGHU_OPERATIONAL_STATE(registry) : NULL;
  unsigned char *slot = laghu_slot(registry);
  bool publishing = state != NULL && laghu_local_flag_try_set(&state->publishing);
  if (publishing && slot != NULL && laghu_runtime_shared_mapping_try_lock(&state->mapping) && laghu_header_valid(state)) {
    uint64_t sequence;
    if (laghu_slot_update_begin(slot, &sequence)) {
      if (laghu_load(slot + LAGHU_WIRE_OPERATIONAL_SLOT_GENERATION_OFFSET) == state->generation &&
          laghu_wire_u32_read(slot + LAGHU_WIRE_OPERATIONAL_SLOT_ACTIVE_OFFSET) != 0U) {
        slot[LAGHU_WIRE_OPERATIONAL_SLOT_HEALTHY_OFFSET] = 0U;
        laghu_wire_u32_write(slot + LAGHU_WIRE_OPERATIONAL_SLOT_ACTIVE_OFFSET, 0U);
      }
      laghu_slot_update_finish(slot, sequence);
      (void)laghu_runtime_shared_mapping_sync(&state->mapping);
    }
    laghu_runtime_shared_mapping_unlock(&state->mapping);
  }
  if (publishing) laghu_local_flag_clear(&state->publishing);
  if (registry != NULL) {
    laghu_operational_state_release(registry);
    laghu_operational_registry_init(registry);
  }
}

bool laghu_operational_registry_heartbeat(laghu_operational_registry *registry, uint64_t now, bool healthy, uint64_t queue_capacity,
                                          uint64_t queue_occupied) {
  laghu_operational_state *state = registry != NULL ? LAGHU_OPERATIONAL_STATE(registry) : NULL;
  if (state == NULL || now == 0U) return false;
  laghu_local_flag_store(&state->healthy, healthy);
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_QUEUE_CAPACITY_OFFSET, queue_capacity);
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_QUEUE_OCCUPIED_OFFSET, queue_occupied > queue_capacity ? queue_capacity : queue_occupied);
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_HEARTBEAT_OFFSET, now);
  return laghu_publish(state);
}

void laghu_operational_registry_record(laghu_operational_registry *registry, laghu_operational_decision decision, size_t original_bytes,
                                       size_t selected_bytes, uint64_t elapsed_microseconds) {
  laghu_operational_state *state = registry != NULL ? LAGHU_OPERATIONAL_STATE(registry) : NULL;
  size_t index;
  if (state == NULL || decision >= LAGHU_OPERATIONAL_DECISION_COUNT) return;
  laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_REQUESTS_OFFSET, 1U);
  laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_DECISIONS_OFFSET + decision * 8U, 1U);
  laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_ORIGINAL_BYTES_OFFSET, (uint64_t)original_bytes);
  laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_SELECTED_BYTES_OFFSET, (uint64_t)selected_bytes);
  if (selected_bytes < original_bytes)
    laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_SAVED_BYTES_OFFSET, (uint64_t)(original_bytes - selected_bytes));
  for (index = 0U; index + 1U < LAGHU_OPERATIONAL_LATENCY_BUCKETS; ++index)
    if (elapsed_microseconds <= laghu_latency_limits[index])
      laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_LATENCY_BUCKETS_OFFSET + index * 8U, 1U);
  laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_LATENCY_BUCKETS_OFFSET + (LAGHU_OPERATIONAL_LATENCY_BUCKETS - 1U) * 8U, 1U);
  laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_LATENCY_COUNT_OFFSET, 1U);
  laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_LATENCY_SUM_OFFSET, elapsed_microseconds);
  (void)laghu_publish(state);
}

void laghu_operational_registry_failure(laghu_operational_registry *registry, laghu_operational_failure failure) {
  laghu_operational_state *state = registry != NULL ? LAGHU_OPERATIONAL_STATE(registry) : NULL;
  if (state != NULL && failure < LAGHU_OPERATIONAL_FAILURE_COUNT) {
    laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_FAILURES_OFFSET + failure * 8U, 1U);
    (void)laghu_publish(state);
  }
}

void laghu_operational_registry_worker_job(laghu_operational_registry *registry, bool success, uint64_t elapsed_microseconds,
                                           laghu_operational_failure failure) {
  laghu_operational_state *state = registry != NULL ? LAGHU_OPERATIONAL_STATE(registry) : NULL;
  size_t index;
  if (state == NULL) return;
  for (index = 0U; index + 1U < LAGHU_OPERATIONAL_LATENCY_BUCKETS; ++index)
    if (elapsed_microseconds <= laghu_latency_limits[index])
      laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_LATENCY_BUCKETS_OFFSET + index * 8U, 1U);
  laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_LATENCY_BUCKETS_OFFSET + (LAGHU_OPERATIONAL_LATENCY_BUCKETS - 1U) * 8U, 1U);
  laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_LATENCY_COUNT_OFFSET, 1U);
  laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_LATENCY_SUM_OFFSET, elapsed_microseconds);
  if (!success && failure < LAGHU_OPERATIONAL_FAILURE_COUNT) laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_FAILURES_OFFSET + failure * 8U, 1U);
  (void)laghu_publish(state);
}

void laghu_operational_registry_cache(laghu_operational_registry *registry, const laghu_cache_stats *stats) {
  laghu_operational_state *state = registry != NULL ? LAGHU_OPERATIONAL_STATE(registry) : NULL;
  if (state == NULL || stats == NULL) return;
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_CACHE_HITS_OFFSET, stats->hits);
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_CACHE_MISSES_OFFSET, stats->misses);
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_CACHE_PUBLICATIONS_OFFSET, stats->publications);
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_CACHE_EVICTIONS_OFFSET, stats->evictions);
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_CACHE_BYTES_OFFSET, stats->bytes);
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_CACHE_FILES_OFFSET, stats->files);
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_CACHE_REJECTED_WRITES_OFFSET, stats->rejected_publications);
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_VARIANT_OCCUPANCY_OFFSET, stats->variant_occupancy);
  (void)laghu_publish(state);
}

void laghu_operational_registry_budget(laghu_operational_registry *registry, const laghu_transform_budget *budget, unsigned int deadline_limit_ms) {
  laghu_operational_state *state = registry != NULL ? LAGHU_OPERATIONAL_STATE(registry) : NULL;
  if (state == NULL || budget == NULL) return;
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_MEMORY_CURRENT_OFFSET, budget->current_memory);
  laghu_local_max(state, LAGHU_WIRE_OPERATIONAL_SLOT_MEMORY_PEAK_OFFSET, budget->peak_memory);
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_MEMORY_LIMIT_OFFSET, budget->memory_limit);
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_DEADLINE_LIMIT_OFFSET, deadline_limit_ms);
  laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_VARIANT_LIMIT_OFFSET, budget->variant_limit);
  if (budget->rejection > LAGHU_BUDGET_REJECTION_NONE && budget->rejection < LAGHU_BUDGET_REJECTION_COUNT)
    laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_BUDGET_REJECTIONS_OFFSET + budget->rejection * 8U, 1U);
  (void)laghu_publish(state);
}

void laghu_operational_registry_lcp(laghu_operational_registry *registry, laghu_lcp_decision decision, bool applied,
                                    const unsigned int observations[4], const bool ready[4]) {
  laghu_operational_state *state = registry != NULL ? LAGHU_OPERATIONAL_STATE(registry) : NULL;
  unsigned int bucket;
  if (state == NULL || decision > LAGHU_LCP_DECISION_CONFLICT) return;
  laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_LCP_DECISIONS_OFFSET + (size_t)decision * 8U, 1U);
  if (applied) laghu_local_add(state, LAGHU_WIRE_OPERATIONAL_SLOT_LCP_APPLIED_OFFSET, 1U);
  if (observations != NULL && ready != NULL) {
    for (bucket = 0U; bucket < 4U; ++bucket) {
      laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_LCP_OBSERVATIONS_OFFSET + bucket * 8U, observations[bucket]);
      laghu_local_store(state, LAGHU_WIRE_OPERATIONAL_SLOT_LCP_READY_OFFSET + bucket * 8U, ready[bucket] ? 1U : 0U);
    }
  }
  (void)laghu_publish(state);
}

bool laghu_operational_registry_snapshot(laghu_operational_registry *registry, laghu_operational_snapshot *snapshot) {
  laghu_operational_state *state = registry != NULL ? LAGHU_OPERATIONAL_STATE(registry) : NULL;
  unsigned int slot, attempt;
  if (state == NULL || snapshot == NULL || !laghu_header_valid(state)) return false;
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->version = LAGHU_WIRE_OPERATIONAL_VERSION;
  snapshot->slot_count = LAGHU_WIRE_OPERATIONAL_SLOT_COUNT;
  snapshot->generation = laghu_load(laghu_header_generation(state->bytes));
  for (slot = 0U; slot < LAGHU_WIRE_OPERATIONAL_SLOT_COUNT; ++slot) {
    unsigned char *source = laghu_slot_at(state->bytes, slot);
    bool stable = false;
    for (attempt = 0U; attempt < 3U; ++attempt) {
      uint64_t before = laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_SEQUENCE_OFFSET);
      if ((before & 1U) != 0U) continue;
      laghu_slot_decode(source, &snapshot->slots[slot]);
      if (!laghu_slot_values_valid(source)) continue;
      if (laghu_load(source + LAGHU_WIRE_OPERATIONAL_SLOT_SEQUENCE_OFFSET) == before) {
        stable = true;
        break;
      }
    }
    if (!stable) return false;
  }
  return true;
}
static bool laghu_append(char *output, size_t capacity, size_t *length, const char *format, ...) {
  va_list arguments;
  int written;
  if (*length >= capacity) return false;
  va_start(arguments, format);
  written = vsnprintf(output + *length, capacity - *length, format, arguments);
  va_end(arguments);
  if (written < 0 || (size_t)written >= capacity - *length) return false;
  *length += (size_t)written;
  return true;
}

static const char *laghu_surface_name(uint64_t value) {
  static const char *names[] = {"nginx", "apache", "standalone", "worker"};
  return value < LAGHU_OPERATIONAL_SURFACE_COUNT ? names[value] : "invalid";
}

static const char *laghu_process_name(uint64_t value) {
  static const char *names[] = {"adapter", "libvips", "javascript", "resource_fetch", "asset_upload", "otel_export"};
  return value < LAGHU_OPERATIONAL_PROCESS_COUNT ? names[value] : "invalid";
}

bool laghu_operational_render_prometheus(const laghu_operational_snapshot *snapshot, uint64_t now, char *output, size_t capacity, size_t *length) {
  uint64_t requests = 0U, original = 0U, selected = 0U, saved = 0U;
  uint64_t cache_hits = 0U, cache_misses = 0U, cache_publications = 0U, cache_evictions = 0U, cache_bytes = 0U, cache_files = 0U;
  uint64_t decisions[LAGHU_OPERATIONAL_DECISION_COUNT] = {0};
  uint64_t failures[LAGHU_OPERATIONAL_FAILURE_COUNT] = {0};
  uint64_t budget_rejections[LAGHU_BUDGET_REJECTION_COUNT] = {0};
  uint64_t latency[LAGHU_OPERATIONAL_LATENCY_BUCKETS] = {0};
  uint64_t latency_count = 0U, latency_sum = 0U;
  uint64_t transform_current = 0U, transform_peak = 0U, transform_limit = 0U, deadline_limit = 0U, cache_rejected = 0U, variant_occupancy = 0U,
           variant_limit = 0U;
  uint64_t lcp_decisions[6] = {0}, lcp_applied = 0U, lcp_observations[4] = {0}, lcp_ready[4] = {0};
  static const char *decision_names[] = {"bypass", "original", "optimized", "cached", "queued"};
  static const char *failure_names[] = {"runtime", "cache", "queue", "worker", "transform", "transport"};
  static const char *rejection_names[] = {"none", "content", "memory", "deadline", "cache", "variants"};
  static const char *lcp_names[] = {"none", "learned", "heuristic", "unresolved", "stale", "conflict"};
  static const char *viewport_names[] = {"mobile", "mobile", "desktop", "desktop"};
  static const char *theme_names[] = {"light", "dark", "light", "dark"};
  static const char *bucket_names[] = {"0.001", "0.005", "0.010", "0.025", "0.050", "0.100", "0.250", "0.500", "1.000", "2.500", "5.000", "+Inf"};
  unsigned int index;
  size_t used = 0U;
  if (snapshot == NULL || output == NULL || length == NULL || now == 0U || snapshot->version != LAGHU_OPERATIONAL_VERSION ||
      snapshot->slot_count > LAGHU_OPERATIONAL_MAX_SLOTS)
    return false;
  if (!laghu_append(output, capacity, &used,
                    "# HELP laghu_requests_total Requests handled by Laghu.\n"
                    "# TYPE laghu_requests_total counter\n"))
    return false;
  for (index = 0U; index < snapshot->slot_count; ++index) {
    const laghu_operational_slot_snapshot *slot = &snapshot->slots[index];
    unsigned int metric;
    bool current = slot->active != 0U && slot->heartbeat != 0U && slot->heartbeat <= now && now - slot->heartbeat <= LAGHU_OPERATIONAL_STALE_SECONDS;
    if (!current) continue;
    requests += slot->requests;
    original += slot->original_bytes;
    selected += slot->selected_bytes;
    saved += slot->saved_bytes;
    cache_hits += slot->cache_hits;
    cache_misses += slot->cache_misses;
    cache_publications += slot->cache_publications;
    cache_evictions += slot->cache_evictions;
    if (slot->cache_bytes > cache_bytes) cache_bytes = slot->cache_bytes;
    if (slot->cache_files > cache_files) cache_files = slot->cache_files;
    transform_current += slot->transform_memory_current;
    if (slot->transform_memory_peak > transform_peak) transform_peak = slot->transform_memory_peak;
    transform_limit += slot->transform_memory_limit;
    if (slot->transform_deadline_limit_ms > deadline_limit) deadline_limit = slot->transform_deadline_limit_ms;
    if (slot->cache_rejected_writes > cache_rejected) cache_rejected = slot->cache_rejected_writes;
    variant_occupancy += slot->variant_occupancy;
    variant_limit += slot->variant_limit;
    lcp_applied += slot->lcp_applied;
    for (metric = 0U; metric < 6U; ++metric) lcp_decisions[metric] += slot->lcp_decisions[metric];
    for (metric = 0U; metric < 4U; ++metric) {
      if (slot->lcp_profile_observations[metric] > lcp_observations[metric]) lcp_observations[metric] = slot->lcp_profile_observations[metric];
      if (slot->lcp_profile_ready[metric] > lcp_ready[metric]) lcp_ready[metric] = slot->lcp_profile_ready[metric];
    }
    for (metric = 0U; metric < LAGHU_OPERATIONAL_DECISION_COUNT; ++metric) decisions[metric] += slot->decisions[metric];
    for (metric = 0U; metric < LAGHU_OPERATIONAL_FAILURE_COUNT; ++metric) failures[metric] += slot->failures[metric];
    for (metric = 0U; metric < LAGHU_BUDGET_REJECTION_COUNT; ++metric) budget_rejections[metric] += slot->budget_rejections[metric];
    for (metric = 0U; metric < LAGHU_OPERATIONAL_LATENCY_BUCKETS; ++metric) latency[metric] += slot->latency_buckets[metric];
    latency_count += slot->latency_count;
    latency_sum += slot->latency_sum_microseconds;
    if (!laghu_append(output, capacity, &used,
                      "laghu_requests_total{scope=\"process\",surface=\"%s\","
                      "process_kind=\"%s\",worker_slot=\"%u\"} %" PRIu64 "\n",
                      laghu_surface_name(slot->surface), laghu_process_name(slot->process_kind), index, slot->requests))
      return false;
  }
  if (!laghu_append(output, capacity, &used,
                    "laghu_requests_total{scope=\"instance\"} %" PRIu64 "\n"
                    "# TYPE laghu_cache_requests_total counter\n"
                    "laghu_cache_requests_total{result=\"hit\"} %" PRIu64 "\n"
                    "laghu_cache_requests_total{result=\"miss\"} %" PRIu64 "\n"
                    "# TYPE laghu_cache_publications_total counter\n"
                    "laghu_cache_publications_total %" PRIu64 "\n"
                    "# TYPE laghu_cache_evictions_total counter\n"
                    "laghu_cache_evictions_total %" PRIu64 "\n"
                    "# TYPE laghu_cache_bytes gauge\nlaghu_cache_bytes %" PRIu64 "\n"
                    "# TYPE laghu_cache_files gauge\nlaghu_cache_files %" PRIu64 "\n"
                    "# TYPE laghu_response_bytes_total counter\n"
                    "laghu_response_bytes_total{kind=\"original\"} %" PRIu64 "\n"
                    "laghu_response_bytes_total{kind=\"selected\"} %" PRIu64 "\n"
                    "laghu_response_bytes_total{kind=\"saved\"} %" PRIu64 "\n",
                    requests, cache_hits, cache_misses, cache_publications, cache_evictions, cache_bytes, cache_files, original, selected, saved))
    return false;
  if (!laghu_append(output, capacity, &used, "# TYPE laghu_decisions_total counter\n")) return false;
  for (index = 0U; index < LAGHU_OPERATIONAL_DECISION_COUNT; ++index)
    if (!laghu_append(output, capacity, &used, "laghu_decisions_total{decision=\"%s\"} %" PRIu64 "\n", decision_names[index], decisions[index]))
      return false;
  if (!laghu_append(output, capacity, &used, "# TYPE laghu_failures_total counter\n")) return false;
  for (index = 0U; index < LAGHU_OPERATIONAL_FAILURE_COUNT; ++index)
    if (!laghu_append(output, capacity, &used, "laghu_failures_total{subsystem=\"%s\"} %" PRIu64 "\n", failure_names[index], failures[index]))
      return false;
  if (!laghu_append(output, capacity, &used, "# TYPE laghu_transform_rejections_total counter\n")) return false;
  for (index = 1U; index < LAGHU_BUDGET_REJECTION_COUNT; ++index)
    if (!laghu_append(output, capacity, &used, "laghu_transform_rejections_total{reason=\"%s\"} %" PRIu64 "\n", rejection_names[index],
                      budget_rejections[index]))
      return false;
  if (!laghu_append(output, capacity, &used,
                    "# TYPE laghu_transform_memory_bytes gauge\n"
                    "laghu_transform_memory_bytes{kind=\"current\"} %" PRIu64 "\n"
                    "laghu_transform_memory_bytes{kind=\"peak\"} %" PRIu64 "\n"
                    "laghu_transform_memory_bytes{kind=\"limit\"} %" PRIu64 "\n"
                    "# TYPE laghu_transform_deadline_milliseconds gauge\n"
                    "laghu_transform_deadline_milliseconds %" PRIu64 "\n"
                    "# TYPE laghu_cache_rejected_writes_total counter\n"
                    "laghu_cache_rejected_writes_total %" PRIu64 "\n"
                    "# TYPE laghu_variant_occupancy gauge\n"
                    "laghu_variant_occupancy %" PRIu64 "\n"
                    "# TYPE laghu_variant_limit gauge\nlaghu_variant_limit %" PRIu64 "\n",
                    transform_current, transform_peak, transform_limit, deadline_limit, cache_rejected, variant_occupancy, variant_limit))
    return false;
  if (!laghu_append(output, capacity, &used, "# TYPE laghu_lcp_decisions_total counter\n")) return false;
  for (index = 0U; index < 6U; ++index)
    if (!laghu_append(output, capacity, &used, "laghu_lcp_decisions_total{decision=\"%s\"} %" PRIu64 "\n", lcp_names[index], lcp_decisions[index]))
      return false;
  if (!laghu_append(output, capacity, &used,
                    "# TYPE laghu_lcp_applied_total counter\n"
                    "laghu_lcp_applied_total %" PRIu64 "\n"
                    "# TYPE laghu_lcp_profile_observations gauge\n"
                    "# TYPE laghu_lcp_profile_ready gauge\n",
                    lcp_applied))
    return false;
  for (index = 0U; index < 4U; ++index)
    if (!laghu_append(output, capacity, &used,
                      "laghu_lcp_profile_observations{viewport=\"%s\",theme=\"%s\"} "
                      "%" PRIu64 "\n"
                      "laghu_lcp_profile_ready{viewport=\"%s\",theme=\"%s\"} %" PRIu64 "\n",
                      viewport_names[index], theme_names[index], lcp_observations[index], viewport_names[index], theme_names[index],
                      lcp_ready[index]))
      return false;
  if (!laghu_append(output, capacity, &used, "# TYPE laghu_request_duration_seconds histogram\n")) return false;
  for (index = 0U; index < LAGHU_OPERATIONAL_LATENCY_BUCKETS; ++index)
    if (!laghu_append(output, capacity, &used, "laghu_request_duration_seconds_bucket{le=\"%s\"} %" PRIu64 "\n", bucket_names[index], latency[index]))
      return false;
  if (!laghu_append(output, capacity, &used,
                    "laghu_request_duration_seconds_sum %.6f\n"
                    "laghu_request_duration_seconds_count %" PRIu64 "\n",
                    (double)latency_sum / 1000000.0, latency_count))
    return false;
  for (index = 0U; index < snapshot->slot_count; ++index) {
    const laghu_operational_slot_snapshot *slot = &snapshot->slots[index];
    if (slot->active == 0U || slot->heartbeat == 0U || slot->process_kind == LAGHU_OPERATIONAL_PROCESS_ADAPTER) continue;
    if (!laghu_append(output, capacity, &used,
                      "laghu_worker_up{process_kind=\"%s\",worker_slot=\"%u\"} %u\n"
                      "laghu_queue_capacity{process_kind=\"%s\",worker_slot=\"%u\"} "
                      "%" PRIu64 "\n"
                      "laghu_queue_occupied{process_kind=\"%s\",worker_slot=\"%u\"} "
                      "%" PRIu64 "\n",
                      laghu_process_name(slot->process_kind), index,
                      slot->heartbeat <= now && now - slot->heartbeat <= LAGHU_OPERATIONAL_STALE_SECONDS && slot->healthy != 0U ? 1U : 0U,
                      laghu_process_name(slot->process_kind), index, slot->queue_capacity, laghu_process_name(slot->process_kind), index,
                      slot->queue_occupied))
      return false;
  }
  *length = used;
  return true;
}

bool laghu_operational_readiness_evaluate(const laghu_operational_snapshot *snapshot, uint64_t now, bool runtime_ready, bool cache_ready,
                                          bool strict_workers, laghu_operational_readiness *readiness) {
  unsigned int index;
  if (snapshot == NULL || readiness == NULL || now == 0U || snapshot->version != LAGHU_OPERATIONAL_VERSION) return false;
  memset(readiness, 0, sizeof(*readiness));
  readiness->runtime_ready = runtime_ready;
  readiness->cache_ready = cache_ready;
  readiness->workers_ready = true;
  readiness->budgets_ready = true;
  for (index = 0U; index < snapshot->slot_count; ++index) {
    const laghu_operational_slot_snapshot *slot = &snapshot->slots[index];
    bool healthy;
    if (slot->active == 0U || slot->process_kind == LAGHU_OPERATIONAL_PROCESS_ADAPTER) continue;
    ++readiness->configured_workers;
    healthy = slot->healthy != 0U && slot->heartbeat != 0U && slot->heartbeat <= now && now - slot->heartbeat <= LAGHU_OPERATIONAL_STALE_SECONDS;
    if (healthy) ++readiness->healthy_workers;
    if (slot->required != 0U && !healthy) readiness->workers_ready = false;
  }
  readiness->degraded = !readiness->workers_ready;
  if (!strict_workers) readiness->workers_ready = true;
  return true;
}

bool laghu_operational_render_readiness(const laghu_operational_readiness *readiness, bool strict_workers, char *output, size_t capacity,
                                        size_t *length) {
  int written;
  if (readiness == NULL || output == NULL || length == NULL) return false;
  written = snprintf(
      output, capacity,
      "{\"status\":\"%s\",\"runtime\":\"%s\",\"cache\":\"%s\","
      "\"workers\":\"%s\",\"budgets\":\"%s\",\"policy\":\"%s\","
      "\"configured_workers\":%u,"
      "\"healthy_workers\":%u}",
      readiness->runtime_ready && readiness->cache_ready && readiness->workers_ready ? (readiness->degraded ? "degraded" : "ready") : "not_ready",
      readiness->runtime_ready ? "ready" : "not_ready", readiness->cache_ready ? "ready" : "not_ready", readiness->degraded ? "degraded" : "ready",
      readiness->budgets_ready ? "ready" : "unavailable", strict_workers ? "strict" : "degraded", readiness->configured_workers,
      readiness->healthy_workers);
  if (written < 0 || (size_t)written >= capacity) return false;
  *length = (size_t)written;
  return true;
}
