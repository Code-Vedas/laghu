// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "laghu/runtime.h"

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define laghu_asset_mkdir(path) _mkdir(path)
#define laghu_asset_unlink(path) _unlink(path)
#define laghu_asset_pid() GetCurrentProcessId()
#define laghu_asset_replace(from, to) \
  MoveFileExA((from), (to), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define laghu_asset_mkdir(path) mkdir((path), 0750)
#define laghu_asset_unlink(path) unlink(path)
#define laghu_asset_pid() getpid()
#define laghu_asset_replace(from, to) (rename((from), (to)) == 0)
#endif

typedef struct {
  uint32_t version;
  laghu_asset_record record;
  char checksum[LAGHU_RUNTIME_KEY_SIZE];
} laghu_asset_catalog_file;

typedef struct {
  uint32_t version;
  laghu_asset_record record;
  size_t body_length;
  char body_hash[LAGHU_RUNTIME_KEY_SIZE];
} laghu_asset_job_file;

static void laghu_asset_error(char *error, size_t size, const char *message) {
  if (error != NULL && size != 0U) (void)snprintf(error, size, "%s", message);
}

static bool laghu_asset_copy(char *output, size_t size, const char *value) {
  size_t length = value == NULL ? 0U : strlen(value);
  if (output == NULL || length >= size) return false;
  memcpy(output, value == NULL ? "" : value, length + 1U);
  return true;
}

static char *laghu_asset_trim(char *value) {
  char *end;
  while (isspace((unsigned char)*value)) ++value;
  end = value + strlen(value);
  while (end > value && isspace((unsigned char)end[-1])) --end;
  *end = '\0';
  return value;
}

static bool laghu_asset_safe_path(const char *path) {
  return path != NULL && path[0] != '\0' &&
         strlen(path) < LAGHU_RUNTIME_PATH_SIZE && strstr(path, "..") == NULL &&
         strpbrk(path, "\r\n") == NULL;
}

static bool laghu_asset_hash_valid(const char *value) {
  size_t index;
  if (value == NULL || strlen(value) != LAGHU_SHA256_HEX_LENGTH) return false;
  for (index = 0U; index < LAGHU_SHA256_HEX_LENGTH; ++index)
    if (!isxdigit((unsigned char)value[index])) return false;
  return true;
}

static bool laghu_asset_source_identity(const char *source, char *output,
                                        size_t output_size) {
  const char *tail;
  size_t length;
  if (source == NULL || output == NULL || output_size == 0U) return false;
  tail = strpbrk(source, "?#");
  length = tail == NULL ? strlen(source) : (size_t)(tail - source);
  if (length == 0U || length >= output_size) return false;
  memcpy(output, source, length);
  output[length] = '\0';
  return true;
}

static bool laghu_asset_directory(const char *path) {
  if (!laghu_asset_safe_path(path)) return false;
  return laghu_asset_mkdir(path) == 0 || errno == EEXIST;
}

static bool laghu_asset_directory_has_capacity(const char *path,
                                               unsigned int limit) {
  unsigned int count = 0U;
#ifdef _WIN32
  WIN32_FIND_DATAA found;
  HANDLE search;
  char pattern[LAGHU_RUNTIME_PATH_SIZE];
  if (snprintf(pattern, sizeof(pattern), "%s/*", path) <= 0) return false;
  search = FindFirstFileA(pattern, &found);
  if (search == INVALID_HANDLE_VALUE) return true;
  do {
    if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
        ++count >= limit) {
      (void)FindClose(search);
      return false;
    }
  } while (FindNextFileA(search, &found));
  (void)FindClose(search);
#else
  DIR *directory = opendir(path);
  struct dirent *entry;
  if (directory == NULL) return false;
  while ((entry = readdir(directory)) != NULL)
    if (entry->d_name[0] != '.' && ++count >= limit) {
      (void)closedir(directory);
      return false;
    }
  (void)closedir(directory);
#endif
  return true;
}

static bool laghu_asset_https_origin(const char *value) {
  const char *authority;
  const char *end;
  if (value == NULL || strncmp(value, "https://", 8U) != 0) return false;
  authority = value + 8U;
  end = authority + strlen(authority);
  return authority[0] != '\0' && strchr(authority, '@') == NULL &&
         strchr(authority, '?') == NULL && strchr(authority, '#') == NULL &&
         strpbrk(authority, " \t\r\n") == NULL &&
         strncmp(authority, "localhost", 9U) != 0 &&
         !isdigit((unsigned char)authority[0]) && authority[0] != '[' &&
         (end == authority || end[-1] != '/');
}

