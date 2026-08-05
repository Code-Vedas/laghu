// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "laghu/config.h"
#include "mod_laghu_internal.h"

#define LAGHU_CACHE_SET_SIZE (1U << 0U)
#define LAGHU_CACHE_SET_INODES (1U << 1U)
#define LAGHU_CACHE_SET_CLEAN (1U << 2U)
#define LAGHU_CACHE_SET_METADATA (1U << 3U)
#define LAGHU_ADMIN_SET_METHOD (1U << 0U)
#define LAGHU_ADMIN_SET_QUERY (1U << 1U)
#define LAGHU_ADMIN_SET_STATS (1U << 2U)
#define LAGHU_ADMIN_SET_TOKEN (1U << 3U)
#define LAGHU_ADMIN_SET_FLUSH (1U << 4U)
#define LAGHU_ADMIN_SET_METRICS (1U << 5U)
#define LAGHU_ADMIN_SET_READINESS (1U << 6U)
#define LAGHU_ADMIN_SET_READINESS_POLICY (1U << 7U)

static apr_status_t laghu_apache_queue_cleanup(void *data) {
  laghu_apache_config *config = data;
  laghu_runtime_queue_close(&config->queue);
  laghu_runtime_queue_close(&config->font_queue);
  laghu_runtime_queue_close(&config->javascript_runtime_queue);
  return APR_SUCCESS;
}

void *laghu_apache_create_config(apr_pool_t *pool, char *path) {
  laghu_apache_config *config = apr_pcalloc(pool, sizeof(*config));
  (void)path;
  if (config != NULL) {
    laghu_config_init(&config->core);
    laghu_runtime_queue_init(&config->queue);
    laghu_runtime_queue_init(&config->font_queue);
    laghu_runtime_queue_init(&config->javascript_runtime_queue);
    config->rum_store = "local:";
    config->rum_memory_limit = LAGHU_RUM_DEFAULT_MEMORY_BYTES;
    config->rum_pending_limit = LAGHU_RUM_DEFAULT_PENDING_BYTES;
    config->rum_ttl = LAGHU_IMAGE_METADATA_TTL_DEFAULT;
    config->rum_sync_interval = LAGHU_RUM_DEFAULT_SYNC_SECONDS;
    config->rum_timeout_ms = LAGHU_RUM_DEFAULT_TIMEOUT_MS;
    config->rum_retry_limit = LAGHU_RUM_DEFAULT_RETRY_LIMIT;
    laghu_cache_limits_init(&config->cache_limits);
    laghu_source_policy_init(&config->source_policy);
    apr_pool_cleanup_register(pool, config, laghu_apache_queue_cleanup,
                              apr_pool_cleanup_null);
  }
  return config;
}

void *laghu_apache_create_server_config(apr_pool_t *pool, server_rec *server) {
  (void)server;
  return laghu_apache_create_config(pool, NULL);
}

