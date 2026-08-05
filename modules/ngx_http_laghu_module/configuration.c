// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <errno.h>
#include <ngx_config.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/catalog.h"
#include "laghu/config.h"
#include "laghu/css.h"
#include "laghu/instrumentation.h"
#include "laghu/types.h"
#include "ngx_http_laghu_internal.h"

void *ngx_http_laghu_create_main_conf(ngx_conf_t *configuration) {
  ngx_http_laghu_main_conf_t *conf =
      ngx_pcalloc(configuration->pool, sizeof(*conf));
  if (conf == NULL) return NULL;
  ngx_str_set(&conf->store_uri, "local:");
  conf->memory_limit = LAGHU_RUM_DEFAULT_MEMORY_BYTES;
  conf->pending_limit = LAGHU_RUM_DEFAULT_PENDING_BYTES;
  conf->ttl_seconds = LAGHU_IMAGE_METADATA_TTL_DEFAULT;
  conf->sync_interval_seconds = LAGHU_RUM_DEFAULT_SYNC_SECONDS;
  conf->timeout_ms = LAGHU_RUM_DEFAULT_TIMEOUT_MS;
  conf->retry_limit = LAGHU_RUM_DEFAULT_RETRY_LIMIT;
  conf->required = 0;
  return conf;
}

void *ngx_http_laghu_create_loc_conf(ngx_conf_t *configuration) {
  ngx_http_laghu_loc_conf_t *conf;
  ngx_pool_cleanup_t *cleanup;

  conf = ngx_pcalloc(configuration->pool, sizeof(ngx_http_laghu_loc_conf_t));
  if (conf == NULL) {
    return NULL;
  }

  laghu_config_init(&conf->core);
  laghu_source_policy_init(&conf->source_policy);
  laghu_runtime_queue_init(&conf->runtime_queue);
  laghu_runtime_queue_init(&conf->font_fetch_runtime_queue);
  laghu_runtime_queue_init(&conf->javascript_runtime_queue);
  conf->file_cache_size = NGX_CONF_UNSET_SIZE;
  conf->file_cache_inode_limit = NGX_CONF_UNSET_SIZE;
  conf->file_cache_metadata_size = NGX_CONF_UNSET_SIZE;
  conf->file_cache_clean_interval = NGX_CONF_UNSET_UINT;
  conf->purge_method = NGX_CONF_UNSET;
  conf->purge_query = NGX_CONF_UNSET;
  conf->statistics = NGX_CONF_UNSET;
  conf->metrics = NGX_CONF_UNSET;
  conf->readiness = NGX_CONF_UNSET;
  conf->readiness_strict = NGX_CONF_UNSET;
  cleanup = ngx_pool_cleanup_add(configuration->pool, 0);
  if (cleanup == NULL) {
    return NULL;
  }
  cleanup->handler = ngx_http_laghu_queue_cleanup;
  cleanup->data = &conf->runtime_queue;
  cleanup = ngx_pool_cleanup_add(configuration->pool, 0);
  if (cleanup == NULL) {
    return NULL;
  }
  cleanup->handler = ngx_http_laghu_queue_cleanup;
  cleanup->data = &conf->font_fetch_runtime_queue;
  cleanup = ngx_pool_cleanup_add(configuration->pool, 0);
  if (cleanup == NULL) return NULL;
  cleanup->handler = ngx_http_laghu_queue_cleanup;
  cleanup->data = &conf->javascript_runtime_queue;
  return conf;
}

