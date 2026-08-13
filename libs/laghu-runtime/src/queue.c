// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/queue.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/fonts.h"
#include "laghu/javascript.h"
#include "laghu/types.h"
#include "persisted_state_wire.h"
#include "runtime_platform.h"

typedef struct {
  laghu_runtime_shared_mapping mapping;
  unsigned int slot_count;
  size_t slot_payload_size;
} laghu_runtime_queue_state;

#define LAGHU_QUEUE_STATE(queue) ((laghu_runtime_queue_state *)(queue)->implementation)

static bool laghu_queue_hash_valid(const char *value) {
  size_t index;
  if (value == NULL || value[LAGHU_RUNTIME_KEY_SIZE - 1U] != '\0') return false;
  for (index = 0U; index + 1U < LAGHU_RUNTIME_KEY_SIZE; ++index)
    if (!((value[index] >= '0' && value[index] <= '9') || (value[index] >= 'a' && value[index] <= 'f'))) return false;
  return true;
}

static bool laghu_queue_copy(unsigned char *target, size_t capacity, const char *source) {
  size_t length;
  if (target == NULL || source == NULL || capacity == 0U) return false;
  length = strlen(source);
  if (length >= capacity) return false;
  memset(target, 0, capacity);
  memcpy(target, source, length);
  return true;
}

static bool laghu_queue_sprite_valid(const laghu_runtime_job *job) {
  unsigned int index;
  if (job->kind != LAGHU_RUNTIME_JOB_SPRITE) return job->sprite_count == 0U;
  if (job->sprite_count < 2U || job->sprite_count > LAGHU_RUNTIME_MAX_SPRITE_INPUTS || job->payload.length != 0U) return false;
  for (index = 0U; index < job->sprite_count; ++index)
    if (!laghu_queue_hash_valid(job->sprite_variant_keys[index])) return false;
  return true;
}

static bool laghu_queue_font_valid(const laghu_runtime_job *job) {
  if (job->kind != LAGHU_RUNTIME_JOB_FONT_CSS) return job->provider_id[0] == '\0' && job->provider_digest[0] == '\0';
  return job->payload.length == 0U && job->request_path[0] != '\0' && job->provider_id[0] != '\0' &&
         memchr(job->provider_id, '\0', sizeof(job->provider_id)) != NULL && laghu_queue_hash_valid(job->provider_digest);
}

static bool laghu_queue_javascript_valid(const laghu_runtime_job *job) {
  if (job->kind != LAGHU_RUNTIME_JOB_JAVASCRIPT) return job->javascript_target[0] == '\0';
  return job->payload.length > 0U && job->payload.length <= LAGHU_JAVASCRIPT_MAX_BYTES && job->javascript_target[0] != '\0' &&
         memchr(job->javascript_target, '\0', sizeof(job->javascript_target)) != NULL;
}

static bool laghu_queue_browser_analysis_valid(const laghu_runtime_job *job) {
  if (job->kind != LAGHU_RUNTIME_JOB_BROWSER_ANALYSIS) return job->analysis_timeout_ms == 0U;
  return job->payload.length > 0U && job->payload.length <= 1024U * 1024U && job->analysis_timeout_ms >= 100U && job->analysis_timeout_ms <= 10000U &&
         strcmp(job->content_type, "text/html") == 0;
}

static bool laghu_queue_job_valid(const laghu_runtime_job *job, size_t payload_limit) {
  return job != NULL && job->kind <= LAGHU_RUNTIME_JOB_BROWSER_ANALYSIS && job->target_count <= LAGHU_RUNTIME_MAX_TARGETS &&
         job->sprite_count <= LAGHU_RUNTIME_MAX_SPRITE_INPUTS && laghu_queue_hash_valid(job->index_key) && laghu_queue_hash_valid(job->policy_key) &&
         laghu_wire_string_valid((const unsigned char *)job->request_path, sizeof(job->request_path)) &&
         laghu_wire_string_valid((const unsigned char *)job->validator, sizeof(job->validator)) &&
         laghu_wire_string_valid((const unsigned char *)job->content_type, sizeof(job->content_type)) && laghu_queue_sprite_valid(job) &&
         laghu_queue_font_valid(job) && laghu_queue_javascript_valid(job) && laghu_queue_browser_analysis_valid(job) &&
         job->payload.length <= payload_limit && (job->payload.data != NULL || job->payload.length == 0U);
}