void *laghu_apache_merge_config(apr_pool_t *pool, void *parent_value,
                                void *child_value) {
  laghu_apache_config *parent = parent_value;
  laghu_apache_config *child = child_value;
  laghu_apache_config *merged = apr_pcalloc(pool, sizeof(*merged));
  if (merged == NULL || parent == NULL || child == NULL) {
    return NULL;
  }
  if (!laghu_resource_rules_merge_valid(&parent->core, &child->core))
    return NULL;
  laghu_config_merge(&merged->core, &parent->core, &child->core);
  merged->worker_queue =
      child->worker_queue != NULL ? child->worker_queue : parent->worker_queue;
  merged->asset_offload_config = child->asset_offload_config != NULL
                                     ? child->asset_offload_config
                                     : parent->asset_offload_config;
  merged->asset_upload_queue = child->asset_upload_queue != NULL
                                   ? child->asset_upload_queue
                                   : parent->asset_upload_queue;
  if (child->asset_offload_loaded) {
    merged->asset_offload = child->asset_offload;
    merged->asset_offload_loaded = true;
  } else if (parent->asset_offload_loaded) {
    merged->asset_offload = parent->asset_offload;
    merged->asset_offload_loaded = true;
  }
  if (!laghu_source_policy_merge(&merged->source_policy, &parent->source_policy,
                                 &child->source_policy))
    return NULL;
  merged->source_mode_set = parent->source_mode_set || child->source_mode_set;
  if (child->source_mode_set)
    merged->source_policy.mode = child->source_policy.mode;
  if (merged->source_policy.mode == LAGHU_SOURCE_FILE_OFF) {
    merged->source_policy.mapping_count = 0U;
    merged->source_policy.native_root[0] = '\0';
  }
  if (child->file_cache_backend != NULL || child->image_cache_set) {
    merged->file_cache_backend = child->file_cache_backend;
    merged->image_cache = child->image_cache;
    merged->image_cache_set = child->image_cache_set;
  } else {
    merged->file_cache_backend = parent->file_cache_backend;
    merged->image_cache = parent->image_cache;
    merged->image_cache_set = parent->image_cache_set;
  }
  merged->cache_limits = parent->cache_limits;
  if ((child->cache_set_mask & LAGHU_CACHE_SET_SIZE) != 0U)
    merged->cache_limits.size_limit = child->cache_limits.size_limit;
  if ((child->cache_set_mask & LAGHU_CACHE_SET_INODES) != 0U)
    merged->cache_limits.inode_limit = child->cache_limits.inode_limit;
  if ((child->cache_set_mask & LAGHU_CACHE_SET_CLEAN) != 0U)
    merged->cache_limits.clean_interval = child->cache_limits.clean_interval;
  if ((child->cache_set_mask & LAGHU_CACHE_SET_METADATA) != 0U)
    merged->cache_limits.metadata_size = child->cache_limits.metadata_size;
  merged->cache_set_mask = parent->cache_set_mask | child->cache_set_mask;
  merged->purge_method = (child->admin_set_mask & LAGHU_ADMIN_SET_METHOD) != 0U
                             ? child->purge_method
                             : parent->purge_method;
  merged->purge_query = (child->admin_set_mask & LAGHU_ADMIN_SET_QUERY) != 0U
                            ? child->purge_query
                            : parent->purge_query;
  merged->statistics = (child->admin_set_mask & LAGHU_ADMIN_SET_STATS) != 0U
                           ? child->statistics
                           : parent->statistics;
  merged->metrics = (child->admin_set_mask & LAGHU_ADMIN_SET_METRICS) != 0U
                        ? child->metrics
                        : parent->metrics;
  merged->readiness = (child->admin_set_mask & LAGHU_ADMIN_SET_READINESS) != 0U
                          ? child->readiness
                          : parent->readiness;
  merged->readiness_strict =
      (child->admin_set_mask & LAGHU_ADMIN_SET_READINESS_POLICY) != 0U
          ? child->readiness_strict
          : parent->readiness_strict;
  merged->purge_token_file = child->purge_token_file != NULL
                                 ? child->purge_token_file
                                 : parent->purge_token_file;
  merged->cache_flush_file = child->cache_flush_file != NULL
                                 ? child->cache_flush_file
                                 : parent->cache_flush_file;
  merged->purge_allow =
      child->purge_allow != NULL ? child->purge_allow : parent->purge_allow;
  merged->trusted_proxy = child->trusted_proxy != NULL ? child->trusted_proxy
                                                       : parent->trusted_proxy;
  merged->admin_set_mask = parent->admin_set_mask | child->admin_set_mask;
  merged->font_fetch_queue = child->font_fetch_queue != NULL
                                 ? child->font_fetch_queue
                                 : parent->font_fetch_queue;
  merged->font_provider_config = child->font_provider_config != NULL
                                     ? child->font_provider_config
                                     : parent->font_provider_config;
  merged->javascript_queue = child->javascript_queue != NULL
                                 ? child->javascript_queue
                                 : parent->javascript_queue;
  merged->javascript_target = child->javascript_target != NULL
                                  ? child->javascript_target
                                  : parent->javascript_target;
  merged->javascript_observation_config =
      child->javascript_observation_config != NULL
          ? child->javascript_observation_config
          : parent->javascript_observation_config;
  merged->javascript_defer_config = child->javascript_defer_config != NULL
                                        ? child->javascript_defer_config
                                        : parent->javascript_defer_config;
  if (child->font_providers_loaded) {
    merged->font_providers = child->font_providers;
    merged->font_providers_loaded = true;
  } else if (parent->font_providers_loaded) {
    merged->font_providers = parent->font_providers;
    merged->font_providers_loaded = true;
  }
  if (child->javascript_observations_loaded) {
    merged->javascript_observations = child->javascript_observations;
    merged->javascript_observations_loaded = true;
  } else if (parent->javascript_observations_loaded) {
    merged->javascript_observations = parent->javascript_observations;
    merged->javascript_observations_loaded = true;
  }
  if (child->javascript_defer_loaded) {
    merged->javascript_defer = child->javascript_defer;
    merged->javascript_defer_loaded = true;
  } else if (parent->javascript_defer_loaded) {
    merged->javascript_defer = parent->javascript_defer;
    merged->javascript_defer_loaded = true;
  }
  laghu_runtime_queue_init(&merged->queue);
  laghu_runtime_queue_init(&merged->font_queue);
  laghu_runtime_queue_init(&merged->javascript_runtime_queue);
  apr_pool_cleanup_register(pool, merged, laghu_apache_queue_cleanup,
                            apr_pool_cleanup_null);
  return merged;
}

