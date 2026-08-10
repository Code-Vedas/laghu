// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>
#include <stdio.h>
#include <string.h>

#include "laghu/config.h"
#include "ngx_http_laghu_internal.h"

static bool ngx_http_laghu_rum_setting(laghu_service_setting setting) {
  return setting >= LAGHU_SERVICE_SETTING_RUM_STORE &&
         setting <= LAGHU_SERVICE_SETTING_RUM_STORE_PENDING_LIMIT;
}

static void ngx_http_laghu_service_cleanup(void *data) {
  laghu_service_config_dispose(data);
}

static bool ngx_http_laghu_service_cleanup_register(
    ngx_pool_t *pool, laghu_service_config *service) {
  ngx_pool_cleanup_t *cleanup = ngx_pool_cleanup_add(pool, 0);
  if (cleanup == NULL) return false;
  cleanup->handler = ngx_http_laghu_service_cleanup;
  cleanup->data = service;
  return true;
}

static char *ngx_http_laghu_service_error(
    ngx_conf_t *configuration, ngx_str_t *name,
    const laghu_service_diagnostic *error) {
  ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0, "invalid laghu %V: %s",
                     name, error->message);
  return NGX_CONF_ERROR;
}

static void ngx_http_laghu_service_defaults(laghu_service_config *service) {
  if (service->worker_queue[0] == '\0')
    (void)snprintf(service->worker_queue, sizeof(service->worker_queue), "%s",
                   LAGHU_NGINX_DEFAULT_QUEUE);
  if (service->font_provider_config[0] != '\0' &&
      service->font_fetch_queue[0] == '\0')
    (void)snprintf(service->font_fetch_queue, sizeof(service->font_fetch_queue),
                   "%s", LAGHU_NGINX_DEFAULT_FONT_QUEUE);
  if (service->javascript_queue[0] == '\0')
    (void)snprintf(service->javascript_queue, sizeof(service->javascript_queue),
                   "%s", LAGHU_NGINX_DEFAULT_JAVASCRIPT_QUEUE);
  if (service->image_cache[0] == '\0')
    (void)snprintf(service->image_cache, sizeof(service->image_cache), "%s",
                   LAGHU_NGINX_DEFAULT_CACHE);
}

void *ngx_http_laghu_create_main_conf(ngx_conf_t *configuration) {
  ngx_http_laghu_main_conf_t *conf =
      ngx_pcalloc(configuration->pool, sizeof(*conf));
  if (conf != NULL) {
    laghu_service_config_init(&conf->service);
    conf->queue_configs = ngx_array_create(configuration->pool, 8U,
                                           sizeof(ngx_http_laghu_loc_conf_t *));
    if (conf->queue_configs == NULL) return NULL;
    if (!ngx_http_laghu_service_cleanup_register(configuration->pool,
                                                 &conf->service))
      return NULL;
  }
  return conf;
}

void *ngx_http_laghu_create_loc_conf(ngx_conf_t *configuration) {
  ngx_http_laghu_loc_conf_t *conf;
  conf = ngx_pcalloc(configuration->pool, sizeof(*conf));
  if (conf == NULL) return NULL;
  laghu_config_init(&conf->core);
  laghu_service_config_init(&conf->service);
  if (!ngx_http_laghu_service_cleanup_register(configuration->pool,
                                               &conf->service))
    return NULL;
  laghu_runtime_queue_init(&conf->runtime_queue);
  laghu_runtime_queue_init(&conf->font_fetch_runtime_queue);
  laghu_runtime_queue_init(&conf->javascript_runtime_queue);
  laghu_runtime_queue_init(&conf->html_refresh_runtime_queue);
  laghu_runtime_queue_init(&conf->chrome_analysis_runtime_queue);
  return conf;
}