char *ngx_http_laghu_merge_loc_conf(ngx_conf_t *configuration, void *parent,
                                    void *child) {
  ngx_http_laghu_loc_conf_t *parent_conf = parent;
  ngx_http_laghu_loc_conf_t *child_conf = child;
  laghu_config merged;
  laghu_policy policy;
  laghu_source_policy source;
  char source_error[160U];
  ngx_http_laghu_main_conf_t *main_conf;

  (void)configuration;

  if (!laghu_resource_rules_merge_valid(&parent_conf->core, &child_conf->core))
    return "invalid, duplicate, conflicting, or excessive inherited resource "
           "rules";
  laghu_config_merge(&merged, &parent_conf->core, &child_conf->core);
  if (!laghu_resolve_config_policy(&merged, &policy))
    return "invalid or conflicting inherited laghu filter policy";
  child_conf->core = merged;
  ngx_conf_merge_str_value(child_conf->worker_queue, parent_conf->worker_queue,
                           LAGHU_NGINX_DEFAULT_QUEUE);
  ngx_conf_merge_str_value(child_conf->font_fetch_queue,
                           parent_conf->font_fetch_queue,
                           LAGHU_NGINX_DEFAULT_FONT_QUEUE);
  ngx_conf_merge_str_value(child_conf->font_provider_config,
                           parent_conf->font_provider_config, "");
  ngx_conf_merge_str_value(child_conf->javascript_queue,
                           parent_conf->javascript_queue,
                           LAGHU_NGINX_DEFAULT_JAVASCRIPT_QUEUE);
  ngx_conf_merge_str_value(child_conf->javascript_target,
                           parent_conf->javascript_target,
                           "defaults and supports es6-module and not dead");
  ngx_conf_merge_str_value(child_conf->javascript_observation_config,
                           parent_conf->javascript_observation_config, "");
  ngx_conf_merge_str_value(child_conf->javascript_defer_config,
                           parent_conf->javascript_defer_config, "");
  if (!child_conf->font_providers_loaded &&
      parent_conf->font_providers_loaded) {
    child_conf->font_providers = parent_conf->font_providers;
    child_conf->font_providers_loaded = true;
  }
  if (!child_conf->javascript_observations_loaded &&
      parent_conf->javascript_observations_loaded) {
    child_conf->javascript_observations = parent_conf->javascript_observations;
    child_conf->javascript_observations_loaded = true;
  }
  if (!child_conf->javascript_defer_loaded &&
      parent_conf->javascript_defer_loaded) {
    child_conf->javascript_defer = parent_conf->javascript_defer;
    child_conf->javascript_defer_loaded = true;
  }
  if (child_conf->file_cache_backend.len == 0U &&
      !child_conf->image_cache_set) {
    child_conf->file_cache_backend = parent_conf->file_cache_backend;
    child_conf->image_cache = parent_conf->image_cache;
    child_conf->image_cache_set = parent_conf->image_cache_set;
  }
  if (child_conf->image_cache.len == 0U) {
    ngx_str_set(&child_conf->image_cache, LAGHU_NGINX_DEFAULT_CACHE);
  }
  ngx_conf_merge_size_value(child_conf->file_cache_size,
                            parent_conf->file_cache_size,
                            (size_t)LAGHU_CACHE_DEFAULT_SIZE_BYTES);
  ngx_conf_merge_size_value(child_conf->file_cache_inode_limit,
                            parent_conf->file_cache_inode_limit,
                            LAGHU_CACHE_DEFAULT_INODE_LIMIT);
  ngx_conf_merge_size_value(child_conf->file_cache_metadata_size,
                            parent_conf->file_cache_metadata_size,
                            LAGHU_CACHE_DEFAULT_METADATA_BYTES);
  ngx_conf_merge_uint_value(child_conf->file_cache_clean_interval,
                            parent_conf->file_cache_clean_interval,
                            LAGHU_CACHE_DEFAULT_CLEAN_INTERVAL);
  ngx_conf_merge_value(child_conf->purge_method, parent_conf->purge_method, 0);
  ngx_conf_merge_value(child_conf->purge_query, parent_conf->purge_query, 0);
  ngx_conf_merge_value(child_conf->statistics, parent_conf->statistics, 0);
  ngx_conf_merge_value(child_conf->metrics, parent_conf->metrics, 0);
  ngx_conf_merge_value(child_conf->readiness, parent_conf->readiness, 0);
  ngx_conf_merge_value(child_conf->readiness_strict,
                       parent_conf->readiness_strict, 0);
  main_conf =
      ngx_http_conf_get_module_main_conf(configuration, ngx_http_laghu_module);
  if (child_conf->metrics || child_conf->readiness) {
    if (main_conf->operational_cache.len == 0U)
      main_conf->operational_cache = child_conf->image_cache;
    else if (main_conf->operational_cache.len != child_conf->image_cache.len ||
             ngx_strncmp(main_conf->operational_cache.data,
                         child_conf->image_cache.data,
                         child_conf->image_cache.len) != 0)
      return "metrics and readiness locations must share one file cache";
  }
  ngx_conf_merge_str_value(child_conf->purge_token_file,
                           parent_conf->purge_token_file, "");
  ngx_conf_merge_str_value(child_conf->cache_flush_file,
                           parent_conf->cache_flush_file, "");
  if (child_conf->purge_allow == NULL)
    child_conf->purge_allow = parent_conf->purge_allow;
  if ((child_conf->purge_method || child_conf->purge_query ||
       child_conf->statistics || child_conf->metrics ||
       child_conf->readiness) &&
      (child_conf->purge_token_file.len == 0U ||
       child_conf->purge_allow == NULL || child_conf->purge_allow->nelts == 0U))
    return "network administration requires purge_token_file and purge_allow";
  if (child_conf->trusted_proxy == NULL)
    child_conf->trusted_proxy = parent_conf->trusted_proxy;
  if (child_conf->core.respect_x_forwarded_proto == LAGHU_MODE_ON &&
      (child_conf->trusted_proxy == NULL ||
       child_conf->trusted_proxy->nelts == 0U))
    return "respect_x_forwarded_proto requires trusted_proxy";
  ngx_conf_merge_str_value(child_conf->asset_offload_config,
                           parent_conf->asset_offload_config, "");
  ngx_conf_merge_str_value(child_conf->asset_upload_queue,
                           parent_conf->asset_upload_queue, "");
  if (!child_conf->asset_offload_loaded && parent_conf->asset_offload_loaded) {
    child_conf->asset_offload = parent_conf->asset_offload;
    child_conf->asset_offload_loaded = true;
  }
  if (child_conf->asset_offload_loaded &&
      (child_conf->asset_upload_queue.len == 0U ||
       ngx_strcmp(child_conf->asset_upload_queue.data,
                  child_conf->asset_offload.queue_path) != 0))
    return "asset_upload_queue must match the loaded asset offload config";
  if (!child_conf->asset_offload_loaded &&
      child_conf->asset_upload_queue.len != 0U)
    return "asset_upload_queue requires asset_offload_config";
  if (!laghu_source_policy_merge(&source, &parent_conf->source_policy,
                                 &child_conf->source_policy))
    return "invalid, duplicate, or excessive inherited file source mapping";
  if (child_conf->source_mode_set) source.mode = child_conf->source_policy.mode;
  if (source.mode == LAGHU_SOURCE_FILE_OFF) {
    source.mapping_count = 0U;
    source.native_root[0] = '\0';
  }
  if (source.mode == LAGHU_SOURCE_FILE_NATIVE ||
      source.mode == LAGHU_SOURCE_FILE_BOTH) {
    ngx_http_core_loc_conf_t *core_location =
        ngx_http_conf_get_module_loc_conf(configuration, ngx_http_core_module);
    if (core_location == NULL || core_location->alias != 0U ||
        core_location->root.len == 0U ||
        core_location->root.len >= sizeof(source.native_root) ||
        core_location->root_lengths != NULL)
      return "native file loading requires one static non-alias NGINX root";
    ngx_memcpy(source.native_root, core_location->root.data,
               core_location->root.len);
    source.native_root[core_location->root.len] = '\0';
  }
  if (!laghu_source_policy_validate(&source, true, source_error,
                                    sizeof(source_error)))
    return "invalid direct file loading configuration";
  child_conf->source_policy = source;
  if (source.mode != LAGHU_SOURCE_FILE_OFF) {
    if (!child_conf->asset_offload_loaded)
      return "direct file loading requires asset_offload_config";
    if (!laghu_source_registry_publish(
            (const char *)child_conf->asset_upload_queue.data, &source))
      return "unable to publish direct file source registry";
  }
  return NGX_CONF_OK;
}