static bool laghu_queue_size(unsigned int slots, size_t payload, size_t *result) {
  size_t slot_size;
  if (result == NULL || slots == 0U || payload == 0U || payload > SIZE_MAX - LAGHU_WIRE_QUEUE_SLOT_HEADER_SIZE) return false;
  slot_size = LAGHU_WIRE_QUEUE_SLOT_HEADER_SIZE + payload;
  if (slots > (SIZE_MAX - LAGHU_WIRE_QUEUE_HEADER_SIZE) / slot_size) return false;
  *result = LAGHU_WIRE_QUEUE_HEADER_SIZE + (size_t)slots * slot_size;
  return true;
}

static unsigned char *laghu_queue_header(laghu_runtime_queue_state *state) { return state != NULL ? state->mapping.mapping : NULL; }

static unsigned char *laghu_queue_slot(laghu_runtime_queue_state *state, unsigned int index) {
  return laghu_queue_header(state) + LAGHU_WIRE_QUEUE_HEADER_SIZE + (size_t)index * (LAGHU_WIRE_QUEUE_SLOT_HEADER_SIZE + state->slot_payload_size);
}

static unsigned char *laghu_queue_payload(unsigned char *slot) { return slot + LAGHU_WIRE_QUEUE_SLOT_HEADER_SIZE; }

static bool laghu_queue_header_valid(const unsigned char *header, const laghu_runtime_queue_state *state) {
  return header != NULL && state != NULL && laghu_wire_u64_read(header + LAGHU_WIRE_QUEUE_HEADER_MAGIC_OFFSET) == LAGHU_WIRE_QUEUE_MAGIC &&
         laghu_wire_u32_read(header + LAGHU_WIRE_QUEUE_HEADER_VERSION_OFFSET) == LAGHU_WIRE_QUEUE_VERSION &&
         laghu_wire_u32_read(header + LAGHU_WIRE_QUEUE_HEADER_SLOT_COUNT_OFFSET) == state->slot_count &&
         laghu_wire_u64_read(header + LAGHU_WIRE_QUEUE_HEADER_SLOT_PAYLOAD_SIZE_OFFSET) == state->slot_payload_size &&
         laghu_wire_zeroes(header + 36U, 4U) &&
         laghu_wire_string_valid(header + LAGHU_WIRE_QUEUE_HEADER_BACKEND_ID_OFFSET, LAGHU_RUNTIME_BACKEND_SIZE);
}

static bool laghu_queue_slot_strings_valid(const unsigned char *slot) {
  return laghu_wire_string_valid(slot + LAGHU_WIRE_QUEUE_SLOT_INDEX_KEY_OFFSET, LAGHU_RUNTIME_KEY_SIZE) &&
         laghu_wire_string_valid(slot + LAGHU_WIRE_QUEUE_SLOT_REQUEST_PATH_OFFSET, LAGHU_RUNTIME_PATH_SIZE) &&
         laghu_wire_string_valid(slot + LAGHU_WIRE_QUEUE_SLOT_VALIDATOR_OFFSET, LAGHU_RUNTIME_VALIDATOR_SIZE) &&
         laghu_wire_string_valid(slot + LAGHU_WIRE_QUEUE_SLOT_CONTENT_TYPE_OFFSET, LAGHU_RUNTIME_TYPE_SIZE) &&
         laghu_wire_string_valid(slot + LAGHU_WIRE_QUEUE_SLOT_POLICY_KEY_OFFSET, LAGHU_RUNTIME_KEY_SIZE) &&
         laghu_wire_string_valid(slot + LAGHU_WIRE_QUEUE_SLOT_PROVIDER_ID_OFFSET, LAGHU_FONT_PROVIDER_ID_SIZE) &&
         laghu_wire_string_valid(slot + LAGHU_WIRE_QUEUE_SLOT_PROVIDER_DIGEST_OFFSET, LAGHU_RUNTIME_KEY_SIZE) &&
         laghu_wire_string_valid(slot + LAGHU_WIRE_QUEUE_SLOT_JAVASCRIPT_TARGET_OFFSET, LAGHU_JAVASCRIPT_TARGET_SIZE);
}

static void laghu_queue_state_release(laghu_runtime_queue *queue) {
  laghu_runtime_queue_state *state;
  if (queue == NULL || queue->implementation == NULL) return;
  state = LAGHU_QUEUE_STATE(queue);
  laghu_runtime_shared_mapping_close(&state->mapping);
  free(state);
  queue->implementation = NULL;
}