static bool laghu_apache_on_off(const char *value, laghu_mode *mode) {
  if (value != NULL && ap_cstr_casecmp(value, "on") == 0) {
    *mode = LAGHU_MODE_ON;
    return true;
  }
  if (value != NULL && ap_cstr_casecmp(value, "off") == 0) {
    *mode = LAGHU_MODE_OFF;
    return true;
  }
  return false;
}

static bool laghu_apache_size(const char *value, size_t minimum,
                              size_t *output) {
  unsigned long long parsed, multiplier = 1U;
  char *end = NULL;
  if (value == NULL || *value == '\0') return false;
  errno = 0;
  parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value) return false;
  if (*end != '\0') {
    if (end[1] != '\0') return false;
    if (*end == 'k' || *end == 'K')
      multiplier = 1024U;
    else if (*end == 'm' || *end == 'M')
      multiplier = 1024U * 1024U;
    else
      return false;
  }
  if (parsed > SIZE_MAX / multiplier) return false;
  parsed *= multiplier;
  if (parsed < minimum) return false;
  *output = (size_t)parsed;
  return true;
}

const char *laghu_apache_command(cmd_parms *command, void *value,
                                 const char *arguments) {
  laghu_apache_config *config = value;
  const char *cursor = arguments;
  const char *name = ap_getword_conf(command->pool, &cursor);
  const char *parameter = ap_getword_conf(command->pool, &cursor);
  const char *extra = ap_getword_conf(command->pool, &cursor);
  char *end = NULL;
  unsigned long quality;
  laghu_config_setting shared_setting;
  char shared_error[160U];

  if (ap_cstr_casecmp(name, "FileSourceMap") == 0) {
    if (command->path != NULL)
      return "Laghu FileSourceMap is allowed only in server configuration";
    if (parameter[0] == '\0' || extra[0] == '\0' || *cursor != '\0' ||
        !laghu_source_mapping_add(&config->source_policy, parameter, extra))
      return "Laghu FileSourceMap is invalid, duplicated, or excessive";
    return NULL;
  }
  if (name[0] == '\0' || extra[0] != '\0') {
    return "Laghu expects one setting and, where required, one value";
  }
  if (ap_cstr_casecmp(name, "RumStore") == 0 ||
      ap_cstr_casecmp(name, "RumStoreLocalSnapshot") == 0 ||
      ap_cstr_casecmp(name, "RumStoreClientLibrary") == 0 ||
      ap_cstr_casecmp(name, "RumStoreRequired") == 0 ||
      ap_cstr_casecmp(name, "RumStoreTimeout") == 0 ||
      ap_cstr_casecmp(name, "RumStoreTtl") == 0 ||
      ap_cstr_casecmp(name, "RumStoreRetryLimit") == 0 ||
      ap_cstr_casecmp(name, "RumStoreSyncInterval") == 0 ||
      ap_cstr_casecmp(name, "RumStoreMemoryLimit") == 0 ||
      ap_cstr_casecmp(name, "RumStorePendingLimit") == 0) {
    uint32_t setting_bit;
    if (command->path != NULL || command->server->is_virtual)
      return "Laghu RUM store settings are allowed only in the main server "
             "context";
    if (ap_cstr_casecmp(name, "RumStore") == 0)
      setting_bit = 1U << 0;
    else if (ap_cstr_casecmp(name, "RumStoreLocalSnapshot") == 0)
      setting_bit = 1U << 1;
    else if (ap_cstr_casecmp(name, "RumStoreClientLibrary") == 0)
      setting_bit = 1U << 2;
    else if (ap_cstr_casecmp(name, "RumStoreRequired") == 0)
      setting_bit = 1U << 3;
    else if (ap_cstr_casecmp(name, "RumStoreTimeout") == 0)
      setting_bit = 1U << 4;
    else if (ap_cstr_casecmp(name, "RumStoreTtl") == 0)
      setting_bit = 1U << 5;
    else if (ap_cstr_casecmp(name, "RumStoreRetryLimit") == 0)
      setting_bit = 1U << 6;
    else if (ap_cstr_casecmp(name, "RumStoreSyncInterval") == 0)
      setting_bit = 1U << 7;
    else if (ap_cstr_casecmp(name, "RumStoreMemoryLimit") == 0)
      setting_bit = 1U << 8;
    else
      setting_bit = 1U << 9;
    if ((config->rum_set_mask & setting_bit) != 0U)
      return "Laghu RUM store setting is duplicated";
    config->rum_set_mask |= setting_bit;
    if (ap_cstr_casecmp(name, "RumStore") == 0) {
      if (!laghu_rum_store_validate(parameter, NULL, 0U))
        return "Laghu RumStore URI is invalid or unsupported";
      config->rum_store = parameter;
    } else if (ap_cstr_casecmp(name, "RumStoreLocalSnapshot") == 0)
      config->rum_snapshot = parameter;
    else if (ap_cstr_casecmp(name, "RumStoreClientLibrary") == 0)
      config->rum_client_library = parameter;
    else if (ap_cstr_casecmp(name, "RumStoreRequired") == 0) {
      laghu_mode mode;
      if (!laghu_apache_on_off(parameter, &mode))
        return "Laghu RumStoreRequired expects On or Off";
      config->rum_required = mode == LAGHU_MODE_ON;
    } else if (ap_cstr_casecmp(name, "RumStoreMemoryLimit") == 0 ||
               ap_cstr_casecmp(name, "RumStorePendingLimit") == 0) {
      size_t parsed;
      if (!laghu_apache_size(parameter, LAGHU_RUM_MAX_RECORD_BYTES, &parsed))
        return "Laghu RUM store size setting is out of range";
      if (ap_cstr_casecmp(name, "RumStoreMemoryLimit") == 0)
        config->rum_memory_limit = parsed;
      else
        config->rum_pending_limit = parsed;
    } else {
      quality = strtoul(parameter, &end, 10);
      if (end == parameter || *end != '\0')
        return "Laghu RUM store numeric setting is invalid";
      if (ap_cstr_casecmp(name, "RumStoreTimeout") == 0 && quality >= 10U &&
          quality <= 10000U)
        config->rum_timeout_ms = (unsigned int)quality;
      else if (ap_cstr_casecmp(name, "RumStoreTtl") == 0 && quality >= 3600U &&
               quality <= 2592000U)
        config->rum_ttl = (unsigned int)quality;
      else if (ap_cstr_casecmp(name, "RumStoreRetryLimit") == 0 &&
               quality <= 10U)
        config->rum_retry_limit = (unsigned int)quality;
      else if (ap_cstr_casecmp(name, "RumStoreSyncInterval") == 0 &&
               quality >= 1U && quality <= 300U)
        config->rum_sync_interval = (unsigned int)quality;
      else
        return "Laghu RUM store numeric setting is out of range";
    }
    return NULL;
  }
  if (parameter[0] == '\0') {
    laghu_mode mode;
    if (!laghu_apache_on_off(name, &mode) ||
        config->core.mode != LAGHU_MODE_UNSET) {
      return "Laghu expects On or Off exactly once in this scope";
    }
    config->core.mode = mode;
    return NULL;
  }
  shared_setting = laghu_config_setting_find(name);
  if (shared_setting != LAGHU_CONFIG_SETTING_UNKNOWN) {
    if (!laghu_config_setting_apply(&config->core, shared_setting, parameter,
                                    shared_error, sizeof(shared_error)))
      return apr_pstrdup(command->pool, shared_error);
    return NULL;
  }
  if (ap_cstr_casecmp(name, "LoadFromFile") == 0) {
    const char *root;
    char lowered[16U];
    size_t index;
    if (command->path != NULL)
      return "Laghu LoadFromFile is allowed only in server configuration";
    if (config->source_mode_set || strlen(parameter) >= sizeof(lowered))
      return "Laghu LoadFromFile is duplicated or invalid";
    for (index = 0U; parameter[index] != '\0'; ++index)
      lowered[index] = (char)tolower((unsigned char)parameter[index]);
    lowered[index] = '\0';
    if (!laghu_source_mode_parse(lowered, true, &config->source_policy.mode))
      return "Laghu LoadFromFile expects Off, Mapped, Native, or Both";
    config->source_mode_set = true;
    if (config->source_policy.mode == LAGHU_SOURCE_FILE_NATIVE ||
        config->source_policy.mode == LAGHU_SOURCE_FILE_BOTH) {
      const core_server_config *core_server =
          ap_get_core_module_config(command->server->module_config);
      root = core_server == NULL ? NULL : core_server->ap_document_root;
      if (root == NULL ||
          strlen(root) >= sizeof(config->source_policy.native_root))
        return "Laghu native file loading requires an absolute document root";
      (void)snprintf(config->source_policy.native_root,
                     sizeof(config->source_policy.native_root), "%s", root);
    }
    return NULL;
  }
  if (ap_cstr_casecmp(name, "TrustedProxy") == 0) {
    apr_ipsubnet_t *subnet;
    char *copy = apr_pstrdup(command->pool, parameter);
    char *slash = strrchr(copy, '/');
    if (slash == NULL) return "Laghu TrustedProxy expects CIDR";
    *slash++ = '\0';
    if (apr_ipsubnet_create(&subnet, copy, slash, command->pool) != APR_SUCCESS)
      return "Laghu TrustedProxy expects a valid CIDR";
    if (config->trusted_proxy == NULL)
      config->trusted_proxy =
          apr_array_make(command->pool, 4, sizeof(apr_ipsubnet_t *));
    *(apr_ipsubnet_t **)apr_array_push(config->trusted_proxy) = subnet;
    return NULL;
  }

  if (ap_cstr_casecmp(name, "WorkerQueue") == 0) {
    if (config->worker_queue != NULL) {
      return "Laghu WorkerQueue may appear only once in this scope";
    }
    config->worker_queue = apr_pstrdup(command->pool, parameter);
    return NULL;
  }
  if (ap_cstr_casecmp(name, "AssetOffloadConfig") == 0) {
    char error[256] = "configuration appears more than once";
    if (config->asset_offload_config != NULL ||
        !laghu_asset_config_load(parameter, &config->asset_offload, error,
                                 sizeof(error)))
      return apr_psprintf(command->pool, "invalid AssetOffloadConfig: %s",
                          error);
    config->asset_offload_config = apr_pstrdup(command->pool, parameter);
    config->asset_offload_loaded = true;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "AssetUploadQueue") == 0) {
    if (config->asset_upload_queue != NULL)
      return "Laghu AssetUploadQueue may appear only once in this scope";
    config->asset_upload_queue = apr_pstrdup(command->pool, parameter);
    return NULL;
  }
  if (ap_cstr_casecmp(name, "FontFetchQueue") == 0) {
    if (config->font_fetch_queue != NULL) {
      return "Laghu FontFetchQueue may appear only once in this scope";
    }
    config->font_fetch_queue = apr_pstrdup(command->pool, parameter);
    return NULL;
  }
  if (ap_cstr_casecmp(name, "FontProviderConfig") == 0) {
    char error[256] = "configuration appears more than once";
    if (config->font_provider_config != NULL ||
        !laghu_font_providers_load(parameter, &config->font_providers, error,
                                   sizeof(error))) {
      return apr_psprintf(command->pool,
                          "Laghu FontProviderConfig is invalid: %s", error);
    }
    config->font_provider_config = apr_pstrdup(command->pool, parameter);
    config->font_providers_loaded = true;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "JavaScriptQueue") == 0) {
    if (config->javascript_queue != NULL)
      return "Laghu JavaScriptQueue may appear only once in this scope";
    config->javascript_queue = apr_pstrdup(command->pool, parameter);
    return NULL;
  }
  if (ap_cstr_casecmp(name, "JavaScriptTarget") == 0) {
    char normalized[LAGHU_JAVASCRIPT_TARGET_SIZE];
    if (config->javascript_target != NULL ||
        !laghu_javascript_target_normalize(parameter, normalized))
      return "Laghu JavaScriptTarget expects a bounded Browserslist query";
    config->javascript_target = apr_pstrdup(command->pool, normalized);
    return NULL;
  }
  if (ap_cstr_casecmp(name, "JavaScriptObservationConfig") == 0) {
    char error[256] = "configuration appears more than once";
    if (config->javascript_observation_config != NULL ||
        !laghu_javascript_observations_load(
            parameter, &config->javascript_observations, error, sizeof(error)))
      return apr_psprintf(command->pool,
                          "Laghu JavaScriptObservationConfig is invalid: %s",
                          error);
    config->javascript_observation_config =
        apr_pstrdup(command->pool, parameter);
    config->javascript_observations_loaded = true;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "JavaScriptDeferConfig") == 0) {
    char error[256] = "configuration appears more than once";
    if (config->javascript_defer_config != NULL ||
        !laghu_javascript_defer_load(parameter, &config->javascript_defer,
                                     error, sizeof(error)))
      return apr_psprintf(command->pool,
                          "Laghu JavaScriptDeferConfig is invalid: %s", error);
    config->javascript_defer_config = apr_pstrdup(command->pool, parameter);
    config->javascript_defer_loaded = true;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "ImageCache") == 0) {
    if (config->image_cache != NULL || config->file_cache_backend != NULL) {
      return "Laghu ImageCache may appear only once in this scope";
    }
    config->image_cache = apr_pstrdup(command->pool, parameter);
    config->image_cache_set = true;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "FileCacheBackend") == 0) {
    char path[LAGHU_RUNTIME_PATH_SIZE];
    if (config->file_cache_backend != NULL || config->image_cache_set ||
        !laghu_cache_backend_uri_parse(parameter, path, sizeof(path)))
      return "Laghu FileCacheBackend expects one local absolute file: URI";
    config->file_cache_backend = apr_pstrdup(command->pool, parameter);
    config->image_cache = apr_pstrdup(command->pool, path);
    return NULL;
  }
  if (ap_cstr_casecmp(name, "FileCacheSize") == 0 ||
      ap_cstr_casecmp(name, "FileCacheInodeLimit") == 0 ||
      ap_cstr_casecmp(name, "FileCacheMetadataSize") == 0) {
    uint32_t bit = ap_cstr_casecmp(name, "FileCacheSize") == 0
                       ? LAGHU_CACHE_SET_SIZE
                       : (ap_cstr_casecmp(name, "FileCacheInodeLimit") == 0
                              ? LAGHU_CACHE_SET_INODES
                              : LAGHU_CACHE_SET_METADATA);
    uint64_t minimum = bit == LAGHU_CACHE_SET_SIZE
                           ? 1024U * 1024U
                           : (bit == LAGHU_CACHE_SET_INODES ? 16U : 16384U);
    uint64_t maximum =
        bit == LAGHU_CACHE_SET_INODES
            ? 100000000U
            : (bit == LAGHU_CACHE_SET_METADATA ? 1024U * 1024U * 1024U
                                               : UINT64_C(1) << 60U);
    uint64_t parsed;
    if ((config->cache_set_mask & bit) != 0U ||
        !(bit == LAGHU_CACHE_SET_INODES
              ? laghu_cache_count_parse(parameter, minimum, maximum, &parsed)
              : laghu_cache_size_parse(parameter, minimum, maximum, &parsed)) ||
        (bit == LAGHU_CACHE_SET_METADATA && parsed > SIZE_MAX))
      return "invalid or duplicate Laghu file cache limit";
    if (bit == LAGHU_CACHE_SET_SIZE)
      config->cache_limits.size_limit = parsed;
    else if (bit == LAGHU_CACHE_SET_INODES)
      config->cache_limits.inode_limit = parsed;
    else
      config->cache_limits.metadata_size = (size_t)parsed;
    config->cache_set_mask |= bit;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "FileCacheCleanInterval") == 0) {
    unsigned int parsed;
    if ((config->cache_set_mask & LAGHU_CACHE_SET_CLEAN) != 0U ||
        !laghu_cache_duration_parse(parameter, 1U, 86400U, &parsed))
      return "Laghu FileCacheCleanInterval expects 1s through 24h";
    config->cache_limits.clean_interval = parsed;
    config->cache_set_mask |= LAGHU_CACHE_SET_CLEAN;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "PurgeMethod") == 0) {
    if ((config->admin_set_mask & LAGHU_ADMIN_SET_METHOD) != 0U ||
        strcmp(parameter, "PURGE") != 0)
      return "Laghu PurgeMethod accepts PURGE once";
    config->purge_method = true;
    config->admin_set_mask |= LAGHU_ADMIN_SET_METHOD;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "PurgeQuery") == 0 ||
      ap_cstr_casecmp(name, "Statistics") == 0 ||
      ap_cstr_casecmp(name, "Metrics") == 0 ||
      ap_cstr_casecmp(name, "Readiness") == 0) {
    uint32_t bit = ap_cstr_casecmp(name, "PurgeQuery") == 0
                       ? LAGHU_ADMIN_SET_QUERY
                       : (ap_cstr_casecmp(name, "Statistics") == 0
                              ? LAGHU_ADMIN_SET_STATS
                              : (ap_cstr_casecmp(name, "Metrics") == 0
                                     ? LAGHU_ADMIN_SET_METRICS
                                     : LAGHU_ADMIN_SET_READINESS));
    bool *target =
        bit == LAGHU_ADMIN_SET_QUERY
            ? &config->purge_query
            : (bit == LAGHU_ADMIN_SET_STATS
                   ? &config->statistics
                   : (bit == LAGHU_ADMIN_SET_METRICS ? &config->metrics
                                                     : &config->readiness));
    if ((config->admin_set_mask & bit) != 0U ||
        (ap_cstr_casecmp(parameter, "On") != 0 &&
         ap_cstr_casecmp(parameter, "Off") != 0))
      return "Laghu administration toggle expects On or Off once";
    *target = ap_cstr_casecmp(parameter, "On") == 0;
    config->admin_set_mask |= bit;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "ReadinessPolicy") == 0) {
    if ((config->admin_set_mask & LAGHU_ADMIN_SET_READINESS_POLICY) != 0U ||
        (ap_cstr_casecmp(parameter, "Degraded") != 0 &&
         ap_cstr_casecmp(parameter, "Strict") != 0))
      return "Laghu ReadinessPolicy expects Degraded or Strict once";
    config->readiness_strict = ap_cstr_casecmp(parameter, "Strict") == 0;
    config->admin_set_mask |= LAGHU_ADMIN_SET_READINESS_POLICY;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "PurgeTokenFile") == 0 ||
      ap_cstr_casecmp(name, "CacheFlushFile") == 0) {
    uint32_t bit = ap_cstr_casecmp(name, "PurgeTokenFile") == 0
                       ? LAGHU_ADMIN_SET_TOKEN
                       : LAGHU_ADMIN_SET_FLUSH;
    const char **target = bit == LAGHU_ADMIN_SET_TOKEN
                              ? &config->purge_token_file
                              : &config->cache_flush_file;
    if ((config->admin_set_mask & bit) != 0U ||
        !ap_os_is_path_absolute(command->pool, parameter))
      return "Laghu administration file expects one absolute path";
    *target = apr_pstrdup(command->pool, parameter);
    config->admin_set_mask |= bit;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "PurgeAllow") == 0) {
    apr_ipsubnet_t *subnet;
    char *copy = apr_pstrdup(command->pool, parameter);
    char *slash = strrchr(copy, '/');
    if (slash == NULL) return "Laghu PurgeAllow expects CIDR";
    *slash++ = '\0';
    if (apr_ipsubnet_create(&subnet, copy, slash, command->pool) != APR_SUCCESS)
      return "Laghu PurgeAllow expects a valid CIDR";
    if (config->purge_allow == NULL)
      config->purge_allow =
          apr_array_make(command->pool, 4, sizeof(apr_ipsubnet_t *));
    *(apr_ipsubnet_t **)apr_array_push(config->purge_allow) = subnet;
    return NULL;
  }
  return "unknown Laghu setting";
}
