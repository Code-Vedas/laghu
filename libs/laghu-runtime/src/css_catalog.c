// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/cache.h"
#include "laghu/css.h"
#include "laghu/types.h"

static bool laghu_stylesheet_key(const char *normalized_url, const char *policy_key, uint32_t capability_mask, unsigned int inline_limit,
                                 unsigned int outline_threshold, char output[LAGHU_RUNTIME_KEY_SIZE]) {
  char material[LAGHU_RUNTIME_PATH_SIZE + LAGHU_RUNTIME_KEY_SIZE + 96U];
  int length;
  if (normalized_url == NULL || normalized_url[0] != '/' || policy_key == NULL) {
    return false;
  }
  length = snprintf(material, sizeof(material), "stylesheet-catalog-v%u\n%s\n%s\n%u\n%u\n%u", LAGHU_STYLESHEET_CATALOG_VERSION, normalized_url,
                    policy_key, capability_mask, inline_limit, outline_threshold);
  return length > 0 && (size_t)length < sizeof(material) && laghu_sha256_hex((laghu_buffer){(const unsigned char *)material, (size_t)length}, output);
}

bool laghu_stylesheet_publish(const char *cache_path, const laghu_stylesheet_record *record) {
  char key[LAGHU_RUNTIME_KEY_SIZE];
  laghu_runtime_cache_entry entry;
  if (record == NULL || record->version != LAGHU_STYLESHEET_CATALOG_VERSION ||
      !laghu_stylesheet_key(record->normalized_url, record->policy_key, record->capability_mask, record->inline_limit, record->outline_threshold,
                            key)) {
    return false;
  }
  return laghu_runtime_cache_publish(cache_path, key, key, record->source_hash, "application/x-laghu-stylesheet-catalog", "laghu-css-catalog",
                                     (laghu_buffer){(const unsigned char *)record, sizeof(*record)}, &entry);
}

bool laghu_stylesheet_lookup(const char *cache_path, const char *normalized_url, const char *policy_key, uint32_t capability_mask,
                             unsigned int inline_limit, unsigned int outline_threshold, uint64_t now, unsigned int ttl_seconds,
                             laghu_stylesheet_record *record) {
  char key[LAGHU_RUNTIME_KEY_SIZE];
  laghu_runtime_cache_entry entry;
  if (record == NULL || ttl_seconds == 0U ||
      !laghu_stylesheet_key(normalized_url, policy_key, capability_mask, inline_limit, outline_threshold, key) ||
      !laghu_runtime_cache_lookup_variant(cache_path, key, &entry) || entry.length != sizeof(*record) ||
      !laghu_runtime_cache_read(&entry, (unsigned char *)record, sizeof(*record)) || record->version != LAGHU_STYLESHEET_CATALOG_VERSION ||
      strcmp(record->normalized_url, normalized_url) != 0 || strcmp(record->policy_key, policy_key) != 0 ||
      record->capability_mask != capability_mask || record->inline_limit != inline_limit || record->outline_threshold != outline_threshold ||
      now < record->updated_at || now - record->updated_at > ttl_seconds) {
    return false;
  }
  return true;
}
