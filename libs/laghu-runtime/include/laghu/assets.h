// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_ASSETS_H
#define LAGHU_ASSETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"
#include "laghu/image.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_ASSET_MAX_RULES 32U
#define LAGHU_ASSET_MAX_BODY_BYTES (64U * 1024U * 1024U)
#define LAGHU_ASSET_CONFIG_VERSION 1U
#define LAGHU_ASSET_DEFAULT_RETRIES 3U
#define LAGHU_ASSET_DEFAULT_TIMEOUT 10U
#define LAGHU_ASSET_DEFAULT_STALE_TTL 86400U
#define LAGHU_ASSET_MAX_SHARDS 8U
#define LAGHU_ASSET_MAX_JOBS 4096U
#define LAGHU_ASSET_MAX_RECORDS 10000U
typedef enum {
  LAGHU_ASSET_PENDING = 0,
  LAGHU_ASSET_UPLOADING,
  LAGHU_ASSET_READY,
  LAGHU_ASSET_RETRYABLE_FAILURE,
  LAGHU_ASSET_PERMANENT_FAILURE,
  LAGHU_ASSET_STALE
} laghu_asset_state;

typedef struct {
  char source_domain[LAGHU_RUNTIME_PATH_SIZE];
  char public_domain[LAGHU_RUNTIME_PATH_SIZE];
  char source_prefix[LAGHU_RUNTIME_PATH_SIZE];
  char public_prefix[LAGHU_RUNTIME_PATH_SIZE];
  char mime_types[LAGHU_MIME_ALLOWLIST_SIZE];
  char allow_paths[LAGHU_MIME_ALLOWLIST_SIZE];
  char deny_paths[LAGHU_MIME_ALLOWLIST_SIZE];
  char shards[LAGHU_ASSET_MAX_SHARDS][LAGHU_RUNTIME_PATH_SIZE];
  unsigned int shard_count;
  bool upload;
  bool preserve_query;
  bool trusted_origin_fallback;
  size_t max_body_bytes;
  unsigned int retry_limit;
  unsigned int timeout_seconds;
  unsigned int stale_ttl_seconds;
} laghu_asset_policy;

typedef struct {
  laghu_asset_state state;
  char source_url[LAGHU_RUNTIME_PATH_SIZE];
  char object_key[LAGHU_RUNTIME_PATH_SIZE];
  char content_hash[LAGHU_RUNTIME_KEY_SIZE];
  char content_type[LAGHU_RUNTIME_TYPE_SIZE];
  char source_validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  char policy_digest[LAGHU_RUNTIME_KEY_SIZE];
  char provider_digest[LAGHU_RUNTIME_KEY_SIZE];
  size_t body_length;
  uint64_t updated_at;
  uint64_t retry_after;
  unsigned int attempts;
} laghu_asset_record;

typedef struct {
  unsigned int version;
  laghu_asset_policy policy;
  char catalog_path[LAGHU_RUNTIME_PATH_SIZE];
  char queue_path[LAGHU_RUNTIME_PATH_SIZE];
  char operational_cache_path[LAGHU_RUNTIME_PATH_SIZE];
  char provider[LAGHU_RUNTIME_TYPE_SIZE];
  char endpoint[LAGHU_RUNTIME_PATH_SIZE];
  char region[LAGHU_RUNTIME_TYPE_SIZE];
  char bucket[LAGHU_RUNTIME_PATH_SIZE];
  char object_prefix[LAGHU_RUNTIME_PATH_SIZE];
  char access_key_env[LAGHU_RUNTIME_TYPE_SIZE];
  char secret_key_env[LAGHU_RUNTIME_TYPE_SIZE];
  char digest[LAGHU_RUNTIME_KEY_SIZE];
} laghu_asset_config;

typedef enum { LAGHU_ASSET_PROVIDER_OK = 0, LAGHU_ASSET_PROVIDER_RETRYABLE, LAGHU_ASSET_PROVIDER_PERMANENT } laghu_asset_provider_result;

typedef struct {
  laghu_asset_provider_result (*upload)(void *context, const char *object_key, laghu_buffer body, const char *content_type, const char *checksum);
  laghu_asset_provider_result (*verify)(void *context, const char *object_key, size_t size, const char *content_type, const char *checksum);
  laghu_asset_provider_result (*purge)(void *context, const char *object_key);
  bool (*healthy)(void *context);
  void *context;
} laghu_asset_provider;
void laghu_asset_policy_init(laghu_asset_policy *policy);
bool laghu_asset_policy_validate(const laghu_asset_policy *policy, char *error, size_t error_size);
bool laghu_asset_url_rewrite(const laghu_asset_policy *policy, const char *source_url, const char *content_type, const char *content_hash,
                             char *output, size_t output_size);
bool laghu_asset_source_allowed(const laghu_asset_policy *policy, const char *source_url, const char *content_type, size_t body_length);
/* The response path may call this helper; it performs no I/O and publishes no
 * work.  Only a verified, atomically published READY record is rewriteable. */
bool laghu_asset_record_rewrite(const laghu_asset_policy *policy, const laghu_asset_record *record, char *output, size_t output_size);
bool laghu_asset_object_key(const laghu_asset_policy *policy, const char *source_url, const char *content_hash, char output[LAGHU_RUNTIME_PATH_SIZE]);
void laghu_asset_config_init(laghu_asset_config *config);
bool laghu_asset_config_load(const char *path, laghu_asset_config *config, char *error, size_t error_size);
bool laghu_asset_catalog_key(const laghu_asset_record *record, char output[LAGHU_RUNTIME_KEY_SIZE]);
bool laghu_asset_catalog_publish(const char *catalog_path, const laghu_asset_record *record);
bool laghu_asset_catalog_lookup(const char *catalog_path, const char *key, laghu_asset_record *record);
bool laghu_asset_catalog_lookup_url(const laghu_asset_config *config, const char *source_url, laghu_asset_record *record);
bool laghu_asset_rewrite_document(const laghu_asset_config *config, laghu_buffer input, unsigned char **output, size_t *output_length);
bool laghu_asset_rewrite_document_at(const laghu_asset_config *config, laghu_buffer input, const char *page_path, unsigned char **output,
                                     size_t *output_length);
bool laghu_asset_job_publish(const laghu_asset_config *config, const laghu_asset_record *record, laghu_buffer body);
bool laghu_asset_job_take(const laghu_asset_config *config, laghu_asset_record *record, unsigned char **body, size_t *body_length,
                          char job_path[LAGHU_RUNTIME_PATH_SIZE]);
bool laghu_asset_job_complete(const char *job_path);
uint64_t laghu_asset_retry_after(const laghu_asset_policy *policy, unsigned int attempts, uint64_t now);

#ifdef __cplusplus
}
#endif

#endif