static bool laghu_asset_path_list(const char *list, const char *path,
                                  bool empty_result) {
  const char *item = list;
  if (list == NULL || list[0] == '\0') return empty_result;
  while (*item != '\0') {
    const char *end = strchr(item, ',');
    size_t length = end == NULL ? strlen(item) : (size_t)(end - item);
    while (length != 0U && isspace((unsigned char)*item)) {
      ++item;
      --length;
    }
    while (length != 0U && isspace((unsigned char)item[length - 1U])) --length;
    if (length != 0U && strncmp(path, item, length) == 0 &&
        (path[length] == '\0' || path[length] == '/' ||
         item[length - 1U] == '/'))
      return true;
    if (end == NULL) break;
    item = end + 1U;
  }
  return false;
}

void laghu_asset_policy_init(laghu_asset_policy *policy) {
  if (policy == NULL) return;
  memset(policy, 0, sizeof(*policy));
  policy->preserve_query = true;
  policy->trusted_origin_fallback = true;
  policy->max_body_bytes = LAGHU_ASSET_MAX_BODY_BYTES;
  policy->retry_limit = LAGHU_ASSET_DEFAULT_RETRIES;
  policy->timeout_seconds = LAGHU_ASSET_DEFAULT_TIMEOUT;
  policy->stale_ttl_seconds = LAGHU_ASSET_DEFAULT_STALE_TTL;
}

bool laghu_asset_policy_validate(const laghu_asset_policy *policy, char *error,
                                 size_t error_size) {
  if (error != NULL && error_size > 0U) error[0] = '\0';
  if (policy == NULL || !laghu_asset_https_origin(policy->source_domain) ||
      !laghu_asset_https_origin(policy->public_domain) ||
      policy->max_body_bytes == 0U ||
      policy->max_body_bytes > LAGHU_ASSET_MAX_BODY_BYTES ||
      policy->retry_limit > 10U || policy->timeout_seconds == 0U ||
      policy->timeout_seconds > 300U || policy->stale_ttl_seconds < 60U ||
      policy->shard_count > LAGHU_ASSET_MAX_SHARDS) {
    if (error != NULL && error_size > 0U)
      (void)snprintf(error, error_size, "invalid asset domain policy");
    return false;
  }
  if (strchr(policy->source_domain, '@') != NULL ||
      strchr(policy->public_domain, '@') != NULL ||
      strchr(policy->source_domain, '#') != NULL ||
      strchr(policy->public_domain, '#') != NULL) {
    if (error != NULL && error_size > 0U)
      (void)snprintf(error, error_size,
                     "asset domains cannot contain credentials");
    return false;
  }
  if ((policy->source_prefix[0] != '\0' &&
       (policy->source_prefix[0] != '/' ||
        strchr(policy->source_prefix, '?') != NULL ||
        strchr(policy->source_prefix, '#') != NULL)) ||
      (policy->public_prefix[0] != '\0' &&
       (policy->public_prefix[0] != '/' ||
        strchr(policy->public_prefix, '?') != NULL ||
        strchr(policy->public_prefix, '#') != NULL ||
        strstr(policy->public_prefix, "..") != NULL ||
        strpbrk(policy->public_prefix, " \t\r\n") != NULL))) {
    if (error != NULL && error_size > 0U)
      (void)snprintf(error, error_size, "invalid asset path prefix");
    return false;
  }
  {
    unsigned int index;
    for (index = 0U; index < policy->shard_count; ++index)
      if (!laghu_asset_https_origin(policy->shards[index])) {
        laghu_asset_error(error, error_size, "invalid asset shard domain");
        return false;
      }
  }
  return true;
}

static bool laghu_asset_source_match(const laghu_asset_policy *policy,
                                     const char *source_url,
                                     const char **suffix) {
  size_t domain_length;
  size_t prefix_length;
  if (policy == NULL || source_url == NULL || suffix == NULL) return false;
  domain_length = strlen(policy->source_domain);
  prefix_length = strlen(policy->source_prefix);
  if (strncmp(source_url, policy->source_domain, domain_length) != 0)
    return false;
  if (prefix_length != 0U && strncmp(source_url + domain_length,
                                     policy->source_prefix, prefix_length) != 0)
    return false;
  if (prefix_length != 0U && policy->source_prefix[prefix_length - 1U] != '/' &&
      source_url[domain_length + prefix_length] != '/' &&
      source_url[domain_length + prefix_length] != '?' &&
      source_url[domain_length + prefix_length] != '#')
    return false;
  *suffix = source_url + domain_length + prefix_length;
  if (!laghu_asset_path_list(policy->allow_paths, *suffix, true) ||
      laghu_asset_path_list(policy->deny_paths, *suffix, false))
    return false;
  return **suffix == '/' || **suffix == '?' || **suffix == '#';
}

bool laghu_asset_source_allowed(const laghu_asset_policy *policy,
                                const char *source_url,
                                const char *content_type, size_t body_length) {
  const char *suffix;
  return policy != NULL && content_type != NULL && body_length != 0U &&
         body_length <= policy->max_body_bytes &&
         laghu_mime_type_allowed(policy->mime_types, content_type) &&
         laghu_asset_source_match(policy, source_url, &suffix);
}