char *ngx_http_laghu_merge_loc_conf(ngx_conf_t *configuration, void *parent,
                                    void *child) {
  ngx_http_laghu_loc_conf_t *parent_conf = parent;
  ngx_http_laghu_loc_conf_t *child_conf = child;
  ngx_http_laghu_main_conf_t *main_conf;
  laghu_config merged;
  laghu_policy policy;
  laghu_service_diagnostic error = {0};
  laghu_service_finalize_options options = {
      .native_file_loading = true,
      .require_admin_authorization = true,
      .respect_x_forwarded_proto =
          child_conf->core.respect_x_forwarded_proto == LAGHU_MODE_ON};
  if (!laghu_resource_rules_merge_valid(&parent_conf->core, &child_conf->core))
    return "invalid, duplicate, conflicting, or excessive inherited resource "
           "rules";
  if (!laghu_domain_policy_merge_valid(&parent_conf->core.domain_policy,
                                       &child_conf->core.domain_policy))
    return "invalid, duplicate, conflicting, or excessive inherited domain "
           "policy";
  laghu_config_merge(&merged, &parent_conf->core, &child_conf->core);
  if (!laghu_resolve_config_policy(&merged, &policy))
    return "invalid or conflicting inherited laghu filter policy";
  child_conf->core = merged;
  options.respect_x_forwarded_proto =
      child_conf->core.respect_x_forwarded_proto == LAGHU_MODE_ON;
  if (!laghu_service_config_merge(&child_conf->service, &parent_conf->service,
                                  &child_conf->service, &error))
    return "invalid inherited laghu service configuration";
  ngx_http_laghu_service_defaults(&child_conf->service);
  if (child_conf->service.source_policy.mode == LAGHU_SOURCE_FILE_NATIVE ||
      child_conf->service.source_policy.mode == LAGHU_SOURCE_FILE_BOTH) {
    ngx_http_core_loc_conf_t *core_location =
        ngx_http_conf_get_module_loc_conf(configuration, ngx_http_core_module);
    if (core_location == NULL || core_location->alias != 0U ||
        core_location->root.len == 0U ||
        core_location->root.len >=
            sizeof(child_conf->service.source_policy.native_root) ||
        core_location->root_lengths != NULL)
      return "native file loading requires one static non-alias NGINX root";
    ngx_memcpy(child_conf->service.source_policy.native_root,
               core_location->root.data, core_location->root.len);
    child_conf->service.source_policy.native_root[core_location->root.len] =
        '\0';
  }
  if (!laghu_service_config_finalize(&child_conf->service, &options, &error))
    return "invalid laghu service configuration";
  if (child_conf->service.source_policy.mode != LAGHU_SOURCE_FILE_OFF &&
      !laghu_source_registry_publish(child_conf->service.asset_upload_queue,
                                     &child_conf->service.source_policy))
    return "unable to publish direct file source registry";
  main_conf =
      ngx_http_conf_get_module_main_conf(configuration, ngx_http_laghu_module);
  if (child_conf->service.metrics || child_conf->service.readiness) {
    if (main_conf->operational_cache.len == 0U) {
      main_conf->operational_cache.data =
          (u_char *)child_conf->service.image_cache;
      main_conf->operational_cache.len =
          strlen(child_conf->service.image_cache);
    } else if (main_conf->operational_cache.len !=
                   strlen(child_conf->service.image_cache) ||
               ngx_strncmp(main_conf->operational_cache.data,
                           child_conf->service.image_cache,
                           main_conf->operational_cache.len) != 0)
      return "metrics and readiness locations must share one file cache";
  }
  if (child_conf->core.mode == LAGHU_MODE_ON && !child_conf->queue_registered) {
    ngx_http_laghu_loc_conf_t **entry;
    if (main_conf == NULL || main_conf->queue_configs == NULL)
      return NGX_CONF_ERROR;
    if (main_conf->queue_configs->nelts >= LAGHU_NGINX_QUEUE_CONFIG_LIMIT) {
      ngx_conf_log_error(
          NGX_LOG_WARN, configuration, 0,
          "laghu queue attachment limit reached; queue work disabled");
      return NGX_CONF_OK;
    }
    entry = ngx_array_push(main_conf->queue_configs);
    if (entry == NULL) return NGX_CONF_ERROR;
    *entry = child_conf;
    child_conf->queue_registered = true;
  }
  return NGX_CONF_OK;
}