void laghu_runtime_queue_init(laghu_runtime_queue *queue) {
  if (queue != NULL) queue->implementation = NULL;
}

bool laghu_runtime_queue_create(laghu_runtime_queue *queue, const char *path, unsigned int slot_count, size_t slot_payload_size) {
  laghu_runtime_queue_state *state;
  unsigned char *header;
  size_t length;
  if (queue == NULL || path == NULL || !laghu_queue_size(slot_count, slot_payload_size, &length)) return false;
  laghu_runtime_queue_close(queue);
  state = calloc(1U, sizeof(*state));
  if (state == NULL) return false;
  laghu_runtime_shared_mapping_init(&state->mapping);
  if (!laghu_runtime_shared_mapping_open(&state->mapping, path, length)) {
    free(state);
    return false;
  }
  state->slot_count = slot_count;
  state->slot_payload_size = slot_payload_size;
  header = laghu_queue_header(state);
  memset(header, 0, length);
  laghu_wire_u64_write(header + LAGHU_WIRE_QUEUE_HEADER_MAGIC_OFFSET, LAGHU_WIRE_QUEUE_MAGIC);
  laghu_wire_u32_write(header + LAGHU_WIRE_QUEUE_HEADER_VERSION_OFFSET, LAGHU_WIRE_QUEUE_VERSION);
  laghu_wire_u32_write(header + LAGHU_WIRE_QUEUE_HEADER_SLOT_COUNT_OFFSET, slot_count);
  laghu_wire_u64_write(header + LAGHU_WIRE_QUEUE_HEADER_SLOT_PAYLOAD_SIZE_OFFSET, slot_payload_size);
  if (!laghu_runtime_shared_mapping_sync(&state->mapping)) {
    laghu_runtime_shared_mapping_close(&state->mapping);
    free(state);
    return false;
  }
  queue->implementation = state;
  return true;
}

bool laghu_runtime_queue_open(laghu_runtime_queue *queue, const char *path) {
  unsigned char header[LAGHU_WIRE_QUEUE_HEADER_SIZE];
  laghu_runtime_queue_state *state;
  laghu_runtime_shared_mapping prefix;
  uint64_t payload_size;
  uint32_t slot_count;
  size_t length;
  if (queue == NULL || path == NULL) return false;
  if (queue->implementation != NULL) return true;
  laghu_runtime_shared_mapping_init(&prefix);
  if (!laghu_runtime_shared_mapping_open_prefix(&prefix, path, sizeof(header))) return false;
  memcpy(header, prefix.mapping, sizeof(header));
  laghu_runtime_shared_mapping_close(&prefix);
  if (laghu_wire_u64_read(header + LAGHU_WIRE_QUEUE_HEADER_MAGIC_OFFSET) != LAGHU_WIRE_QUEUE_MAGIC ||
      laghu_wire_u32_read(header + LAGHU_WIRE_QUEUE_HEADER_VERSION_OFFSET) != LAGHU_WIRE_QUEUE_VERSION ||
      !laghu_wire_string_valid(header + LAGHU_WIRE_QUEUE_HEADER_BACKEND_ID_OFFSET, LAGHU_RUNTIME_BACKEND_SIZE) ||
      !laghu_wire_zeroes(header + 36U, 4U))
    return false;
  slot_count = laghu_wire_u32_read(header + LAGHU_WIRE_QUEUE_HEADER_SLOT_COUNT_OFFSET);
  payload_size = laghu_wire_u64_read(header + LAGHU_WIRE_QUEUE_HEADER_SLOT_PAYLOAD_SIZE_OFFSET);
  if (payload_size > SIZE_MAX || !laghu_queue_size(slot_count, (size_t)payload_size, &length)) return false;
  laghu_runtime_queue_close(queue);
  state = calloc(1U, sizeof(*state));
  if (state == NULL) return false;
  laghu_runtime_shared_mapping_init(&state->mapping);
  /* Header and exact size were validated before mapping; dimensions never
   * change after queue creation, so attaching must not contend with writers. */
  if (!laghu_runtime_shared_mapping_open_existing(&state->mapping, path, length)) {
    free(state);
    return false;
  }
  state->slot_count = slot_count;
  state->slot_payload_size = (size_t)payload_size;
  if (!laghu_queue_header_valid(laghu_queue_header(state), state)) {
    laghu_runtime_shared_mapping_close(&state->mapping);
    free(state);
    return false;
  }
  queue->implementation = state;
  return true;
}