bool laghu_asset_object_key(const laghu_asset_policy *policy,
                            const char *source_url, const char *content_hash,
                            char output[LAGHU_RUNTIME_PATH_SIZE]) {
  const char *suffix;
  const char *end;
  size_t base_length;
  int written;
  if (output == NULL || content_hash == NULL ||
      strlen(content_hash) != LAGHU_SHA256_HEX_LENGTH ||
      !laghu_asset_policy_validate(policy, NULL, 0U) ||
      !laghu_asset_source_match(policy, source_url, &suffix))
    return false;
  end = strpbrk(suffix, "?#");
  base_length = end == NULL ? strlen(suffix) : (size_t)(end - suffix);
  if (base_length == 0U || suffix[0] != '/') return false;
  written =
      snprintf(output, LAGHU_RUNTIME_PATH_SIZE, "%s/%s%.*s",
               policy->public_prefix, content_hash, (int)base_length, suffix);
  if (written <= 0 || (size_t)written >= LAGHU_RUNTIME_PATH_SIZE) return false;
  {
    size_t index;
    for (index = strlen(policy->public_prefix) + 1U + LAGHU_SHA256_HEX_LENGTH;
         output[index] != '\0'; ++index)
      if (!(isalnum((unsigned char)output[index]) || output[index] == '/' ||
            output[index] == '-' || output[index] == '_' ||
            output[index] == '.' || output[index] == '~'))
        output[index] = '_';
  }
  return true;
}

bool laghu_asset_url_rewrite(const laghu_asset_policy *policy,
                             const char *source_url, const char *content_type,
                             const char *content_hash, char *output,
                             size_t output_size) {
  const char *suffix;
  const char *tail;
  char object_key[LAGHU_RUNTIME_PATH_SIZE];
  int written;
  if (output == NULL || output_size == 0U || content_type == NULL ||
      !laghu_asset_policy_validate(policy, NULL, 0U) ||
      !laghu_mime_type_allowed(policy == NULL ? NULL : policy->mime_types,
                               content_type) ||
      !laghu_asset_source_match(policy, source_url, &suffix) ||
      !laghu_asset_object_key(policy, source_url, content_hash, object_key))
    return false;
  tail = strpbrk(suffix, "?#");
  if (tail != NULL && !policy->preserve_query && tail[0] == '?') {
    const char *fragment = strchr(tail, '#');
    tail = fragment == NULL ? "" : fragment;
  }
  {
    const char *public_domain = policy->public_domain;
    if (policy->shard_count != 0U) {
      char hash[LAGHU_RUNTIME_KEY_SIZE];
      char shard_hash[9];
      if (!laghu_sha256_hex((laghu_buffer){(const unsigned char *)source_url,
                                           strlen(source_url)},
                            hash))
        return false;
      memcpy(shard_hash, hash, 8U);
      shard_hash[8] = '\0';
      public_domain =
          policy->shards[(unsigned int)strtoul(shard_hash, NULL, 16) %
                         policy->shard_count];
    }
    written = snprintf(output, output_size, "%s%s%s", public_domain, object_key,
                       tail == NULL ? "" : tail);
  }
  return written > 0 && (size_t)written < output_size;
}

bool laghu_asset_record_rewrite(const laghu_asset_policy *policy,
                                const laghu_asset_record *record, char *output,
                                size_t output_size) {
  char expected[LAGHU_RUNTIME_PATH_SIZE];
  if (record == NULL || record->state != LAGHU_ASSET_READY ||
      record->body_length == 0U ||
      record->body_length > policy->max_body_bytes ||
      !laghu_asset_object_key(policy, record->source_url, record->content_hash,
                              expected) ||
      strcmp(expected, record->object_key) != 0)
    return false;
  return laghu_asset_url_rewrite(policy, record->source_url,
                                 record->content_type, record->content_hash,
                                 output, output_size);
}

void laghu_asset_config_init(laghu_asset_config *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->version = LAGHU_ASSET_CONFIG_VERSION;
  laghu_asset_policy_init(&config->policy);
  (void)laghu_asset_copy(config->provider, sizeof(config->provider), "s3");
}

static bool laghu_asset_parse_bool(const char *value, bool *output) {
  if (strcmp(value, "on") == 0 || strcmp(value, "true") == 0)
    *output = true;
  else if (strcmp(value, "off") == 0 || strcmp(value, "false") == 0)
    *output = false;
  else
    return false;
  return true;
}

static bool laghu_asset_config_value(laghu_asset_config *config,
                                     const char *name, const char *value) {
  if (strcmp(name, "version") == 0) {
    char *end = NULL;
    unsigned long version = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || version != LAGHU_ASSET_CONFIG_VERSION)
      return false;
    config->version = (unsigned int)version;
    return true;
  }
