// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/cache.h"
#include "laghu/types.h"
#include "persisted_state_wire.h"
#include "runtime_platform.h"

bool laghu_runtime_index_key(const char *request_path, const char *validator, const char *policy_key, bool accept_webp, bool accept_avif,
                             char output[LAGHU_RUNTIME_KEY_SIZE]) {
  size_t path_length, validator_length, policy_length, total;
  unsigned char *canonical;
  int written;
  bool success;
  if (request_path == NULL || validator == NULL || policy_key == NULL || output == NULL) return false;
  path_length = strlen(request_path);
  validator_length = strlen(validator);
  policy_length = strlen(policy_key);
  if (validator_length > SIZE_MAX - 7U || path_length > SIZE_MAX - validator_length - 7U ||
      path_length + validator_length + 7U > SIZE_MAX - policy_length) {
    output[0] = '\0';
    return false;
  }
  total = path_length + validator_length + policy_length + 7U;
  canonical = malloc(total);
  if (canonical == NULL) {
    output[0] = '\0';
    return false;
  }
  written =
      snprintf((char *)canonical, total, "%s\n%s\n%s\n%c\n%c", request_path, validator, policy_key, accept_webp ? '1' : '0', accept_avif ? '1' : '0');
  success = written >= 0 && (size_t)written < total && laghu_sha256_hex((laghu_buffer){canonical, (size_t)written}, output);
  free(canonical);
  return success;
}

static bool laghu_artifact_hash_valid(const char *value) {
  size_t index;
  if (value == NULL || value[LAGHU_RUNTIME_KEY_SIZE - 1U] != '\0') return false;
  for (index = 0U; index + 1U < LAGHU_RUNTIME_KEY_SIZE; ++index)
    if (!((value[index] >= '0' && value[index] <= '9') || (value[index] >= 'a' && value[index] <= 'f'))) return false;
  return true;
}

static bool laghu_artifact_copy(unsigned char *target, size_t target_size, const char *source) {
  size_t length;
  if (target == NULL || source == NULL || target_size == 0U) return false;
  length = strlen(source);
  if (length >= target_size) return false;
  memset(target, 0, target_size);
  memcpy(target, source, length);
  return true;
}

static bool laghu_artifact_paths(const char *cache_path, const char *index_key, const char *variant_key, char *index_path, size_t index_size,
                                 char *variant_path, size_t variant_size) {
  int index_length = snprintf(index_path, index_size, "%s/index-%s.meta", cache_path, index_key);
  int variant_length = snprintf(variant_path, variant_size, "%s/variant-%s.bin", cache_path, variant_key);
  return cache_path != NULL && index_key != NULL && variant_key != NULL && index_length > 0 && (size_t)index_length < index_size &&
         variant_length > 0 && (size_t)variant_length < variant_size;
}

static bool laghu_artifact_metadata_encode(unsigned char output[LAGHU_WIRE_CACHE_ARTIFACT_SIZE], const char *variant_key, const char *payload_hash,
                                           const char *validator, const char *content_type, const char *backend_id, size_t length) {
  if (length > UINT64_MAX || !laghu_artifact_hash_valid(variant_key) || !laghu_artifact_hash_valid(payload_hash)) return false;
  memset(output, 0, LAGHU_WIRE_CACHE_ARTIFACT_SIZE);
  laghu_wire_u64_write(output + LAGHU_WIRE_CACHE_ARTIFACT_MAGIC_OFFSET, LAGHU_WIRE_CACHE_ARTIFACT_MAGIC);
  laghu_wire_u32_write(output + LAGHU_WIRE_CACHE_ARTIFACT_VERSION_OFFSET, LAGHU_WIRE_CACHE_ARTIFACT_VERSION);
  laghu_wire_u64_write(output + LAGHU_WIRE_CACHE_ARTIFACT_LENGTH_OFFSET, length);
  return laghu_artifact_copy(output + LAGHU_WIRE_CACHE_ARTIFACT_VARIANT_KEY_OFFSET, LAGHU_RUNTIME_KEY_SIZE, variant_key) &&
         laghu_artifact_copy(output + LAGHU_WIRE_CACHE_ARTIFACT_PAYLOAD_HASH_OFFSET, LAGHU_RUNTIME_KEY_SIZE, payload_hash) &&
         laghu_artifact_copy(output + LAGHU_WIRE_CACHE_ARTIFACT_VALIDATOR_OFFSET, LAGHU_RUNTIME_VALIDATOR_SIZE, validator) &&
         laghu_artifact_copy(output + LAGHU_WIRE_CACHE_ARTIFACT_CONTENT_TYPE_OFFSET, LAGHU_RUNTIME_TYPE_SIZE, content_type) &&
         laghu_artifact_copy(output + LAGHU_WIRE_CACHE_ARTIFACT_BACKEND_ID_OFFSET, LAGHU_RUNTIME_BACKEND_SIZE, backend_id);
}