bool laghu_runtime_queue_move(laghu_runtime_queue *destination, laghu_runtime_queue *source) {
  if (destination == NULL || source == NULL || destination->implementation != NULL || source->implementation == NULL) return false;
  destination->implementation = source->implementation;
  source->implementation = NULL;
  return true;
}

bool laghu_runtime_queue_snapshot_get(const laghu_runtime_queue *queue, laghu_runtime_queue_snapshot *snapshot) {
  laghu_runtime_queue_state *state;
  unsigned char *header;
  unsigned int index;
  uint64_t occupied = 0U;
  if (queue == NULL || snapshot == NULL || queue->implementation == NULL) return false;
  state = (laghu_runtime_queue_state *)queue->implementation;
  if (!laghu_runtime_shared_mapping_try_lock(&state->mapping)) return false;
  header = laghu_queue_header(state);
  if (!laghu_queue_header_valid(header, state)) {
    laghu_runtime_shared_mapping_unlock(&state->mapping);
    return false;
  }
  for (index = 0U; index < state->slot_count; ++index)
    if (laghu_wire_u32_read(laghu_queue_slot(state, index) + LAGHU_WIRE_QUEUE_SLOT_STATE_OFFSET) == LAGHU_WIRE_QUEUE_SLOT_READY) ++occupied;
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->capabilities = laghu_wire_u32_read(header + LAGHU_WIRE_QUEUE_HEADER_CAPABILITIES_OFFSET);
  snapshot->worker_heartbeat = laghu_wire_u64_read(header + LAGHU_WIRE_QUEUE_HEADER_HEARTBEAT_OFFSET);
  memcpy(snapshot->backend_id, header + LAGHU_WIRE_QUEUE_HEADER_BACKEND_ID_OFFSET, sizeof(snapshot->backend_id));
  snapshot->capacity = state->slot_count;
  snapshot->occupied = occupied;
  snapshot->payload_capacity = state->slot_payload_size;
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  return true;
}

bool laghu_runtime_queue_refresh(laghu_runtime_queue *queue) {
  laghu_runtime_queue_snapshot snapshot;
  return laghu_runtime_queue_snapshot_get(queue, &snapshot);
}

bool laghu_runtime_queue_set_backend(laghu_runtime_queue *queue, uint32_t capabilities, const char *backend_id) {
  laghu_runtime_queue_state *state;
  unsigned char *header;
  bool success;
  if (queue == NULL || queue->implementation == NULL || backend_id == NULL) return false;
  state = LAGHU_QUEUE_STATE(queue);
  if (!laghu_runtime_shared_mapping_try_lock(&state->mapping)) return false;
  header = laghu_queue_header(state);
  success = laghu_queue_header_valid(header, state) &&
            laghu_queue_copy(header + LAGHU_WIRE_QUEUE_HEADER_BACKEND_ID_OFFSET, LAGHU_RUNTIME_BACKEND_SIZE, backend_id);
  if (success) laghu_wire_u32_write(header + LAGHU_WIRE_QUEUE_HEADER_CAPABILITIES_OFFSET, capabilities);
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  return success;
}

bool laghu_runtime_queue_heartbeat(laghu_runtime_queue *queue, uint64_t epoch_seconds) {
  laghu_runtime_queue_state *state;
  unsigned char *header;
  bool success;
  if (queue == NULL || queue->implementation == NULL || epoch_seconds == 0U) return false;
  state = LAGHU_QUEUE_STATE(queue);
  if (!laghu_runtime_shared_mapping_try_lock(&state->mapping)) return false;
  header = laghu_queue_header(state);
  success = laghu_queue_header_valid(header, state);
  if (success) laghu_wire_u64_write(header + LAGHU_WIRE_QUEUE_HEADER_HEARTBEAT_OFFSET, epoch_seconds);
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  return success;
}

bool laghu_runtime_queue_status(laghu_runtime_queue *queue, uint64_t *capacity, uint64_t *occupied) {
  laghu_runtime_queue_snapshot snapshot;
  if (capacity == NULL || occupied == NULL || !laghu_runtime_queue_snapshot_get(queue, &snapshot)) return false;
  *capacity = snapshot.capacity;
  *occupied = snapshot.occupied;
  return true;
}

void laghu_runtime_queue_close(laghu_runtime_queue *queue) { laghu_queue_state_release(queue); }

