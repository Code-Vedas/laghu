// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "laghu/config.h"
#include "mod_laghu_internal.h"

static apr_status_t laghu_apache_service_cleanup(void *data) {
  laghu_service_config_dispose(data);
  return APR_SUCCESS;
}

static bool laghu_apache_rum_setting(laghu_service_setting setting) {
  return setting >= LAGHU_SERVICE_SETTING_RUM_STORE &&
         setting <= LAGHU_SERVICE_SETTING_RUM_STORE_PENDING_LIMIT;
}

static bool laghu_apache_resource_setting(laghu_service_setting setting) {
  return setting == LAGHU_SERVICE_SETTING_FONT_PROVIDER_CONFIG ||
         setting == LAGHU_SERVICE_SETTING_JAVASCRIPT_OBSERVATION_CONFIG ||
         setting == LAGHU_SERVICE_SETTING_JAVASCRIPT_DEFER_CONFIG ||
         setting == LAGHU_SERVICE_SETTING_ASSET_OFFLOAD_CONFIG;
}

void laghu_apache_service_defaults(laghu_service_config *service) {
  if (service->worker_queue[0] == '\0')
    (void)snprintf(service->worker_queue, sizeof(service->worker_queue), "%s",
                   LAGHU_DEFAULT_QUEUE);
  if (service->font_provider_config[0] != '\0' &&
      service->font_fetch_queue[0] == '\0')
    (void)snprintf(service->font_fetch_queue, sizeof(service->font_fetch_queue),
                   "%s", LAGHU_DEFAULT_FONT_QUEUE);
  if (service->javascript_queue[0] == '\0')
    (void)snprintf(service->javascript_queue, sizeof(service->javascript_queue),
                   "%s", LAGHU_DEFAULT_JAVASCRIPT_QUEUE);
  if (service->image_cache[0] == '\0')
    (void)snprintf(service->image_cache, sizeof(service->image_cache), "%s",
                   LAGHU_DEFAULT_CACHE);
}

bool laghu_apache_service_resolve(laghu_service_config *resolved,
                                  const laghu_service_config *parent,
                                  const laghu_service_config *child,
                                  const laghu_config *core,
                                  laghu_service_diagnostic *diagnostic) {
  if (resolved == NULL || parent == NULL || child == NULL || core == NULL ||
      !laghu_service_config_merge(resolved, parent, child, diagnostic))
    return false;
  laghu_apache_service_defaults(resolved);
  /* This is deliberately the no-I/O half of shared finalization.  Apache
   * accepted and prepared file-backed resources while parsing directives, but
   * it merges directory configuration again for each request. */
  return laghu_service_config_validate(
      resolved,
      &(laghu_service_finalize_options){
          .native_file_loading = true,
          .require_admin_authorization = true,
          .respect_x_forwarded_proto =
              core->respect_x_forwarded_proto == LAGHU_MODE_ON},
      diagnostic);
}

void *laghu_apache_create_config(apr_pool_t *pool, char *path) {
  laghu_apache_config *config = apr_pcalloc(pool, sizeof(*config));
  (void)path;
  if (config == NULL) return NULL;
  laghu_config_init(&config->core);
  laghu_service_config_init(&config->service);
  apr_pool_cleanup_register(pool, &config->service,
                            laghu_apache_service_cleanup,
                            apr_pool_cleanup_null);
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
  laghu_service_diagnostic diagnostic = {0};
  if (merged == NULL || parent == NULL || child == NULL ||
      !laghu_resource_rules_merge_valid(&parent->core, &child->core))
    return NULL;
  laghu_config_merge(&merged->core, &parent->core, &child->core);
  if (!laghu_apache_service_resolve(&merged->service, &parent->service,
                                    &child->service, &merged->core,
                                    &diagnostic))
    return NULL;
  merged->queue_binding =
      laghu_apache_queue_binding_find_service(&merged->service);
  apr_pool_cleanup_register(pool, &merged->service,
                            laghu_apache_service_cleanup,
                            apr_pool_cleanup_null);
  return merged;
}

static const char *laghu_apache_service_error(
    cmd_parms *command, const laghu_service_diagnostic *error) {
  return apr_psprintf(command->pool, "invalid Laghu setting: %s",
                      error->message);
}