char *ngx_http_laghu_command(ngx_conf_t *configuration, ngx_command_t *command,
                             void *conf) {
  ngx_http_laghu_loc_conf_t *location = conf;
  ngx_http_laghu_main_conf_t *main_conf =
      ngx_http_conf_get_module_main_conf(configuration, ngx_http_laghu_module);
  ngx_str_t *values = configuration->args->elts;
  laghu_config_setting core_setting;
  laghu_service_setting service_setting;
  laghu_service_diagnostic error = {0};
  char core_error[160U];
  (void)command;
  if (configuration->args->nelts == 2U) {
    if (location->core.mode != LAGHU_MODE_UNSET) return "is duplicate";
    if (ngx_strcmp(values[1].data, "on") == 0) {
      location->core.mode = LAGHU_MODE_ON;
      return NGX_CONF_OK;
    }
    if (ngx_strcmp(values[1].data, "off") == 0) {
      location->core.mode = LAGHU_MODE_OFF;
      return NGX_CONF_OK;
    }
    return "laghu expects on or off, or one setting and value";
  }
  service_setting = laghu_service_setting_find((const char *)values[1].data);
  if (service_setting == LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP) {
    if (configuration->args->nelts != 4U ||
        !laghu_service_config_apply_pair(&location->service, service_setting,
                                         (const char *)values[2].data,
                                         (const char *)values[3].data, &error))
      return ngx_http_laghu_service_error(configuration, &values[1], &error);
    return NGX_CONF_OK;
  }
  core_setting = laghu_config_setting_find((const char *)values[1].data);
  if (core_setting == LAGHU_CONFIG_SETTING_MAP_REWRITE_DOMAIN ||
      core_setting == LAGHU_CONFIG_SETTING_MAP_PROXY_DOMAIN ||
      core_setting == LAGHU_CONFIG_SETTING_SHARD_DOMAIN) {
    if (configuration->args->nelts != 4U ||
        !laghu_config_setting_apply_pair(
            &location->core, core_setting, (const char *)values[2].data,
            (const char *)values[3].data, core_error, sizeof(core_error))) {
      ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                         "invalid laghu %V: %s", &values[1], core_error);
      return NGX_CONF_ERROR;
    }
    return NGX_CONF_OK;
  }
  if (configuration->args->nelts != 3U)
    return "laghu setting expects one value";
  if (service_setting != LAGHU_SERVICE_SETTING_UNKNOWN) {
    laghu_service_config *target = ngx_http_laghu_rum_setting(service_setting)
                                       ? &main_conf->service
                                       : &location->service;
    if (ngx_http_laghu_rum_setting(service_setting) &&
        configuration->cmd_type != NGX_HTTP_MAIN_CONF)
      return "laghu rum_store settings are allowed only in the http context";
    if (!laghu_service_config_apply(target, service_setting,
                                    (const char *)values[2].data, &error))
      return ngx_http_laghu_service_error(configuration, &values[1], &error);
    return NGX_CONF_OK;
  }
  if (core_setting == LAGHU_CONFIG_SETTING_UNKNOWN)
    return "unsupported laghu command";
  if (configuration->args->nelts != 3U)
    return "laghu setting expects one value";
  if (!laghu_config_setting_apply(&location->core, core_setting,
                                  (const char *)values[2].data, core_error,
                                  sizeof(core_error))) {
    ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0, "invalid laghu %V: %s",
                       &values[1], core_error);
    return NGX_CONF_ERROR;
  }
  return NGX_CONF_OK;
}
