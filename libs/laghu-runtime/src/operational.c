// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "laghu/runtime.h"

#ifdef _WIN32
#include <windows.h>
#endif

#define LAGHU_OPERATIONAL_MAGIC UINT64_C(0x4c414748554f5053)

typedef struct {
  uint64_t magic;
  uint32_t version;
  uint32_t slot_count;
  uint64_t generation;
  laghu_operational_slot_snapshot slots[LAGHU_OPERATIONAL_MAX_SLOTS];
} laghu_operational_header;

static const uint64_t laghu_latency_limits[] = {
    1000U,   5000U,   10000U,   25000U,   50000U,  100000U,
    250000U, 500000U, 1000000U, 2500000U, 5000000U};

static uint64_t laghu_load(const uint64_t *value) {
#ifdef _WIN32
  return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)value, 0, 0);
#else
  return __atomic_load_n(value, __ATOMIC_RELAXED);
#endif
}

static void laghu_store(uint64_t *value, uint64_t replacement) {
#ifdef _WIN32
  (void)InterlockedExchange64((volatile LONG64 *)value, (LONG64)replacement);
#else
  __atomic_store_n(value, replacement, __ATOMIC_RELAXED);
#endif
}

static void laghu_add(uint64_t *value, uint64_t increment) {
#ifdef _WIN32
  uint64_t current, replacement;
  do {
    current = laghu_load(value);
    replacement =
        UINT64_MAX - current < increment ? UINT64_MAX : current + increment;
  } while ((uint64_t)InterlockedCompareExchange64((volatile LONG64 *)value,
                                                  (LONG64)replacement,
                                                  (LONG64)current) != current);
#else
  uint64_t current = laghu_load(value);
  while (!__atomic_compare_exchange_n(
      value, &current,
      UINT64_MAX - current < increment ? UINT64_MAX : current + increment,
      false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
#endif
}

static laghu_operational_slot_snapshot *laghu_slot(
    laghu_operational_registry *registry) {
  laghu_operational_header *header = registry != NULL ? registry->header : NULL;
  return header != NULL && registry->slot < LAGHU_OPERATIONAL_MAX_SLOTS
             ? &header->slots[registry->slot]
             : NULL;
}

static bool laghu_path(const char *cache_path, char *output, size_t capacity) {
  size_t length;
  if (cache_path == NULL || output == NULL || capacity == 0U) return false;
  length = strlen(cache_path);
  if (length == 0U || length + sizeof(".laghu-operations-v2") > capacity)
    return false;
  return snprintf(output, capacity, "%s.laghu-operations-v2", cache_path) > 0;
}

void laghu_operational_registry_init(laghu_operational_registry *registry) {
  if (registry == NULL) return;
  memset(registry, 0, sizeof(*registry));
  laghu_runtime_shared_mapping_init(&registry->mapping);
  registry->slot = LAGHU_OPERATIONAL_MAX_SLOTS;
}

bool laghu_operational_registry_open(laghu_operational_registry *registry,
                                     const char *cache_path,
                                     laghu_operational_surface surface,
                                     laghu_operational_process_kind kind,
                                     bool required, uint64_t now) {
  laghu_operational_header *header;
  char path[LAGHU_RUNTIME_PATH_SIZE];
  unsigned int index, selected = LAGHU_OPERATIONAL_MAX_SLOTS;
  if (registry == NULL || surface >= LAGHU_OPERATIONAL_SURFACE_COUNT ||
      kind >= LAGHU_OPERATIONAL_PROCESS_COUNT || now == 0U ||
      !laghu_path(cache_path, path, sizeof(path)))
    return false;
  laghu_operational_registry_init(registry);
  if (!laghu_runtime_shared_mapping_open(&registry->mapping, path,
                                         sizeof(laghu_operational_header)) ||
      !laghu_runtime_shared_mapping_try_lock(&registry->mapping)) {
    laghu_runtime_shared_mapping_close(&registry->mapping);
    return false;
  }
  header = registry->mapping.mapping;
  if (header->magic == 0U) {
    memset(header, 0, sizeof(*header));
    header->magic = LAGHU_OPERATIONAL_MAGIC;
    header->version = LAGHU_OPERATIONAL_VERSION;
    header->slot_count = LAGHU_OPERATIONAL_MAX_SLOTS;
    header->generation = now;
  }
  if (header->magic != LAGHU_OPERATIONAL_MAGIC ||
      header->version != LAGHU_OPERATIONAL_VERSION ||
      header->slot_count != LAGHU_OPERATIONAL_MAX_SLOTS) {
    laghu_runtime_shared_mapping_unlock(&registry->mapping);
    laghu_runtime_shared_mapping_close(&registry->mapping);
    return false;
  }
  for (index = 0U; index < LAGHU_OPERATIONAL_MAX_SLOTS; ++index) {
    uint64_t heartbeat = laghu_load(&header->slots[index].heartbeat);
    if (laghu_load(&header->slots[index].active) == 0U || heartbeat == 0U ||
        heartbeat > now || now - heartbeat > LAGHU_OPERATIONAL_STALE_SECONDS) {
      selected = index;
      break;
    }
  }
  if (selected == LAGHU_OPERATIONAL_MAX_SLOTS) {
    laghu_runtime_shared_mapping_unlock(&registry->mapping);
    laghu_runtime_shared_mapping_close(&registry->mapping);
    return false;
  }
  memset(&header->slots[selected], 0, sizeof(header->slots[selected]));
  header->slots[selected].generation = ++header->generation;
  header->slots[selected].surface = (uint64_t)surface;
  header->slots[selected].process_kind = (uint64_t)kind;
  header->slots[selected].required = required ? 1U : 0U;
  header->slots[selected].healthy = 1U;
  header->slots[selected].heartbeat = now;
  header->slots[selected].active = 1U;
  registry->header = header;
  registry->slot = selected;
  registry->generation = header->slots[selected].generation;
  (void)laghu_runtime_shared_mapping_sync(&registry->mapping);
  laghu_runtime_shared_mapping_unlock(&registry->mapping);
  return true;
}

void laghu_operational_registry_close(laghu_operational_registry *registry) {
  laghu_operational_slot_snapshot *slot = laghu_slot(registry);
  if (slot != NULL && laghu_load(&slot->generation) == registry->generation) {
    laghu_store(&slot->healthy, 0U);
    if (laghu_load(&slot->process_kind) == LAGHU_OPERATIONAL_PROCESS_ADAPTER)
      laghu_store(&slot->active, 0U);
  }
  if (registry != NULL) {
    laghu_runtime_shared_mapping_close(&registry->mapping);
    laghu_operational_registry_init(registry);
  }
}

bool laghu_operational_registry_heartbeat(laghu_operational_registry *registry,
                                          uint64_t now, bool healthy,
                                          uint64_t queue_capacity,
                                          uint64_t queue_occupied) {
  laghu_operational_slot_snapshot *slot = laghu_slot(registry);
  if (slot == NULL || now == 0U ||
      laghu_load(&slot->generation) != registry->generation ||
      laghu_load(&slot->active) == 0U)
    return false;
  laghu_store(&slot->healthy, healthy ? 1U : 0U);
  laghu_store(&slot->queue_capacity, queue_capacity);
  laghu_store(&slot->queue_occupied, queue_occupied > queue_capacity
                                         ? queue_capacity
                                         : queue_occupied);
  laghu_store(&slot->heartbeat, now);
  return true;
}

void laghu_operational_registry_record(laghu_operational_registry *registry,
                                       laghu_operational_decision decision,
                                       size_t original_bytes,
                                       size_t selected_bytes,
                                       uint64_t elapsed_microseconds) {
  laghu_operational_slot_snapshot *slot = laghu_slot(registry);
  size_t index;
  if (slot == NULL || decision >= LAGHU_OPERATIONAL_DECISION_COUNT) return;
  laghu_add(&slot->requests, 1U);
  laghu_add(&slot->decisions[decision], 1U);
  laghu_add(&slot->original_bytes, (uint64_t)original_bytes);
  laghu_add(&slot->selected_bytes, (uint64_t)selected_bytes);
  if (selected_bytes < original_bytes)
    laghu_add(&slot->saved_bytes, (uint64_t)(original_bytes - selected_bytes));
  for (index = 0U; index + 1U < LAGHU_OPERATIONAL_LATENCY_BUCKETS; ++index)
    if (elapsed_microseconds <= laghu_latency_limits[index])
      laghu_add(&slot->latency_buckets[index], 1U);
  laghu_add(&slot->latency_buckets[LAGHU_OPERATIONAL_LATENCY_BUCKETS - 1U], 1U);
  laghu_add(&slot->latency_count, 1U);
  laghu_add(&slot->latency_sum_microseconds, elapsed_microseconds);
}

void laghu_operational_registry_failure(laghu_operational_registry *registry,
                                        laghu_operational_failure failure) {
  laghu_operational_slot_snapshot *slot = laghu_slot(registry);
  if (slot != NULL && failure < LAGHU_OPERATIONAL_FAILURE_COUNT)
    laghu_add(&slot->failures[failure], 1U);
}

void laghu_operational_registry_cache(laghu_operational_registry *registry,
                                      const laghu_cache_stats *stats) {
  laghu_operational_slot_snapshot *slot = laghu_slot(registry);
  if (slot == NULL || stats == NULL) return;
  laghu_store(&slot->cache_hits, stats->hits);
  laghu_store(&slot->cache_misses, stats->misses);
  laghu_store(&slot->cache_publications, stats->publications);
  laghu_store(&slot->cache_evictions, stats->evictions);
  laghu_store(&slot->cache_bytes, stats->bytes);
  laghu_store(&slot->cache_files, stats->files);
  laghu_store(&slot->cache_rejected_writes, stats->rejected_publications);
  laghu_store(&slot->variant_occupancy, stats->variant_occupancy);
}

void laghu_operational_registry_budget(laghu_operational_registry *registry,
                                       const laghu_transform_budget *budget,
                                       unsigned int deadline_limit_ms) {
  laghu_operational_slot_snapshot *slot = laghu_slot(registry);
  if (slot == NULL || budget == NULL) return;
  laghu_store(&slot->transform_memory_current, budget->current_memory);
  if (budget->peak_memory > laghu_load(&slot->transform_memory_peak))
    laghu_store(&slot->transform_memory_peak, budget->peak_memory);
  laghu_store(&slot->transform_memory_limit, budget->memory_limit);
  laghu_store(&slot->transform_deadline_limit_ms, deadline_limit_ms);
  laghu_store(&slot->variant_limit, budget->variant_limit);
  if (budget->rejection > LAGHU_BUDGET_REJECTION_NONE &&
      budget->rejection < LAGHU_BUDGET_REJECTION_COUNT)
    laghu_add(&slot->budget_rejections[budget->rejection], 1U);
}

bool laghu_operational_registry_snapshot(laghu_operational_registry *registry,
                                         laghu_operational_snapshot *snapshot) {
  laghu_operational_header *header = registry != NULL ? registry->header : NULL;
  unsigned int slot, field;
  if (header == NULL || snapshot == NULL ||
      header->magic != LAGHU_OPERATIONAL_MAGIC ||
      header->version != LAGHU_OPERATIONAL_VERSION)
    return false;
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->version = header->version;
  snapshot->slot_count = header->slot_count;
  snapshot->generation = laghu_load(&header->generation);
  for (slot = 0U; slot < snapshot->slot_count; ++slot) {
    const laghu_operational_slot_snapshot *source = &header->slots[slot];
    laghu_operational_slot_snapshot *target = &snapshot->slots[slot];
    const uint64_t *source_fields = (const uint64_t *)source;
    uint64_t *target_fields = (uint64_t *)target;
    for (field = 0U; field < sizeof(*target) / sizeof(uint64_t); ++field)
      target_fields[field] = laghu_load(&source_fields[field]);
  }
  return true;
}

static bool laghu_append(char *output, size_t capacity, size_t *length,
                         const char *format, ...) {
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
  static const char *names[] = {"adapter", "libvips", "javascript",
                                "resource_fetch", "asset_upload"};
  return value < LAGHU_OPERATIONAL_PROCESS_COUNT ? names[value] : "invalid";
}

bool laghu_operational_render_prometheus(
    const laghu_operational_snapshot *snapshot, uint64_t now, char *output,
    size_t capacity, size_t *length) {
  uint64_t requests = 0U, original = 0U, selected = 0U, saved = 0U;
  uint64_t cache_hits = 0U, cache_misses = 0U, cache_publications = 0U,
           cache_evictions = 0U, cache_bytes = 0U, cache_files = 0U;
  uint64_t decisions[LAGHU_OPERATIONAL_DECISION_COUNT] = {0};
  uint64_t failures[LAGHU_OPERATIONAL_FAILURE_COUNT] = {0};
  uint64_t budget_rejections[LAGHU_BUDGET_REJECTION_COUNT] = {0};
  uint64_t latency[LAGHU_OPERATIONAL_LATENCY_BUCKETS] = {0};
  uint64_t latency_count = 0U, latency_sum = 0U;
  uint64_t transform_current = 0U, transform_peak = 0U, transform_limit = 0U,
           deadline_limit = 0U, cache_rejected = 0U, variant_occupancy = 0U,
           variant_limit = 0U;
  static const char *decision_names[] = {"bypass", "original", "optimized",
                                         "cached", "queued"};
  static const char *failure_names[] = {"runtime", "cache",     "queue",
                                        "worker",  "transform", "transport"};
  static const char *rejection_names[] = {"none",     "content", "memory",
                                          "deadline", "cache",   "variants"};
  static const char *bucket_names[] = {"0.001", "0.005", "0.010", "0.025",
                                       "0.050", "0.100", "0.250", "0.500",
                                       "1.000", "2.500", "5.000", "+Inf"};
  unsigned int index;
  size_t used = 0U;
  if (snapshot == NULL || output == NULL || length == NULL || now == 0U ||
      snapshot->version != LAGHU_OPERATIONAL_VERSION ||
      snapshot->slot_count > LAGHU_OPERATIONAL_MAX_SLOTS)
    return false;
  if (!laghu_append(output, capacity, &used,
                    "# HELP laghu_requests_total Requests handled by Laghu.\n"
                    "# TYPE laghu_requests_total counter\n"))
    return false;
  for (index = 0U; index < snapshot->slot_count; ++index) {
    const laghu_operational_slot_snapshot *slot = &snapshot->slots[index];
    bool current = slot->active != 0U && slot->heartbeat != 0U &&
                   slot->heartbeat <= now &&
                   now - slot->heartbeat <= LAGHU_OPERATIONAL_STALE_SECONDS;
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
    if (slot->transform_memory_peak > transform_peak)
      transform_peak = slot->transform_memory_peak;
    transform_limit += slot->transform_memory_limit;
    if (slot->transform_deadline_limit_ms > deadline_limit)
      deadline_limit = slot->transform_deadline_limit_ms;
    if (slot->cache_rejected_writes > cache_rejected)
      cache_rejected = slot->cache_rejected_writes;
    variant_occupancy += slot->variant_occupancy;
    variant_limit += slot->variant_limit;
    {
      unsigned int metric;
      for (metric = 0U; metric < LAGHU_OPERATIONAL_DECISION_COUNT; ++metric)
        decisions[metric] += slot->decisions[metric];
      for (metric = 0U; metric < LAGHU_OPERATIONAL_FAILURE_COUNT; ++metric)
        failures[metric] += slot->failures[metric];
      for (metric = 0U; metric < LAGHU_BUDGET_REJECTION_COUNT; ++metric)
        budget_rejections[metric] += slot->budget_rejections[metric];
      for (metric = 0U; metric < LAGHU_OPERATIONAL_LATENCY_BUCKETS; ++metric)
        latency[metric] += slot->latency_buckets[metric];
      latency_count += slot->latency_count;
      latency_sum += slot->latency_sum_microseconds;
    }
    if (!laghu_append(output, capacity, &used,
                      "laghu_requests_total{scope=\"process\",surface=\"%s\","
                      "process_kind=\"%s\",worker_slot=\"%u\"} %" PRIu64 "\n",
                      laghu_surface_name(slot->surface),
                      laghu_process_name(slot->process_kind), index,
                      slot->requests))
      return false;
  }
  if (!laghu_append(
          output, capacity, &used,
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
          requests, cache_hits, cache_misses, cache_publications,
          cache_evictions, cache_bytes, cache_files, original, selected, saved))
    return false;
  if (!laghu_append(output, capacity, &used,
                    "# TYPE laghu_decisions_total counter\n"))
    return false;
  for (index = 0U; index < LAGHU_OPERATIONAL_DECISION_COUNT; ++index)
    if (!laghu_append(output, capacity, &used,
                      "laghu_decisions_total{decision=\"%s\"} %" PRIu64 "\n",
                      decision_names[index], decisions[index]))
      return false;
  if (!laghu_append(output, capacity, &used,
                    "# TYPE laghu_failures_total counter\n"))
    return false;
  for (index = 0U; index < LAGHU_OPERATIONAL_FAILURE_COUNT; ++index)
    if (!laghu_append(output, capacity, &used,
                      "laghu_failures_total{subsystem=\"%s\"} %" PRIu64 "\n",
                      failure_names[index], failures[index]))
      return false;
  if (!laghu_append(output, capacity, &used,
                    "# TYPE laghu_transform_rejections_total counter\n"))
    return false;
  for (index = 1U; index < LAGHU_BUDGET_REJECTION_COUNT; ++index)
    if (!laghu_append(output, capacity, &used,
                      "laghu_transform_rejections_total{reason=\"%s\"} %" PRIu64
                      "\n",
                      rejection_names[index], budget_rejections[index]))
      return false;
  if (!laghu_append(
          output, capacity, &used,
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
          transform_current, transform_peak, transform_limit, deadline_limit,
          cache_rejected, variant_occupancy, variant_limit))
    return false;
  if (!laghu_append(output, capacity, &used,
                    "# TYPE laghu_request_duration_seconds histogram\n"))
    return false;
  for (index = 0U; index < LAGHU_OPERATIONAL_LATENCY_BUCKETS; ++index)
    if (!laghu_append(
            output, capacity, &used,
            "laghu_request_duration_seconds_bucket{le=\"%s\"} %" PRIu64 "\n",
            bucket_names[index], latency[index]))
      return false;
  if (!laghu_append(output, capacity, &used,
                    "laghu_request_duration_seconds_sum %.6f\n"
                    "laghu_request_duration_seconds_count %" PRIu64 "\n",
                    (double)latency_sum / 1000000.0, latency_count))
    return false;
  for (index = 0U; index < snapshot->slot_count; ++index) {
    const laghu_operational_slot_snapshot *slot = &snapshot->slots[index];
    if (slot->active == 0U || slot->heartbeat == 0U ||
        slot->process_kind == LAGHU_OPERATIONAL_PROCESS_ADAPTER)
      continue;
    if (!laghu_append(
            output, capacity, &used,
            "laghu_worker_up{process_kind=\"%s\",worker_slot=\"%u\"} %u\n"
            "laghu_queue_capacity{process_kind=\"%s\",worker_slot=\"%u\"} "
            "%" PRIu64 "\n"
            "laghu_queue_occupied{process_kind=\"%s\",worker_slot=\"%u\"} "
            "%" PRIu64 "\n",
            laghu_process_name(slot->process_kind), index,
            slot->heartbeat <= now &&
                    now - slot->heartbeat <= LAGHU_OPERATIONAL_STALE_SECONDS &&
                    slot->healthy != 0U
                ? 1U
                : 0U,
            laghu_process_name(slot->process_kind), index, slot->queue_capacity,
            laghu_process_name(slot->process_kind), index,
            slot->queue_occupied))
      return false;
  }
  *length = used;
  return true;
}

bool laghu_operational_readiness_evaluate(
    const laghu_operational_snapshot *snapshot, uint64_t now,
    bool runtime_ready, bool cache_ready, bool strict_workers,
    laghu_operational_readiness *readiness) {
  unsigned int index;
  if (snapshot == NULL || readiness == NULL || now == 0U ||
      snapshot->version != LAGHU_OPERATIONAL_VERSION)
    return false;
  memset(readiness, 0, sizeof(*readiness));
  readiness->runtime_ready = runtime_ready;
  readiness->cache_ready = cache_ready;
  readiness->workers_ready = true;
  readiness->budgets_ready = true;
  for (index = 0U; index < snapshot->slot_count; ++index) {
    const laghu_operational_slot_snapshot *slot = &snapshot->slots[index];
    bool healthy;
    if (slot->active == 0U ||
        slot->process_kind == LAGHU_OPERATIONAL_PROCESS_ADAPTER)
      continue;
    ++readiness->configured_workers;
    healthy = slot->healthy != 0U && slot->heartbeat != 0U &&
              slot->heartbeat <= now &&
              now - slot->heartbeat <= LAGHU_OPERATIONAL_STALE_SECONDS;
    if (healthy) ++readiness->healthy_workers;
    if (slot->required != 0U && !healthy) readiness->workers_ready = false;
  }
  readiness->degraded = !readiness->workers_ready;
  if (!strict_workers) readiness->workers_ready = true;
  return true;
}

bool laghu_operational_render_readiness(
    const laghu_operational_readiness *readiness, bool strict_workers,
    char *output, size_t capacity, size_t *length) {
  int written;
  if (readiness == NULL || output == NULL || length == NULL) return false;
  written = snprintf(output, capacity,
                     "{\"status\":\"%s\",\"runtime\":\"%s\",\"cache\":\"%s\","
                     "\"workers\":\"%s\",\"budgets\":\"%s\",\"policy\":\"%s\","
                     "\"configured_workers\":%u,"
                     "\"healthy_workers\":%u}",
                     readiness->runtime_ready && readiness->cache_ready &&
                             readiness->workers_ready
                         ? (readiness->degraded ? "degraded" : "ready")
                         : "not_ready",
                     readiness->runtime_ready ? "ready" : "not_ready",
                     readiness->cache_ready ? "ready" : "not_ready",
                     readiness->degraded ? "degraded" : "ready",
                     readiness->budgets_ready ? "ready" : "unavailable",
                     strict_workers ? "strict" : "degraded",
                     readiness->configured_workers, readiness->healthy_workers);
  if (written < 0 || (size_t)written >= capacity) return false;
  *length = (size_t)written;
  return true;
}