#define COPY_FIELD(key, field) \
  if (strcmp(name, key) == 0)  \
  return laghu_asset_copy(field, sizeof(field), value)
  COPY_FIELD("source_domain", config->policy.source_domain);
  COPY_FIELD("public_domain", config->policy.public_domain);
  COPY_FIELD("source_prefix", config->policy.source_prefix);
  COPY_FIELD("public_prefix", config->policy.public_prefix);
  COPY_FIELD("mime_types", config->policy.mime_types);
  COPY_FIELD("allow_paths", config->policy.allow_paths);
  COPY_FIELD("deny_paths", config->policy.deny_paths);
  COPY_FIELD("catalog_path", config->catalog_path);
  COPY_FIELD("queue_path", config->queue_path);
  COPY_FIELD("operational_cache_path", config->operational_cache_path);
  COPY_FIELD("provider", config->provider);
  COPY_FIELD("endpoint", config->endpoint);
  COPY_FIELD("region", config->region);
  COPY_FIELD("bucket", config->bucket);
  COPY_FIELD("object_prefix", config->object_prefix);
  COPY_FIELD("access_key_env", config->access_key_env);
  COPY_FIELD("secret_key_env", config->secret_key_env);
#undef COPY_FIELD
  if (strncmp(name, "shard", 5U) == 0 &&
      config->policy.shard_count < LAGHU_ASSET_MAX_SHARDS)
    return laghu_asset_copy(config->policy.shards[config->policy.shard_count++],
                            LAGHU_RUNTIME_PATH_SIZE, value);
  if (strcmp(name, "mode") == 0) {
    if (strcmp(value, "rewrite_only") == 0)
      config->policy.upload = false;
    else if (strcmp(value, "upload_and_rewrite") == 0)
      config->policy.upload = true;
    else
      return false;
    return true;
  }
  if (strcmp(name, "preserve_query") == 0)
    return laghu_asset_parse_bool(value, &config->policy.preserve_query);
  if (strcmp(name, "trusted_origin_fallback") == 0)
    return laghu_asset_parse_bool(value,
                                  &config->policy.trusted_origin_fallback);
  if (strcmp(name, "max_body_bytes") == 0) {
    config->policy.max_body_bytes = (size_t)strtoull(value, NULL, 10);
    return config->policy.max_body_bytes != 0U;
  }
  if (strcmp(name, "retry_limit") == 0) {
    config->policy.retry_limit = (unsigned int)strtoul(value, NULL, 10);
    return true;
  }
  if (strcmp(name, "timeout_seconds") == 0) {
    config->policy.timeout_seconds = (unsigned int)strtoul(value, NULL, 10);
    return true;
  }
  if (strcmp(name, "stale_ttl_seconds") == 0) {
    config->policy.stale_ttl_seconds = (unsigned int)strtoul(value, NULL, 10);
    return true;
  }
  return false;
}

bool laghu_asset_config_load(const char *path, laghu_asset_config *config,
                             char *error, size_t error_size) {
  FILE *file;
  char line[4096];
  char material[16384];
  char names[LAGHU_ASSET_MAX_RULES][64];
  unsigned int name_count = 0U;
  size_t used = 0U;
  unsigned int line_number = 0U;
  bool version_seen = false;
  laghu_asset_config parsed;
  if (path == NULL || config == NULL || (file = fopen(path, "rb")) == NULL) {
    laghu_asset_error(error, error_size, "cannot open asset configuration");
    return false;
  }
  laghu_asset_config_init(&parsed);
  while (fgets(line, sizeof(line), file) != NULL) {
    char *equals, *name, *value;
    size_t length;
    ++line_number;
    name = laghu_asset_trim(line);
    if (*name == '\0' || *name == '#') continue;
    equals = strchr(name, '=');
    if (equals == NULL) goto invalid;
    *equals = '\0';
    value = laghu_asset_trim(equals + 1U);
    name = laghu_asset_trim(name);
    if (strncmp(name, "shard", 5U) != 0) {
      unsigned int index;
      for (index = 0U; index < name_count; ++index)
        if (strcmp(names[index], name) == 0) goto invalid;
      if (name_count >= LAGHU_ASSET_MAX_RULES ||
          !laghu_asset_copy(names[name_count], sizeof(names[name_count]), name))
        goto invalid;
      ++name_count;
    }
    if (strcmp(name, "version") == 0) version_seen = true;
    if (strstr(name, "secret") != NULL && strcmp(name, "secret_key_env") != 0)
      goto invalid;
    if (!laghu_asset_config_value(&parsed, name, value)) goto invalid;
    length = strlen(name) + strlen(value) + 2U;
    if (used + length >= sizeof(material)) goto invalid;
    used += (size_t)snprintf(material + used, sizeof(material) - used,
                             "%s=%s\n", name, value);
  }
  (void)fclose(file);
  if (!version_seen || strcmp(parsed.provider, "s3") != 0 ||
      !laghu_asset_policy_validate(&parsed.policy, error, error_size) ||
      !laghu_asset_safe_path(parsed.catalog_path) ||
      !laghu_asset_safe_path(parsed.queue_path) ||
      (parsed.operational_cache_path[0] != '\0' &&
       !laghu_asset_safe_path(parsed.operational_cache_path)) ||
      !laghu_asset_https_origin(parsed.endpoint) || parsed.region[0] == '\0' ||
      parsed.bucket[0] == '\0' || parsed.access_key_env[0] == '\0' ||
      parsed.secret_key_env[0] == '\0' ||
      (parsed.object_prefix[0] != '\0' &&
       (parsed.object_prefix[0] == '/' ||
        strstr(parsed.object_prefix, "..") != NULL ||
        strpbrk(parsed.object_prefix, " \t\r\n?#") != NULL)) ||
      !laghu_sha256_hex((laghu_buffer){(const unsigned char *)material, used},
                        parsed.digest)) {
    if (error != NULL && error[0] == '\0')
      laghu_asset_error(error, error_size, "incomplete asset configuration");
    return false;
  }
  *config = parsed;
  return true;
invalid:
  (void)fclose(file);
  if (error != NULL && error_size != 0U)
    (void)snprintf(error, error_size, "invalid asset configuration line %u",
                   line_number);
  return false;
}