static void laghu_queue_job_encode(unsigned char *slot, const laghu_runtime_job *job) {
  unsigned int index;
  laghu_wire_u64_write(slot + LAGHU_WIRE_QUEUE_SLOT_PAYLOAD_LENGTH_OFFSET, job->payload.length);
  laghu_wire_u32_write(slot + LAGHU_WIRE_QUEUE_SLOT_KIND_OFFSET, job->kind);
  (void)laghu_queue_copy(slot + LAGHU_WIRE_QUEUE_SLOT_INDEX_KEY_OFFSET, LAGHU_RUNTIME_KEY_SIZE, job->index_key);
  (void)laghu_queue_copy(slot + LAGHU_WIRE_QUEUE_SLOT_REQUEST_PATH_OFFSET, LAGHU_RUNTIME_PATH_SIZE, job->request_path);
  (void)laghu_queue_copy(slot + LAGHU_WIRE_QUEUE_SLOT_VALIDATOR_OFFSET, LAGHU_RUNTIME_VALIDATOR_SIZE, job->validator);
  (void)laghu_queue_copy(slot + LAGHU_WIRE_QUEUE_SLOT_CONTENT_TYPE_OFFSET, LAGHU_RUNTIME_TYPE_SIZE, job->content_type);
  (void)laghu_queue_copy(slot + LAGHU_WIRE_QUEUE_SLOT_POLICY_KEY_OFFSET, LAGHU_RUNTIME_KEY_SIZE, job->policy_key);
  (void)laghu_queue_copy(slot + LAGHU_WIRE_QUEUE_SLOT_PROVIDER_ID_OFFSET, LAGHU_FONT_PROVIDER_ID_SIZE, job->provider_id);
  (void)laghu_queue_copy(slot + LAGHU_WIRE_QUEUE_SLOT_PROVIDER_DIGEST_OFFSET, LAGHU_RUNTIME_KEY_SIZE, job->provider_digest);
  (void)laghu_queue_copy(slot + LAGHU_WIRE_QUEUE_SLOT_JAVASCRIPT_TARGET_OFFSET, LAGHU_JAVASCRIPT_TARGET_SIZE, job->javascript_target);
  laghu_wire_u64_write(slot + LAGHU_WIRE_QUEUE_SLOT_FILTERS_OFFSET, job->filters);
  laghu_wire_u32_write(slot + LAGHU_WIRE_QUEUE_SLOT_QUALITY_OFFSET, job->quality);
  laghu_wire_u32_write(slot + LAGHU_WIRE_QUEUE_SLOT_METADATA_LIMIT_OFFSET, job->metadata_limit);
  laghu_wire_u32_write(slot + LAGHU_WIRE_QUEUE_SLOT_METADATA_TTL_OFFSET, job->metadata_ttl);
  laghu_wire_u32_write(slot + LAGHU_WIRE_QUEUE_SLOT_TARGET_COUNT_OFFSET, job->target_count);
  for (index = 0U; index < LAGHU_RUNTIME_MAX_TARGETS; ++index) {
    laghu_wire_u32_write(slot + LAGHU_WIRE_QUEUE_SLOT_TARGET_WIDTH_OFFSET + index * 4U, job->target_width[index]);
    laghu_wire_u32_write(slot + LAGHU_WIRE_QUEUE_SLOT_TARGET_HEIGHT_OFFSET + index * 4U, job->target_height[index]);
    laghu_wire_u64_write(slot + LAGHU_WIRE_QUEUE_SLOT_RESIZE_FILTER_OFFSET + index * 8U, job->resize_filter[index]);
  }
  laghu_wire_u32_write(slot + LAGHU_WIRE_QUEUE_SLOT_SPRITE_COUNT_OFFSET, job->sprite_count);
  for (index = 0U; index < LAGHU_RUNTIME_MAX_SPRITE_INPUTS; ++index) {
    memcpy(slot + LAGHU_WIRE_QUEUE_SLOT_SPRITE_KEYS_OFFSET + index * LAGHU_RUNTIME_KEY_SIZE, job->sprite_variant_keys[index], LAGHU_RUNTIME_KEY_SIZE);
    laghu_wire_u32_write(slot + LAGHU_WIRE_QUEUE_SLOT_SPRITE_WIDTH_OFFSET + index * 4U, job->sprite_width[index]);
    laghu_wire_u32_write(slot + LAGHU_WIRE_QUEUE_SLOT_SPRITE_HEIGHT_OFFSET + index * 4U, job->sprite_height[index]);
  }
  slot[LAGHU_WIRE_QUEUE_SLOT_ALLOW_LOSSY_OFFSET] = job->allow_lossy ? 1U : 0U;
  slot[LAGHU_WIRE_QUEUE_SLOT_ACCEPT_WEBP_OFFSET] = job->accept_webp ? 1U : 0U;
  slot[LAGHU_WIRE_QUEUE_SLOT_ACCEPT_AVIF_OFFSET] = job->accept_avif ? 1U : 0U;
  laghu_wire_u32_write(slot + LAGHU_WIRE_QUEUE_SLOT_ANALYSIS_TIMEOUT_MS_OFFSET, job->analysis_timeout_ms);
}