static bool laghu_artifact_metadata_decode(const unsigned char input[LAGHU_WIRE_CACHE_ARTIFACT_SIZE], laghu_runtime_cache_entry *entry) {
  uint64_t length;
  if (input == NULL || entry == NULL || laghu_wire_u64_read(input + LAGHU_WIRE_CACHE_ARTIFACT_MAGIC_OFFSET) != LAGHU_WIRE_CACHE_ARTIFACT_MAGIC ||
      laghu_wire_u32_read(input + LAGHU_WIRE_CACHE_ARTIFACT_VERSION_OFFSET) != LAGHU_WIRE_CACHE_ARTIFACT_VERSION ||
      !laghu_artifact_hash_valid((const char *)(input + LAGHU_WIRE_CACHE_ARTIFACT_VARIANT_KEY_OFFSET)) ||
      !laghu_artifact_hash_valid((const char *)(input + LAGHU_WIRE_CACHE_ARTIFACT_PAYLOAD_HASH_OFFSET)) ||
      !laghu_wire_string_valid(input + LAGHU_WIRE_CACHE_ARTIFACT_VALIDATOR_OFFSET, LAGHU_RUNTIME_VALIDATOR_SIZE) ||
      !laghu_wire_string_valid(input + LAGHU_WIRE_CACHE_ARTIFACT_CONTENT_TYPE_OFFSET, LAGHU_RUNTIME_TYPE_SIZE) ||
      !laghu_wire_string_valid(input + LAGHU_WIRE_CACHE_ARTIFACT_BACKEND_ID_OFFSET, LAGHU_RUNTIME_BACKEND_SIZE) ||
      !laghu_wire_zeroes(input + LAGHU_WIRE_CACHE_ARTIFACT_BACKEND_ID_END_OFFSET,
                         LAGHU_WIRE_CACHE_ARTIFACT_SIZE - LAGHU_WIRE_CACHE_ARTIFACT_BACKEND_ID_END_OFFSET))
    return false;
  length = laghu_wire_u64_read(input + LAGHU_WIRE_CACHE_ARTIFACT_LENGTH_OFFSET);
  if (length == 0U || length > SIZE_MAX) return false;
  memset(entry, 0, sizeof(*entry));
  memcpy(entry->variant_key, input + LAGHU_WIRE_CACHE_ARTIFACT_VARIANT_KEY_OFFSET, sizeof(entry->variant_key));
  memcpy(entry->payload_hash, input + LAGHU_WIRE_CACHE_ARTIFACT_PAYLOAD_HASH_OFFSET, sizeof(entry->payload_hash));
  memcpy(entry->validator, input + LAGHU_WIRE_CACHE_ARTIFACT_VALIDATOR_OFFSET, sizeof(entry->validator));
  memcpy(entry->content_type, input + LAGHU_WIRE_CACHE_ARTIFACT_CONTENT_TYPE_OFFSET, sizeof(entry->content_type));
  memcpy(entry->backend_id, input + LAGHU_WIRE_CACHE_ARTIFACT_BACKEND_ID_OFFSET, sizeof(entry->backend_id));
  entry->length = (size_t)length;
  return true;
}