bool laghu_asset_catalog_key(const laghu_asset_record *record,
                             char output[LAGHU_RUNTIME_KEY_SIZE]) {
  char material[4096];
  int written;
  if (record == NULL || output == NULL ||
      !laghu_asset_hash_valid(record->content_hash))
    return false;
  written = snprintf(material, sizeof(material), "%s\n%s\n%s\n%s\n%s",
                     record->source_url, record->source_validator,
                     record->content_hash, record->policy_digest,
                     record->provider_digest);
  return written > 0 && (size_t)written < sizeof(material) &&
         laghu_sha256_hex(
             (laghu_buffer){(const unsigned char *)material, (size_t)written},
             output);
}

static bool laghu_asset_file_paths(const char *directory, const char *key,
                                   const char *extension, char *path,
                                   char *temporary) {
  int a, b;
  if (!laghu_asset_safe_path(directory) || !laghu_asset_hash_valid(key))
    return false;
  a = snprintf(path, LAGHU_RUNTIME_PATH_SIZE, "%s/%s.%s", directory, key,
               extension);
  b = snprintf(temporary, LAGHU_RUNTIME_PATH_SIZE, "%s/.%s.%lu.tmp", directory,
               key, (unsigned long)laghu_asset_pid());
  return a > 0 && b > 0 && (size_t)a < LAGHU_RUNTIME_PATH_SIZE &&
         (size_t)b < LAGHU_RUNTIME_PATH_SIZE;
}

bool laghu_asset_catalog_publish(const char *catalog_path,
                                 const laghu_asset_record *record) {
  laghu_asset_catalog_file stored = {0};
  char key[LAGHU_RUNTIME_KEY_SIZE], path[LAGHU_RUNTIME_PATH_SIZE];
  char temporary[LAGHU_RUNTIME_PATH_SIZE];
  FILE *file;
  if (record == NULL || record->state > LAGHU_ASSET_STALE ||
      !laghu_asset_catalog_key(record, key) ||
      !laghu_asset_directory(catalog_path) ||
      !laghu_asset_file_paths(catalog_path, key, "asset", path, temporary))
    return false;
  file = fopen(path, "rb");
  if (file != NULL)
    (void)fclose(file);
  else if (!laghu_asset_directory_has_capacity(catalog_path,
                                               LAGHU_ASSET_MAX_RECORDS * 2U))
    return false;
  stored.version = LAGHU_ASSET_CONFIG_VERSION;
  stored.record = *record;
  if (!laghu_sha256_hex((laghu_buffer){(const unsigned char *)&stored.record,
                                       sizeof(stored.record)},
                        stored.checksum))
    return false;
  file = fopen(temporary, "wb");
  if (file == NULL) return false;
  if (fwrite(&stored, sizeof(stored), 1U, file) != 1U || fflush(file) != 0 ||
      fclose(file) != 0 || !laghu_asset_replace(temporary, path)) {
    (void)laghu_asset_unlink(temporary);
    return false;
  }
  {
    char identity_material[3072], identity[LAGHU_RUNTIME_KEY_SIZE];
    char source_identity[LAGHU_RUNTIME_PATH_SIZE];
    char index_path[LAGHU_RUNTIME_PATH_SIZE],
        index_temporary[LAGHU_RUNTIME_PATH_SIZE];
    FILE *index;
    int length;
    if (!laghu_asset_source_identity(record->source_url, source_identity,
                                     sizeof(source_identity)))
      return false;
    length = snprintf(identity_material, sizeof(identity_material),
                      "%s\n%s\n%s", source_identity, record->policy_digest,
                      record->provider_digest);
    if (length <= 0 || (size_t)length >= sizeof(identity_material) ||
        !laghu_sha256_hex(
            (laghu_buffer){(const unsigned char *)identity_material,
                           (size_t)length},
            identity) ||
        !laghu_asset_file_paths(catalog_path, identity, "asset-index",
                                index_path, index_temporary) ||
        (index = fopen(index_temporary, "wb")) == NULL ||
        fwrite(key, sizeof(key), 1U, index) != 1U || fflush(index) != 0 ||
        fclose(index) != 0 ||
        !laghu_asset_replace(index_temporary, index_path)) {
      (void)laghu_asset_unlink(index_temporary);
      return false;
    }
  }
  return true;
}