const char *laghu_apache_command(cmd_parms *command, void *value,
                                 const char *arguments) {
  laghu_apache_config *config = value;
  const char *cursor = arguments;
  const char *name = ap_getword_conf(command->pool, &cursor);
  const char *parameter = ap_getword_conf(command->pool, &cursor);
  const char *extra = ap_getword_conf(command->pool, &cursor);
  laghu_service_setting service_setting = laghu_service_setting_find(name);
  laghu_config_setting core_setting;
  laghu_service_diagnostic diagnostic = {0};
  char core_error[160U];
  char normalized[16U];
  if (service_setting == LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP) {
    if (command->path != NULL)
      return "Laghu FileSourceMap is allowed only in server configuration";
    if (parameter[0] == '\0' || extra[0] == '\0' || *cursor != '\0' ||
        !laghu_service_config_apply_pair(&config->service, service_setting,
                                         parameter, extra, &diagnostic))
      return laghu_apache_service_error(command, &diagnostic);
    return NULL;
  }
  if (name[0] == '\0') return "Laghu expects one setting";
  if (parameter[0] == '\0') {
    laghu_mode mode;
    if (ap_cstr_casecmp(name, "On") == 0)
      mode = LAGHU_MODE_ON;
    else if (ap_cstr_casecmp(name, "Off") == 0)
      mode = LAGHU_MODE_OFF;
    else
      return "Laghu expects On or Off, or one setting and value";
    if (config->core.mode != LAGHU_MODE_UNSET) return "Laghu is duplicate";
    config->core.mode = mode;
    return NULL;
  }
  if (extra[0] != '\0') return "Laghu expects one setting and one value";
  if (service_setting != LAGHU_SERVICE_SETTING_UNKNOWN) {
    const core_server_config *core_server;
    if (laghu_apache_rum_setting(service_setting) &&
        (command->path != NULL || command->server->is_virtual))
      return "Laghu RUM store settings are allowed only in the main server "
             "context";
    if (service_setting == LAGHU_SERVICE_SETTING_LOAD_FROM_FILE &&
        command->path != NULL)
      return "Laghu LoadFromFile is allowed only in server configuration";
    if (service_setting == LAGHU_SERVICE_SETTING_LOAD_FROM_FILE) {
      size_t index;
      if (strlen(parameter) >= sizeof(normalized))
        return "Laghu LoadFromFile expects Off, Mapped, Native, or Both";
      for (index = 0U; parameter[index] != '\0'; ++index)
        normalized[index] = (char)tolower((unsigned char)parameter[index]);
      normalized[index] = '\0';
      parameter = normalized;
    }
    if (!laghu_service_config_apply(&config->service, service_setting,
                                    parameter, &diagnostic))
      return laghu_apache_service_error(command, &diagnostic);
    if (laghu_apache_resource_setting(service_setting) &&
        !laghu_service_config_prepare_resources(&config->service, &diagnostic))
      return laghu_apache_service_error(command, &diagnostic);
    if (service_setting == LAGHU_SERVICE_SETTING_LOAD_FROM_FILE &&
        (config->service.source_policy.mode == LAGHU_SOURCE_FILE_NATIVE ||
         config->service.source_policy.mode == LAGHU_SOURCE_FILE_BOTH)) {
      core_server = ap_get_core_module_config(command->server->module_config);
      if (core_server == NULL || core_server->ap_document_root == NULL ||
          strlen(core_server->ap_document_root) >=
              sizeof(config->service.source_policy.native_root))
        return "Laghu native file loading requires an absolute document root";
      (void)snprintf(config->service.source_policy.native_root,
                     sizeof(config->service.source_policy.native_root), "%s",
                     core_server->ap_document_root);
    }
    return NULL;
  }
  core_setting = laghu_config_setting_find(name);
  if (core_setting == LAGHU_CONFIG_SETTING_UNKNOWN)
    return "unknown Laghu setting";
  if (!laghu_config_setting_apply(&config->core, core_setting, parameter,
                                  core_error, sizeof(core_error)))
    return apr_pstrdup(command->pool, core_error);
  return NULL;
}
