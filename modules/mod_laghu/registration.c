// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "mod_laghu_internal.h"

module AP_MODULE_DECLARE_DATA laghu_module;

static bool laghu_apache_validate_directory_config(
    server_rec *server, ap_conf_vector_t *directory) {
  laghu_apache_config *parent =
      ap_get_module_config(server->module_config, &laghu_module);
  laghu_apache_config *child = ap_get_module_config(directory, &laghu_module);
  laghu_config core;
  laghu_service_config service;
  laghu_service_diagnostic diagnostic = {0};
  if (child == NULL) return true;
  if (parent == NULL ||
      !laghu_resource_rules_merge_valid(&parent->core, &child->core)) {
    ap_log_error(APLOG_MARK, APLOG_ERR, 0, server,
                 "Laghu directory configuration has invalid inherited rules");
    return false;
  }
  laghu_config_merge(&core, &parent->core, &child->core);
  if (!laghu_apache_service_resolve(&service, &parent->service, &child->service,
                                    &core, &diagnostic)) {
    ap_log_error(APLOG_MARK, APLOG_ERR, 0, server,
                 "Laghu directory configuration is invalid: %s",
                 diagnostic.message);
    return false;
  }
  return true;
}

static int laghu_apache_check_config(apr_pool_t *configuration_pool,
                                     apr_pool_t *log_pool,
                                     apr_pool_t *temporary_pool,
                                     server_rec *server) {
  server_rec *item;
  (void)configuration_pool;
  (void)log_pool;
  (void)temporary_pool;
  for (item = server; item != NULL; item = item->next) {
    const core_server_config *core =
        ap_get_core_module_config(item->module_config);
    apr_array_header_t *lists[2];
    size_t list_index;
    if (core == NULL) continue;
    lists[0] = core->sec_dir;
    lists[1] = core->sec_url;
    for (list_index = 0U; list_index < sizeof(lists) / sizeof(lists[0]);
         ++list_index) {
      ap_conf_vector_t **entries;
      int index;
      if (lists[list_index] == NULL) continue;
      entries = (ap_conf_vector_t **)lists[list_index]->elts;
      for (index = 0; index < lists[list_index]->nelts; ++index)
        if (!laghu_apache_validate_directory_config(item, entries[index]))
          return HTTP_INTERNAL_SERVER_ERROR;
    }
  }
  return OK;
}

int laghu_apache_post_config(apr_pool_t *configuration_pool,
                             apr_pool_t *log_pool, apr_pool_t *temporary_pool,
                             server_rec *server) {
  (void)log_pool;
  (void)temporary_pool;
  {
    server_rec *item;
    for (item = server; item != NULL; item = item->next) {
      laghu_apache_config *config =
          ap_get_module_config(item->module_config, &laghu_module);
      const core_server_config *core_server =
          ap_get_core_module_config(item->module_config);
      laghu_service_diagnostic diagnostic = {0};
      if (config != NULL) laghu_apache_service_defaults(&config->service);
      if (config != NULL &&
          (config->service.source_policy.mode == LAGHU_SOURCE_FILE_NATIVE ||
           config->service.source_policy.mode == LAGHU_SOURCE_FILE_BOTH) &&
          core_server != NULL && core_server->ap_document_root != NULL &&
          strlen(core_server->ap_document_root) <
              sizeof(config->service.source_policy.native_root))
        (void)snprintf(config->service.source_policy.native_root,
                       sizeof(config->service.source_policy.native_root), "%s",
                       core_server->ap_document_root);
      if (config == NULL ||
          !laghu_service_config_finalize(
              &config->service,
              &(laghu_service_finalize_options){
                  .native_file_loading = true,
                  .require_admin_authorization = true,
                  .respect_x_forwarded_proto =
                      config->core.respect_x_forwarded_proto == LAGHU_MODE_ON},
              &diagnostic)) {
        ap_log_error(
            APLOG_MARK, APLOG_ERR, 0, item,
            "Laghu service configuration is invalid: %s",
            config == NULL ? "missing configuration" : diagnostic.message);
        return HTTP_INTERNAL_SERVER_ERROR;
      }
      if (config->service.source_policy.mode != LAGHU_SOURCE_FILE_OFF) {
        if (!laghu_source_registry_publish(config->service.asset_upload_queue,
                                           &config->service.source_policy)) {
          ap_log_error(APLOG_MARK, APLOG_ERR, 0, item,
                       "Laghu direct file loading requires asset offload and "
                       "a writable source registry");
          return HTTP_INTERNAL_SERVER_ERROR;
        }
      }
    }
  }
  {
    laghu_apache_config empty;
    laghu_apache_config *server_config =
        ap_get_module_config(server->module_config, &laghu_module);
    laghu_apache_config *default_config =
        ap_get_module_config(server->lookup_defaults, &laghu_module);
    laghu_service_config operational;
    laghu_config core;
    laghu_service_diagnostic diagnostic = {0};
    if (server_config == NULL) {
      ap_log_error(APLOG_MARK, APLOG_ERR, 0, server,
                   "Laghu service configuration is missing");
      return HTTP_INTERNAL_SERVER_ERROR;
    }
    if (default_config == NULL) {
      memset(&empty, 0, sizeof(empty));
      laghu_config_init(&empty.core);
      laghu_service_config_init(&empty.service);
      default_config = &empty;
    }
    laghu_config_merge(&core, &server_config->core, &default_config->core);
    if (!laghu_apache_service_resolve(&operational, &server_config->service,
                                      &default_config->service, &core,
                                      &diagnostic)) {
      ap_log_error(APLOG_MARK, APLOG_ERR, 0, server,
                   "Laghu operational configuration is invalid: %s",
                   diagnostic.message);
      return HTTP_INTERNAL_SERVER_ERROR;
    }
    laghu_apache_operational_enabled =
        operational.metrics || operational.readiness;
    laghu_apache_operational_cache =
        apr_pstrdup(configuration_pool, operational.image_cache[0] != '\0'
                                            ? operational.image_cache
                                            : LAGHU_DEFAULT_CACHE);
  }
  (void)ap_method_register(configuration_pool, "PURGE");
  return OK;
}

static void laghu_apache_register(apr_pool_t *pool) {
  (void)pool;
  ap_register_output_filter(LAGHU_APACHE_FILTER,
                            laghu_apache_transaction_filter, NULL,
                            AP_FTYPE_RESOURCE);
  ap_hook_insert_filter(laghu_apache_insert_filter, NULL, NULL,
                        APR_HOOK_MIDDLE);
  ap_hook_handler(laghu_apache_variant_handler, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_child_init(laghu_apache_child_init, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_check_config(laghu_apache_check_config, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_post_config(laghu_apache_post_config, NULL, NULL, APR_HOOK_MIDDLE);
}

static const command_rec laghu_apache_commands[] = {
    AP_INIT_RAW_ARGS("Laghu", laghu_apache_command, NULL,
                     RSRC_CONF | ACCESS_CONF,
                     "Laghu On|Off or Laghu <Setting> <Value>"),
    {NULL}};

module AP_MODULE_DECLARE_DATA laghu_module = {
    STANDARD20_MODULE_STUFF,   laghu_apache_create_config,
    laghu_apache_merge_config, laghu_apache_create_server_config,
    laghu_apache_merge_config, laghu_apache_commands,
    laghu_apache_register};