bool laghu_runtime_queue_try_publish(laghu_runtime_queue *queue, const laghu_runtime_job *job) {
  laghu_runtime_queue_state *state;
  unsigned char *header, *slot;
  uint32_t next;
  bool success = false;
  if (queue == NULL || queue->implementation == NULL) return false;
  state = LAGHU_QUEUE_STATE(queue);
  if (!laghu_queue_job_valid(job, state->slot_payload_size) || !laghu_runtime_shared_mapping_try_lock(&state->mapping)) return false;
  header = laghu_queue_header(state);
  if (laghu_queue_header_valid(header, state)) {
    next = laghu_wire_u32_read(header + LAGHU_WIRE_QUEUE_HEADER_NEXT_WRITE_OFFSET);
    slot = laghu_queue_slot(state, next % state->slot_count);
    if (laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_STATE_OFFSET) == LAGHU_WIRE_QUEUE_SLOT_EMPTY) {
      memset(slot, 0, LAGHU_WIRE_QUEUE_SLOT_HEADER_SIZE);
      laghu_queue_job_encode(slot, job);
      if (job->payload.length != 0U) memcpy(laghu_queue_payload(slot), job->payload.data, job->payload.length);
      laghu_wire_u32_write(slot + LAGHU_WIRE_QUEUE_SLOT_STATE_OFFSET, LAGHU_WIRE_QUEUE_SLOT_READY);
      laghu_wire_u32_write(header + LAGHU_WIRE_QUEUE_HEADER_NEXT_WRITE_OFFSET, (next + 1U) % state->slot_count);
      success = true;
    }
  }
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  return success;
}

static void laghu_queue_job_decode(const unsigned char *slot, laghu_runtime_job *job, unsigned char *payload, size_t payload_length) {
  unsigned int index;
  memset(job, 0, sizeof(*job));
  job->kind = (laghu_runtime_job_kind)laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_KIND_OFFSET);
  memcpy(job->index_key, slot + LAGHU_WIRE_QUEUE_SLOT_INDEX_KEY_OFFSET, sizeof(job->index_key));
  memcpy(job->request_path, slot + LAGHU_WIRE_QUEUE_SLOT_REQUEST_PATH_OFFSET, sizeof(job->request_path));
  memcpy(job->validator, slot + LAGHU_WIRE_QUEUE_SLOT_VALIDATOR_OFFSET, sizeof(job->validator));
  memcpy(job->content_type, slot + LAGHU_WIRE_QUEUE_SLOT_CONTENT_TYPE_OFFSET, sizeof(job->content_type));
  memcpy(job->policy_key, slot + LAGHU_WIRE_QUEUE_SLOT_POLICY_KEY_OFFSET, sizeof(job->policy_key));
  memcpy(job->provider_id, slot + LAGHU_WIRE_QUEUE_SLOT_PROVIDER_ID_OFFSET, sizeof(job->provider_id));
  memcpy(job->provider_digest, slot + LAGHU_WIRE_QUEUE_SLOT_PROVIDER_DIGEST_OFFSET, sizeof(job->provider_digest));
  memcpy(job->javascript_target, slot + LAGHU_WIRE_QUEUE_SLOT_JAVASCRIPT_TARGET_OFFSET, sizeof(job->javascript_target));
  job->filters = laghu_wire_u64_read(slot + LAGHU_WIRE_QUEUE_SLOT_FILTERS_OFFSET);
  job->quality = laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_QUALITY_OFFSET);
  job->metadata_limit = laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_METADATA_LIMIT_OFFSET);
  job->metadata_ttl = laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_METADATA_TTL_OFFSET);
  job->target_count = laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_TARGET_COUNT_OFFSET);
  for (index = 0U; index < LAGHU_RUNTIME_MAX_TARGETS; ++index) {
    job->target_width[index] = laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_TARGET_WIDTH_OFFSET + index * 4U);
    job->target_height[index] = laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_TARGET_HEIGHT_OFFSET + index * 4U);
    job->resize_filter[index] = laghu_wire_u64_read(slot + LAGHU_WIRE_QUEUE_SLOT_RESIZE_FILTER_OFFSET + index * 8U);
  }
  job->sprite_count = laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_SPRITE_COUNT_OFFSET);
  for (index = 0U; index < LAGHU_RUNTIME_MAX_SPRITE_INPUTS; ++index) {
    memcpy(job->sprite_variant_keys[index], slot + LAGHU_WIRE_QUEUE_SLOT_SPRITE_KEYS_OFFSET + index * LAGHU_RUNTIME_KEY_SIZE, LAGHU_RUNTIME_KEY_SIZE);
    job->sprite_width[index] = laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_SPRITE_WIDTH_OFFSET + index * 4U);
    job->sprite_height[index] = laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_SPRITE_HEIGHT_OFFSET + index * 4U);
  }
  job->allow_lossy = slot[LAGHU_WIRE_QUEUE_SLOT_ALLOW_LOSSY_OFFSET] != 0U;
  job->accept_webp = slot[LAGHU_WIRE_QUEUE_SLOT_ACCEPT_WEBP_OFFSET] != 0U;
  job->accept_avif = slot[LAGHU_WIRE_QUEUE_SLOT_ACCEPT_AVIF_OFFSET] != 0U;
  job->analysis_timeout_ms = laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_ANALYSIS_TIMEOUT_MS_OFFSET);
  if (payload_length != 0U) memcpy(payload, laghu_queue_payload((unsigned char *)slot), payload_length);
  job->payload = (laghu_buffer){payload, payload_length};
}