bool laghu_asset_catalog_lookup(const char *catalog_path, const char *key,
                                laghu_asset_record *record) {
  laghu_asset_catalog_file stored;
  char path[LAGHU_RUNTIME_PATH_SIZE], temporary[LAGHU_RUNTIME_PATH_SIZE];
  char checksum[LAGHU_RUNTIME_KEY_SIZE];
  FILE *file;
  if (record == NULL ||
      !laghu_asset_file_paths(catalog_path, key, "asset", path, temporary) ||
      (file = fopen(path, "rb")) == NULL)
    return false;
  if (fread(&stored, sizeof(stored), 1U, file) != 1U || fgetc(file) != EOF ||
      fclose(file) != 0 || stored.version != LAGHU_ASSET_CONFIG_VERSION ||
      !laghu_sha256_hex((laghu_buffer){(const unsigned char *)&stored.record,
                                       sizeof(stored.record)},
                        checksum) ||
      strcmp(checksum, stored.checksum) != 0)
    return false;
  *record = stored.record;
  return true;
}

bool laghu_asset_catalog_lookup_url(const laghu_asset_config *config,
                                    const char *source_url,
                                    laghu_asset_record *record) {
  char material[3072], identity[LAGHU_RUNTIME_KEY_SIZE];
  char source_identity[LAGHU_RUNTIME_PATH_SIZE];
  char path[LAGHU_RUNTIME_PATH_SIZE], temporary[LAGHU_RUNTIME_PATH_SIZE];
  char key[LAGHU_RUNTIME_KEY_SIZE];
  FILE *file;
  int length;
  if (config == NULL || source_url == NULL || record == NULL) return false;
  if (!laghu_asset_source_identity(source_url, source_identity,
                                   sizeof(source_identity)))
    return false;
  length = snprintf(material, sizeof(material), "%s\n%s\n%s", source_identity,
                    config->digest, config->digest);
  if (length <= 0 || (size_t)length >= sizeof(material) ||
      !laghu_sha256_hex(
          (laghu_buffer){(const unsigned char *)material, (size_t)length},
          identity) ||
      !laghu_asset_file_paths(config->catalog_path, identity, "asset-index",
                              path, temporary) ||
      (file = fopen(path, "rb")) == NULL ||
      fread(key, sizeof(key), 1U, file) != 1U || fgetc(file) != EOF ||
      fclose(file) != 0 || !laghu_asset_hash_valid(key))
    return false;
  return laghu_asset_catalog_lookup(config->catalog_path, key, record);
}

static bool laghu_asset_url_end(unsigned char value) {
  return value == '\0' || isspace(value) || value == '\'' || value == '"' ||
         value == '<' || value == '>' || value == ')' || value == ']';
}

