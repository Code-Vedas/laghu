// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "mod_laghu_internal.h"

int laghu_apache_variant_handler(request_rec *request) {
  laghu_apache_config *server_config;
  laghu_apache_config *directory_config;
  laghu_apache_config *config;
  bool administration_candidate =
      strcmp(request->method, "PURGE") == 0 ||
      (request->unparsed_uri != NULL &&
       strstr(request->unparsed_uri, "laghu=purge") != NULL) ||
      (request->uri != NULL && (strcmp(request->uri, "/.laghu/stats") == 0 ||
                                strcmp(request->uri, "/.laghu/metrics") == 0 ||
                                strcmp(request->uri, "/.laghu/ready") == 0));
  if (request->uri == NULL ||
      (strncmp(request->uri, "/.laghu/", sizeof("/.laghu/") - 1U) != 0 &&
       !administration_candidate)) {
    return DECLINED;
  }
  server_config =
      ap_get_module_config(request->server->module_config, &laghu_module);
  directory_config =
      ap_get_module_config(request->per_dir_config, &laghu_module);
  config =
      laghu_apache_merge_config(request->pool, server_config, directory_config);
  if (config == NULL || config->core.mode != LAGHU_MODE_ON) {
    return HTTP_NOT_FOUND;
  }
  if (config->service.cache_flush_file[0] != '\0')
    (void)laghu_cache_flush_file_poll(
        config->service.image_cache, config->service.cache_flush_file,
        (uint64_t)apr_time_sec(apr_time_now()), NULL);
  {
    int admin_status = laghu_apache_admin_endpoint(request, config);
    if (admin_status != DECLINED) return admin_status;
  }
  if (request->uri != NULL && strncmp(request->uri, "/.laghu/beacon/",
                                      sizeof("/.laghu/beacon/") - 1U) == 0) {
    int beacon_status = laghu_apache_beacon_endpoint(request, config);
    if (beacon_status != DECLINED) return beacon_status;
  }
  if (request->uri != NULL &&
      (strncmp(request->uri, "/.laghu/image/", sizeof("/.laghu/image/") - 1U) ==
           0 ||
       strncmp(request->uri, "/.laghu/css/", sizeof("/.laghu/css/") - 1U) ==
           0 ||
       strncmp(request->uri, "/.laghu/js/", sizeof("/.laghu/js/") - 1U) == 0 ||
       strncmp(request->uri, "/.laghu/media/", sizeof("/.laghu/media/") - 1U) ==
           0)) {
    return laghu_apache_asset_endpoint(request, config);
  }
  return HTTP_NOT_FOUND;
}