bool laghu_runtime_queue_try_take(laghu_runtime_queue *queue, laghu_runtime_job *job, unsigned char *payload, size_t payload_capacity) {
  laghu_runtime_queue_state *state;
  unsigned char *header, *slot;
  uint32_t next;
  uint64_t payload_length;
  bool success = false;
  if (queue == NULL || queue->implementation == NULL || job == NULL || payload == NULL) return false;
  state = LAGHU_QUEUE_STATE(queue);
  if (!laghu_runtime_shared_mapping_try_lock(&state->mapping)) return false;
  header = laghu_queue_header(state);
  if (laghu_queue_header_valid(header, state)) {
    next = laghu_wire_u32_read(header + LAGHU_WIRE_QUEUE_HEADER_NEXT_READ_OFFSET);
    slot = laghu_queue_slot(state, next % state->slot_count);
    if (laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_STATE_OFFSET) == LAGHU_WIRE_QUEUE_SLOT_READY) {
      payload_length = laghu_wire_u64_read(slot + LAGHU_WIRE_QUEUE_SLOT_PAYLOAD_LENGTH_OFFSET);
      if (payload_length <= payload_capacity && payload_length <= state->slot_payload_size &&
          laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_KIND_OFFSET) <= LAGHU_RUNTIME_JOB_BROWSER_ANALYSIS &&
          laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_TARGET_COUNT_OFFSET) <= LAGHU_RUNTIME_MAX_TARGETS &&
          laghu_wire_u32_read(slot + LAGHU_WIRE_QUEUE_SLOT_SPRITE_COUNT_OFFSET) <= LAGHU_RUNTIME_MAX_SPRITE_INPUTS &&
          laghu_queue_slot_strings_valid(slot) && laghu_wire_zeroes(slot + 4539U, 5U)) {
        laghu_queue_job_decode(slot, job, payload, (size_t)payload_length);
        if (!laghu_queue_job_valid(job, state->slot_payload_size))
          memset(job, 0, sizeof(*job));
        else
          success = true;
      }
      memset(laghu_queue_payload(slot), 0, state->slot_payload_size);
      memset(slot, 0, LAGHU_WIRE_QUEUE_SLOT_HEADER_SIZE);
      laghu_wire_u32_write(header + LAGHU_WIRE_QUEUE_HEADER_NEXT_READ_OFFSET, (next + 1U) % state->slot_count);
    }
  }
  laghu_runtime_shared_mapping_unlock(&state->mapping);
  return success;
}