bool laghu_asset_rewrite_document_at(const laghu_asset_config *config,
                                     laghu_buffer input, const char *page_path,
                                     unsigned char **output,
                                     size_t *output_length) {
  size_t index = 0U, copied = 0U, capacity;
  unsigned char *result;
  bool changed = false;
  size_t prefix_length, domain_length;
  if (output == NULL || output_length == NULL) return false;
  *output = NULL;
  *output_length = 0U;
  if (config == NULL || input.data == NULL || input.length == 0U ||
      !laghu_asset_policy_validate(&config->policy, NULL, 0U))
    return true;
  domain_length = strlen(config->policy.source_domain);
  prefix_length = domain_length + strlen(config->policy.source_prefix);
  capacity = input.length + 1U;
  result = malloc(capacity);
  if (result == NULL) return false;
  while (index < input.length) {
    bool absolute =
        index + prefix_length <= input.length &&
        memcmp(input.data + index, config->policy.source_domain,
               domain_length) == 0 &&
        memcmp(input.data + index + domain_length, config->policy.source_prefix,
               strlen(config->policy.source_prefix)) == 0;
    bool delimited =
        index != 0U &&
        (input.data[index - 1U] == '\'' || input.data[index - 1U] == '"' ||
         input.data[index - 1U] == '(' || input.data[index - 1U] == '=');
    bool root_relative = delimited && input.data[index] == '/' &&
                         index + 1U < input.length &&
                         input.data[index + 1U] != '/';
    bool document_relative =
        delimited && page_path != NULL && input.data[index] != '/' &&
        input.data[index] != '#' && input.data[index] != '?' &&
        (isalnum(input.data[index]) || input.data[index] == '.') &&
        !(index + 5U < input.length &&
          (memcmp(input.data + index, "data:", 5U) == 0 ||
           memcmp(input.data + index, "blob:", 5U) == 0));
    if (absolute || root_relative || document_relative) {
      size_t end = absolute ? index + prefix_length : index;
      char source[LAGHU_RUNTIME_PATH_SIZE], rewritten[LAGHU_RUNTIME_PATH_SIZE];
      laghu_asset_record record;
      while (end < input.length && !laghu_asset_url_end(input.data[end])) ++end;
      if (absolute && end - index < sizeof(source)) {
        memcpy(source, input.data + index, end - index);
        source[end - index] = '\0';
      } else if (root_relative &&
                 domain_length + end - index < sizeof(source)) {
        (void)snprintf(source, sizeof(source), "%s%.*s",
                       config->policy.source_domain, (int)(end - index),
                       input.data + index);
      } else if (document_relative &&
                 !(end - index >= 2U && input.data[index] == '.' &&
                   input.data[index + 1U] == '.')) {
        const char *slash = strrchr(page_path, '/');
        size_t directory_length =
            slash == NULL ? 1U : (size_t)(slash - page_path) + 1U;
        const unsigned char *relative = input.data + index;
        size_t relative_length = end - index;
        if (relative_length >= 2U && relative[0] == '.' && relative[1] == '/') {
          relative += 2U;
          relative_length -= 2U;
        }
        if (domain_length + directory_length + relative_length < sizeof(source))
          (void)snprintf(source, sizeof(source), "%s%.*s%.*s",
                         config->policy.source_domain, (int)directory_length,
                         page_path, (int)relative_length, relative);
        else
          source[0] = '\0';
      } else {
        source[0] = '\0';
      }
      if (source[0] != '\0') {
        bool found = laghu_asset_catalog_lookup_url(config, source, &record);
        uint64_t now = (uint64_t)time(NULL);
        bool stale = found && record.state == LAGHU_ASSET_READY &&
                     record.updated_at <= now &&
                     now - record.updated_at > config->policy.stale_ttl_seconds;
        if (stale) {
          record.state = LAGHU_ASSET_STALE;
          record.updated_at = now;
          (void)laghu_asset_catalog_publish(config->catalog_path, &record);
        }
        if (found && !stale &&
            laghu_asset_record_rewrite(&config->policy, &record, rewritten,
                                       sizeof(rewritten)) &&
            laghu_asset_url_rewrite(&config->policy, source,
                                    record.content_type, record.content_hash,
                                    rewritten, sizeof(rewritten))) {
          size_t replacement = strlen(rewritten);
          if (copied + replacement + input.length - end + 1U > capacity) {
            size_t next = copied + replacement + input.length - end + 1U;
            unsigned char *grown = realloc(result, next);
            if (grown == NULL) {
              free(result);
              return false;
            }
            result = grown;
            capacity = next;
          }
          memcpy(result + copied, rewritten, replacement);
          copied += replacement;
          index = end;
          changed = true;
          continue;
        }
        if (config->policy.trusted_origin_fallback) {
          const char *ignored;
          laghu_asset_record pending = {0};
          if (laghu_asset_source_match(&config->policy, source, &ignored) &&
              laghu_asset_copy(pending.source_url, sizeof(pending.source_url),
                               source) &&
              laghu_asset_copy(pending.policy_digest,
                               sizeof(pending.policy_digest), config->digest) &&
              laghu_asset_copy(pending.provider_digest,
                               sizeof(pending.provider_digest),
                               config->digest)) {
            pending.state = LAGHU_ASSET_PENDING;
            pending.updated_at = (uint64_t)time(NULL);
            (void)laghu_asset_job_publish(config, &pending,
                                          (laghu_buffer){NULL, 0U});
          }
        }
      }
    }
    result[copied++] = input.data[index++];
  }
  if (!changed) {
    free(result);
    return true;
  }
  result[copied] = '\0';
  *output = result;
  *output_length = copied;
  return true;
}

bool laghu_asset_rewrite_document(const laghu_asset_config *config,
                                  laghu_buffer input, unsigned char **output,
                                  size_t *output_length) {
  return laghu_asset_rewrite_document_at(config, input, "/", output,
                                         output_length);
}