bool laghu_runtime_file_cache_publish(const char *cache_path, const char *index_key, const char *variant_key, const char *validator,
                                      const char *content_type, const char *backend_id, laghu_buffer payload, laghu_runtime_cache_entry *entry) {
  unsigned char metadata[LAGHU_WIRE_CACHE_ARTIFACT_SIZE];
  char index_path[LAGHU_RUNTIME_PATH_SIZE];
  char variant_path[LAGHU_RUNTIME_PATH_SIZE];
  char payload_hash[LAGHU_RUNTIME_KEY_SIZE];
  if (cache_path == NULL || index_key == NULL || variant_key == NULL || validator == NULL || content_type == NULL || backend_id == NULL ||
      entry == NULL || payload.data == NULL || payload.length == 0U || !laghu_runtime_directory_ensure(cache_path) ||
      !laghu_sha256_hex(payload, payload_hash) ||
      !laghu_artifact_metadata_encode(metadata, variant_key, payload_hash, validator, content_type, backend_id, payload.length) ||
      !laghu_artifact_paths(cache_path, index_key, variant_key, index_path, sizeof(index_path), variant_path, sizeof(variant_path)) ||
      !laghu_runtime_file_write_atomic(variant_path, payload.data, payload.length) ||
      !laghu_runtime_file_write_atomic(index_path, metadata, sizeof(metadata)))
    return false;
  if (strcmp(index_key, variant_key) != 0 &&
      (!laghu_artifact_paths(cache_path, variant_key, variant_key, index_path, sizeof(index_path), variant_path, sizeof(variant_path)) ||
       !laghu_runtime_file_write_atomic(index_path, metadata, sizeof(metadata))))
    return false;
  if (!laghu_artifact_metadata_decode(metadata, entry)) return false;
  return laghu_artifact_paths(cache_path, index_key, variant_key, index_path, sizeof(index_path), entry->variant_path, sizeof(entry->variant_path));
}

static bool laghu_artifact_lookup(const char *cache_path, const char *index_key, const char *validator, bool exact_variant,
                                  laghu_runtime_cache_entry *entry) {
  unsigned char metadata[LAGHU_WIRE_CACHE_ARTIFACT_SIZE];
  char index_path[LAGHU_RUNTIME_PATH_SIZE];
  char variant_path[LAGHU_RUNTIME_PATH_SIZE];
  uint64_t metadata_size, payload_size;
  laghu_runtime_cache_entry candidate;
  if (cache_path == NULL || index_key == NULL || entry == NULL || (exact_variant && !laghu_artifact_hash_valid(index_key)) ||
      !laghu_artifact_paths(cache_path, index_key, index_key, index_path, sizeof(index_path), variant_path, sizeof(variant_path)) ||
      !laghu_runtime_file_size(index_path, &metadata_size) || metadata_size != sizeof(metadata) ||
      !laghu_runtime_file_read_exact(index_path, metadata, sizeof(metadata)) || !laghu_artifact_metadata_decode(metadata, &candidate) ||
      (!exact_variant && (validator == NULL || strcmp(candidate.validator, validator) != 0)) ||
      (exact_variant && strcmp(candidate.variant_key, index_key) != 0) ||
      !laghu_artifact_paths(cache_path, index_key, candidate.variant_key, index_path, sizeof(index_path), variant_path, sizeof(variant_path)) ||
      !laghu_runtime_file_size(variant_path, &payload_size) || payload_size != candidate.length ||
      !laghu_artifact_copy((unsigned char *)candidate.variant_path, sizeof(candidate.variant_path), variant_path))
    return false;
  *entry = candidate;
  return true;
}

bool laghu_runtime_file_cache_lookup_variant(const char *cache_path, const char *variant_key, laghu_runtime_cache_entry *entry) {
  return laghu_artifact_lookup(cache_path, variant_key, NULL, true, entry);
}

bool laghu_runtime_file_cache_lookup(const char *cache_path, const char *index_key, const char *validator, laghu_runtime_cache_entry *entry) {
  return laghu_artifact_lookup(cache_path, index_key, validator, false, entry);
}

bool laghu_runtime_file_cache_read(const laghu_runtime_cache_entry *entry, unsigned char *output, size_t output_capacity) {
  char payload_hash[LAGHU_RUNTIME_KEY_SIZE];
  if (entry == NULL || output == NULL || entry->length == 0U || entry->length > output_capacity ||
      !laghu_runtime_file_read_exact(entry->variant_path, output, entry->length) ||
      !laghu_sha256_hex((laghu_buffer){output, entry->length}, payload_hash))
    return false;
  return strcmp(payload_hash, entry->payload_hash) == 0;
}