char *ngx_http_laghu_command(ngx_conf_t *configuration, ngx_command_t *command,
                             void *conf) {
  ngx_http_laghu_loc_conf_t *location = conf;
  ngx_http_laghu_main_conf_t *main_conf =
      ngx_http_conf_get_module_main_conf(configuration, ngx_http_laghu_module);
  ngx_str_t *values = configuration->args->elts;
  laghu_config_setting shared_setting;
  char shared_error[160U];

  (void)command;

  if (configuration->args->nelts == 4 &&
      ngx_strcmp(values[1].data, "file_source_map") == 0) {
    if (!laghu_source_mapping_add(&location->source_policy,
                                  (const char *)values[2].data,
                                  (const char *)values[3].data))
      return "file source mapping is invalid, duplicate, or excessive";
    return NGX_CONF_OK;
  }

  if (configuration->args->nelts == 3 && values[1].len >= 9U &&
      ngx_strncmp(values[1].data, "rum_store", 9U) == 0) {
    ngx_int_t parsed;
    uint32_t setting_bit = 0U;
    if (configuration->cmd_type != NGX_HTTP_MAIN_CONF)
      return "laghu rum_store settings are allowed only in the http context";
    if (ngx_strcmp(values[1].data, "rum_store") == 0)
      setting_bit = 1U << 0;
    else if (ngx_strcmp(values[1].data, "rum_store_local_snapshot") == 0)
      setting_bit = 1U << 1;
    else if (ngx_strcmp(values[1].data, "rum_store_client_library") == 0)
      setting_bit = 1U << 2;
    else if (ngx_strcmp(values[1].data, "rum_store_required") == 0)
      setting_bit = 1U << 3;
    else if (ngx_strcmp(values[1].data, "rum_store_timeout") == 0)
      setting_bit = 1U << 4;
    else if (ngx_strcmp(values[1].data, "rum_store_ttl") == 0)
      setting_bit = 1U << 5;
    else if (ngx_strcmp(values[1].data, "rum_store_retry_limit") == 0)
      setting_bit = 1U << 6;
    else if (ngx_strcmp(values[1].data, "rum_store_sync_interval") == 0)
      setting_bit = 1U << 7;
    else if (ngx_strcmp(values[1].data, "rum_store_memory_limit") == 0)
      setting_bit = 1U << 8;
    else if (ngx_strcmp(values[1].data, "rum_store_pending_limit") == 0)
      setting_bit = 1U << 9;
    if (setting_bit == 0U) return "unknown laghu rum_store setting";
    if ((main_conf->set_mask & setting_bit) != 0U)
      return "duplicate laghu rum_store setting";
    main_conf->set_mask |= setting_bit;
    if (ngx_strcmp(values[1].data, "rum_store") == 0) {
      if (!laghu_rum_store_validate((const char *)values[2].data, NULL, 0U))
        return "laghu rum_store URI is invalid or unsupported";
      main_conf->store_uri = values[2];
      return NGX_CONF_OK;
    }
    if (ngx_strcmp(values[1].data, "rum_store_local_snapshot") == 0) {
      main_conf->snapshot_path = values[2];
      return NGX_CONF_OK;
    }
    if (ngx_strcmp(values[1].data, "rum_store_client_library") == 0) {
      main_conf->client_library = values[2];
      return NGX_CONF_OK;
    }
    if (ngx_strcmp(values[1].data, "rum_store_required") == 0) {
      if (ngx_strcmp(values[2].data, "on") == 0)
        main_conf->required = 1;
      else if (ngx_strcmp(values[2].data, "off") == 0)
        main_conf->required = 0;
      else
        return "laghu rum_store_required expects on or off";
      return NGX_CONF_OK;
    }
    if (ngx_strcmp(values[1].data, "rum_store_memory_limit") == 0 ||
        ngx_strcmp(values[1].data, "rum_store_pending_limit") == 0) {
      ssize_t size = ngx_parse_size(&values[2]);
      if (size < (ssize_t)LAGHU_RUM_MAX_RECORD_BYTES)
        return "laghu RUM size limit is too small or invalid";
      if (ngx_strcmp(values[1].data, "rum_store_memory_limit") == 0)
        main_conf->memory_limit = (size_t)size;
      else
        main_conf->pending_limit = (size_t)size;
      return NGX_CONF_OK;
    }
    parsed = ngx_atoi(values[2].data, values[2].len);
    if (parsed == NGX_ERROR) return "laghu RUM setting expects an integer";
    if (ngx_strcmp(values[1].data, "rum_store_timeout") == 0 && parsed >= 10 &&
        parsed <= 10000)
      main_conf->timeout_ms = (ngx_uint_t)parsed;
    else if (ngx_strcmp(values[1].data, "rum_store_ttl") == 0 &&
             parsed >= 3600 && parsed <= 2592000)
      main_conf->ttl_seconds = (ngx_uint_t)parsed;
    else if (ngx_strcmp(values[1].data, "rum_store_retry_limit") == 0 &&
             parsed >= 0 && parsed <= 10)
      main_conf->retry_limit = (ngx_uint_t)parsed;
    else if (ngx_strcmp(values[1].data, "rum_store_sync_interval") == 0 &&
             parsed >= 1 && parsed <= 300)
      main_conf->sync_interval_seconds = (ngx_uint_t)parsed;
    else
      return "unknown or out-of-range laghu rum_store setting";
    return NGX_CONF_OK;
  }

  if (configuration->args->nelts == 2) {
    if (location->core.mode != LAGHU_MODE_UNSET) {
      return "is duplicate";
    }

    if (ngx_strcmp(values[1].data, "on") == 0) {
      location->core.mode = LAGHU_MODE_ON;
      return NGX_CONF_OK;
    }

    if (ngx_strcmp(values[1].data, "off") == 0) {
      location->core.mode = LAGHU_MODE_OFF;
      return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                       "laghu expects 'on', 'off', 'preset <name>', or "
                       "'rewrite_level <name>', or 'allow_api on|off'");
    return NGX_CONF_ERROR;
  }

  shared_setting = laghu_config_setting_find((const char *)values[1].data);
  if (shared_setting != LAGHU_CONFIG_SETTING_UNKNOWN) {
    if (!laghu_config_setting_apply(&location->core, shared_setting,
                                    (const char *)values[2].data, shared_error,
                                    sizeof(shared_error))) {
      ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                         "invalid laghu %V: %s", &values[1], shared_error);
      return NGX_CONF_ERROR;
    }
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "trusted_proxy") == 0) {
    ngx_cidr_t cidr;
    ngx_cidr_t *stored;
    if (ngx_ptocidr(&values[2], &cidr) != NGX_OK)
      return "trusted_proxy expects a canonical CIDR";
    if (location->trusted_proxy == NULL) {
      location->trusted_proxy =
          ngx_array_create(configuration->pool, 4U, sizeof(ngx_cidr_t));
      if (location->trusted_proxy == NULL) return NGX_CONF_ERROR;
    }
    stored = ngx_array_push(location->trusted_proxy);
    if (stored == NULL) return NGX_CONF_ERROR;
    *stored = cidr;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "worker_queue") == 0) {
    if (location->worker_queue.len != 0U) {
      return "is duplicate";
    }
    location->worker_queue = values[2];
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "asset_offload_config") == 0) {
    char error[256] = "configuration appears more than once";
    if (location->asset_offload_config.len != 0U ||
        !laghu_asset_config_load((const char *)values[2].data,
                                 &location->asset_offload, error,
                                 sizeof(error))) {
      ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                         "invalid asset offload config \"%V\": %s", &values[2],
                         error);
      return NGX_CONF_ERROR;
    }
    location->asset_offload_config = values[2];
    location->asset_offload_loaded = true;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "asset_upload_queue") == 0) {
    if (location->asset_upload_queue.len != 0U) return "is duplicate";
    location->asset_upload_queue = values[2];
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "load_from_file") == 0) {
    if (location->source_mode_set ||
        !laghu_source_mode_parse((const char *)values[2].data, true,
                                 &location->source_policy.mode))
      return "load_from_file expects off, mapped, native, or both once";
    location->source_mode_set = true;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "font_fetch_queue") == 0) {
    if (location->font_fetch_queue.len != 0U) {
      return "is duplicate";
    }
    location->font_fetch_queue = values[2];
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "font_provider_config") == 0) {
    char error[256] = "configuration appears more than once";
    if (location->font_provider_config.len != 0U ||
        !laghu_font_providers_load((const char *)values[2].data,
                                   &location->font_providers, error,
                                   sizeof(error))) {
      ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                         "invalid Laghu font provider config \"%V\": %s",
                         &values[2], error);
      return NGX_CONF_ERROR;
    }
    location->font_provider_config = values[2];
    location->font_providers_loaded = true;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "javascript_queue") == 0) {
    if (location->javascript_queue.len != 0U) return "is duplicate";
    location->javascript_queue = values[2];
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "javascript_target") == 0) {
    char normalized[LAGHU_JAVASCRIPT_TARGET_SIZE];
    if (location->javascript_target.len != 0U ||
        !laghu_javascript_target_normalize((const char *)values[2].data,
                                           normalized))
      return "laghu javascript_target expects a bounded Browserslist query";
    location->javascript_target = values[2];
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "javascript_observation_config") == 0) {
    char error[256] = "configuration appears more than once";
    if (location->javascript_observation_config.len != 0U ||
        !laghu_javascript_observations_load((const char *)values[2].data,
                                            &location->javascript_observations,
                                            error, sizeof(error))) {
      ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                         "invalid JavaScript observation config \"%V\": %s",
                         &values[2], error);
      return NGX_CONF_ERROR;
    }
    location->javascript_observation_config = values[2];
    location->javascript_observations_loaded = true;
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "javascript_defer_config") == 0) {
    char error[256U];
    if (location->javascript_defer_config.len != 0U ||
        !laghu_javascript_defer_load((const char *)values[2].data,
                                     &location->javascript_defer, error,
                                     sizeof(error))) {
      ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                         "invalid laghu javascript_defer_config: %s", error);
      return NGX_CONF_ERROR;
    }
    location->javascript_defer_config = values[2];
    location->javascript_defer_loaded = true;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "image_cache") == 0) {
    if (location->image_cache.len != 0U ||
        location->file_cache_backend.len != 0U) {
      return "is duplicate";
    }
    location->image_cache = values[2];
    location->image_cache_set = true;
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "file_cache_backend") == 0) {
    char backend_path[LAGHU_RUNTIME_PATH_SIZE];
    u_char *path;
    if (location->file_cache_backend.len != 0U || location->image_cache_set ||
        !laghu_cache_backend_uri_parse((const char *)values[2].data,
                                       backend_path, sizeof(backend_path)))
      return "file_cache_backend expects one local absolute file: URI";
    path = ngx_pnalloc(configuration->pool, strlen(backend_path) + 1U);
    if (path == NULL) return NGX_CONF_ERROR;
    ngx_memcpy(path, backend_path, strlen(backend_path) + 1U);
    location->file_cache_backend = values[2];
    location->image_cache.data = path;
    location->image_cache.len = strlen(backend_path);
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "file_cache_size") == 0 ||
      ngx_strcmp(values[1].data, "file_cache_inode_limit") == 0 ||
      ngx_strcmp(values[1].data, "file_cache_metadata_size") == 0) {
    ssize_t parsed = ngx_strcmp(values[1].data, "file_cache_inode_limit") == 0
                         ? ngx_atoi(values[2].data, values[2].len)
                         : ngx_parse_size(&values[2]);
    size_t *target =
        ngx_strcmp(values[1].data, "file_cache_size") == 0
            ? &location->file_cache_size
            : (ngx_strcmp(values[1].data, "file_cache_inode_limit") == 0
                   ? &location->file_cache_inode_limit
                   : &location->file_cache_metadata_size);
    size_t minimum =
        target == &location->file_cache_size
            ? 1024U * 1024U
            : (target == &location->file_cache_inode_limit ? 16U : 16384U);
    if (*target != NGX_CONF_UNSET_SIZE || parsed < 0 ||
        (size_t)parsed < minimum)
      return "invalid or duplicate file cache limit";
    *target = (size_t)parsed;
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "file_cache_clean_interval") == 0) {
    time_t parsed = ngx_parse_time(&values[2], 1);
    if (location->file_cache_clean_interval != NGX_CONF_UNSET_UINT ||
        parsed < 1 || parsed > 86400)
      return "file_cache_clean_interval expects 1s through 24h";
    location->file_cache_clean_interval = (ngx_uint_t)parsed;
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "purge_method") == 0) {
    if (location->purge_method != NGX_CONF_UNSET ||
        ngx_strcmp(values[2].data, "PURGE") != 0)
      return "purge_method accepts PURGE once";
    location->purge_method = 1;
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "purge_query") == 0 ||
      ngx_strcmp(values[1].data, "statistics") == 0 ||
      ngx_strcmp(values[1].data, "metrics") == 0 ||
      ngx_strcmp(values[1].data, "readiness") == 0) {
    ngx_flag_t *target =
        ngx_strcmp(values[1].data, "purge_query") == 0
            ? &location->purge_query
            : (ngx_strcmp(values[1].data, "statistics") == 0
                   ? &location->statistics
                   : (ngx_strcmp(values[1].data, "metrics") == 0
                          ? &location->metrics
                          : &location->readiness));
    if (*target != NGX_CONF_UNSET) return "is duplicate";
    if (ngx_strcmp(values[2].data, "on") == 0)
      *target = 1;
    else if (ngx_strcmp(values[2].data, "off") == 0)
      *target = 0;
    else
      return "expects on or off";
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "readiness_policy") == 0) {
    if (location->readiness_strict != NGX_CONF_UNSET)
      return "readiness_policy is duplicate";
    if (ngx_strcmp(values[2].data, "strict") == 0)
      location->readiness_strict = 1;
    else if (ngx_strcmp(values[2].data, "degraded") == 0)
      location->readiness_strict = 0;
    else
      return "readiness_policy expects degraded or strict";
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "purge_token_file") == 0 ||
      ngx_strcmp(values[1].data, "cache_flush_file") == 0) {
    ngx_str_t *target = ngx_strcmp(values[1].data, "purge_token_file") == 0
                            ? &location->purge_token_file
                            : &location->cache_flush_file;
    if (target->len != 0U || values[2].len == 0U ||
#ifdef _WIN32
        !(values[2].len >= 3U &&
          ((values[2].data[0] >= 'A' && values[2].data[0] <= 'Z') ||
           (values[2].data[0] >= 'a' && values[2].data[0] <= 'z')) &&
          values[2].data[1] == ':' &&
          (values[2].data[2] == '/' || values[2].data[2] == '\\'))
#else
        values[2].data[0] != '/'
#endif
    )
      return "expects one absolute local path";
    *target = values[2];
    return NGX_CONF_OK;
  }
  if (ngx_strcmp(values[1].data, "purge_allow") == 0) {
    ngx_cidr_t cidr;
    ngx_cidr_t *stored;
    if (ngx_ptocidr(&values[2], &cidr) != NGX_OK)
      return "purge_allow expects a canonical CIDR";
    if (location->purge_allow == NULL) {
      location->purge_allow =
          ngx_array_create(configuration->pool, 4U, sizeof(ngx_cidr_t));
      if (location->purge_allow == NULL) return NGX_CONF_ERROR;
    }
    stored = ngx_array_push(location->purge_allow);
    if (stored == NULL) return NGX_CONF_ERROR;
    *stored = cidr;
    return NGX_CONF_OK;
  }

  ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                     "unsupported laghu command \"%V\"", &values[1]);
  return NGX_CONF_ERROR;
}
