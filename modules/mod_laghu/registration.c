// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "mod_laghu_internal.h"

module AP_MODULE_DECLARE_DATA laghu_module;

int laghu_apache_post_config(apr_pool_t *configuration_pool,
                             apr_pool_t *log_pool, apr_pool_t *temporary_pool,
                             server_rec *server) {
  (void)log_pool;
  (void)temporary_pool;
  {
    laghu_apache_config *server_config =
        ap_get_module_config(server->module_config, &laghu_module);
    laghu_apache_config *default_config =
        ap_get_module_config(server->lookup_defaults, &laghu_module);
    laghu_apache_config *operational_config =
        default_config != NULL ? default_config : server_config;
    laghu_apache_operational_enabled =
        operational_config != NULL &&
        (operational_config->metrics || operational_config->readiness);
    laghu_apache_operational_cache =
        operational_config != NULL && operational_config->image_cache != NULL
            ? operational_config->image_cache
            : (server_config != NULL && server_config->image_cache != NULL
                   ? server_config->image_cache
                   : LAGHU_DEFAULT_CACHE);
  }
  {
    server_rec *item;
    for (item = server; item != NULL; item = item->next) {
      laghu_apache_config *config =
          ap_get_module_config(item->module_config, &laghu_module);
      const core_server_config *core_server =
          ap_get_core_module_config(item->module_config);
      char error[160U];
      if (config != NULL &&
          (config->source_policy.mode == LAGHU_SOURCE_FILE_NATIVE ||
           config->source_policy.mode == LAGHU_SOURCE_FILE_BOTH) &&
          core_server != NULL && core_server->ap_document_root != NULL &&
          strlen(core_server->ap_document_root) <
              sizeof(config->source_policy.native_root))
        (void)snprintf(config->source_policy.native_root,
                       sizeof(config->source_policy.native_root), "%s",
                       core_server->ap_document_root);
      if (config == NULL ||
          !laghu_source_policy_validate(&config->source_policy, true, error,
                                        sizeof(error))) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, item,
                     "Laghu direct file configuration is invalid: %s",
                     config == NULL ? "missing configuration" : error);
        return HTTP_INTERNAL_SERVER_ERROR;
      }
      if (config->source_policy.mode != LAGHU_SOURCE_FILE_OFF) {
        if (!config->asset_offload_loaded ||
            config->asset_upload_queue == NULL ||
            !laghu_source_registry_publish(config->asset_upload_queue,
                                           &config->source_policy)) {
          ap_log_error(APLOG_MARK, APLOG_ERR, 0, item,
                       "Laghu direct file loading requires asset offload and "
                       "a writable source registry");
          return HTTP_INTERNAL_SERVER_ERROR;
        }
      }
    }
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