bool laghu_asset_job_publish(const laghu_asset_config *config,
                             const laghu_asset_record *record,
                             laghu_buffer body) {
  laghu_asset_job_file header = {0};
  char key[LAGHU_RUNTIME_KEY_SIZE], path[LAGHU_RUNTIME_PATH_SIZE];
  char temporary[LAGHU_RUNTIME_PATH_SIZE];
  FILE *file;
  if (config == NULL || record == NULL ||
      (body.length != 0U && body.data == NULL) ||
      body.length > config->policy.max_body_bytes ||
      ((laghu_asset_hash_valid(record->content_hash) &&
        !laghu_asset_catalog_key(record, key)) ||
       (!laghu_asset_hash_valid(record->content_hash) &&
        !laghu_sha256_hex(
            (laghu_buffer){(const unsigned char *)record->source_url,
                           strlen(record->source_url)},
            key))) ||
      !laghu_asset_directory(config->queue_path) ||
      !laghu_asset_file_paths(config->queue_path, key, "job", path, temporary))
    return false;
  file = fopen(path, "rb");
  if (file != NULL) {
    (void)fclose(file);
    return true;
  }
  if (!laghu_asset_directory_has_capacity(config->queue_path,
                                          LAGHU_ASSET_MAX_JOBS))
    return false;
  header.version = LAGHU_ASSET_CONFIG_VERSION;
  header.record = *record;
  header.body_length = body.length;
  if (!laghu_sha256_hex(body, header.body_hash) ||
      (file = fopen(temporary, "wb")) == NULL)
    return false;
  if (fwrite(&header, sizeof(header), 1U, file) != 1U ||
      (body.length != 0U && fwrite(body.data, body.length, 1U, file) != 1U) ||
      fflush(file) != 0 || fclose(file) != 0 ||
      !laghu_asset_replace(temporary, path)) {
    (void)laghu_asset_unlink(temporary);
    return false;
  }
  return true;
}

bool laghu_asset_job_take(const laghu_asset_config *config,
                          laghu_asset_record *record, unsigned char **body,
                          size_t *body_length,
                          char job_path[LAGHU_RUNTIME_PATH_SIZE]) {
  if (config == NULL || record == NULL || body == NULL || body_length == NULL ||
      job_path == NULL)
    return false;
  *body = NULL;
  *body_length = 0U;
#ifdef _WIN32
  WIN32_FIND_DATAA found;
  HANDLE search;
  char pattern[LAGHU_RUNTIME_PATH_SIZE];
  const char *name = NULL;
  (void)snprintf(pattern, sizeof(pattern), "%s/*.job", config->queue_path);
  search = FindFirstFileA(pattern, &found);
  if (search == INVALID_HANDLE_VALUE) return false;
  name = found.cFileName;
#else
  DIR *directory;
  struct dirent *entry;
  const char *name = NULL;
  directory = opendir(config->queue_path);
  if (directory == NULL) return false;
  while ((entry = readdir(directory)) != NULL) {
    size_t length = strlen(entry->d_name);
    if (length > 4U && strcmp(entry->d_name + length - 4U, ".job") == 0) {
      name = entry->d_name;
      break;
    }
  }
  if (name == NULL) {
    (void)closedir(directory);
    return false;
  }
#endif
  {
    char source[LAGHU_RUNTIME_PATH_SIZE];
    laghu_asset_job_file header;
    char actual[LAGHU_RUNTIME_KEY_SIZE];
    FILE *file;
    int a = snprintf(source, sizeof(source), "%s/%s", config->queue_path, name);
    int b = snprintf(job_path, LAGHU_RUNTIME_PATH_SIZE, "%s/%s.work",
                     config->queue_path, name);
#ifndef _WIN32
    (void)closedir(directory);
#else
    (void)FindClose(search);
#endif
    if (a <= 0 || b <= 0 || (size_t)a >= sizeof(source) ||
        (size_t)b >= LAGHU_RUNTIME_PATH_SIZE ||
        !laghu_asset_replace(source, job_path) ||
        (file = fopen(job_path, "rb")) == NULL)
      return false;
    if (fread(&header, sizeof(header), 1U, file) != 1U ||
        header.version != LAGHU_ASSET_CONFIG_VERSION ||
        header.body_length > config->policy.max_body_bytes ||
        (header.body_length != 0U &&
         ((*body = malloc(header.body_length)) == NULL ||
          fread(*body, header.body_length, 1U, file) != 1U)) ||
        fgetc(file) != EOF || fclose(file) != 0 ||
        !laghu_sha256_hex((laghu_buffer){*body, header.body_length}, actual) ||
        strcmp(actual, header.body_hash) != 0) {
      free(*body);
      *body = NULL;
      (void)laghu_asset_unlink(job_path);
      return false;
    }
    if (header.record.retry_after > (uint64_t)time(NULL)) {
      free(*body);
      *body = NULL;
      (void)laghu_asset_replace(job_path, source);
      return false;
    }
    *record = header.record;
    *body_length = header.body_length;
  }
  return true;
}

bool laghu_asset_job_complete(const char *job_path) {
  return laghu_asset_safe_path(job_path) && laghu_asset_unlink(job_path) == 0;
}

uint64_t laghu_asset_retry_after(const laghu_asset_policy *policy,
                                 unsigned int attempts, uint64_t now) {
  uint64_t delay = 1U;
  unsigned int index;
  if (policy == NULL || attempts > policy->retry_limit) return UINT64_MAX;
  for (index = 0U; index < attempts && delay < 3600U; ++index) delay *= 2U;
  return now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
}
