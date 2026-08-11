// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "mod_laghu_internal.h"

static bool laghu_apache_administration_candidate(
    request_rec *request, laghu_apache_config *config) {
  laghu_http_administrative_options options;
  laghu_http_administrative_plan plan;
  laghu_buffer method = {NULL, 0U};
  laghu_buffer target = {NULL, 0U};
  if (request == NULL || config == NULL || request->unparsed_uri == NULL ||
      request->method == NULL) {
    return false;
  }
  if (strcmp(request->method, "PURGE") == 0 ||
      strstr(request->unparsed_uri, "laghu=purge") != NULL) {
    return true;
  }
  laghu_http_administrative_options_init(&options);
  options.metrics_enabled = config->service.metrics;
  options.readiness_enabled = config->service.readiness;
  options.statistics_enabled = config->service.statistics;
  options.purge_method_enabled = config->service.purge_method;
  options.purge_query_enabled = config->service.purge_query;
  options.purge_query_get_only = false;
  method = (laghu_buffer){(const unsigned char *)request->method,
                          strlen(request->method)};
  target = (laghu_buffer){(const unsigned char *)request->unparsed_uri,
                          strlen(request->unparsed_uri)};
  if (!laghu_http_administrative_plan_build(&plan, method, target, &options)) {
    return false;
  }
  return plan.recognized;
}

static void laghu_apache_enqueue_html_refresh(
    laghu_apache_config *config, const char *request_path,
    const laghu_html_cache_record *record) {
  laghu_runtime_queue *queue;
  laghu_runtime_job job = {0};
  size_t length;
  if (config == NULL || request_path == NULL || record == NULL ||
      (length = strlen(request_path)) >= sizeof(job.request_path) ||
      !laghu_html_cache_key(config->core.html_cache_origin, request_path,
                            job.index_key)) {
    return;
  }
  queue = laghu_apache_html_refresh_queue(config);
  if (queue == NULL) return;
  job.kind = LAGHU_RUNTIME_JOB_HTML_REFRESH;
  memcpy(job.request_path, request_path, length);
  memcpy(job.policy_key, job.index_key, sizeof(job.policy_key));
  (void)snprintf(job.validator, sizeof(job.validator), "%s",
                 record->entry.validator);
  (void)laghu_apache_html_refresh_try_publish(
      config, &job, (uint64_t)apr_time_sec(apr_time_now()));
}

static int laghu_apache_html_cache_handler(request_rec *request,
                                           laghu_apache_config *config) {
  laghu_html_cache_record record;
  unsigned char *body;
  if (request == NULL || config == NULL || config->core.mode != LAGHU_MODE_ON ||
      request->method_number != M_GET || request->unparsed_uri == NULL ||
      request->unparsed_uri[0] != '/' ||
      strncmp(request->unparsed_uri, "/.laghu/", sizeof("/.laghu/") - 1U) ==
          0 ||
      apr_table_get(request->headers_in, "Authorization") != NULL ||
      apr_table_get(request->headers_in, "Cookie") != NULL ||
      config->core.html_cache_origin[0] == '\0' ||
      config->core.html_cache_ttl == LAGHU_HTML_CACHE_TTL_UNSET ||
      !laghu_html_cache_lookup(
          config->service.image_cache, config->core.html_cache_origin,
          request->unparsed_uri, (uint64_t)apr_time_sec(apr_time_now()),
          config->core.html_cache_ttl, config->core.html_cache_stale_ttl,
          &record) ||
      record.state == LAGHU_HTML_CACHE_MISS || record.entry.length == 0U ||
      record.entry.content_type[0] == '\0' || record.entry.length > INT_MAX) {
    return DECLINED;
  }
  if (record.state == LAGHU_HTML_CACHE_STALE)
    laghu_apache_enqueue_html_refresh(config, request->unparsed_uri, &record);
  body = apr_palloc(request->pool, record.entry.length);
  if (body == NULL ||
      !laghu_runtime_cache_read(&record.entry, body, record.entry.length)) {
    return DECLINED;
  }
  request->status = HTTP_OK;
  ap_set_content_type(request, record.entry.content_type);
  ap_set_content_length(request, (apr_off_t)record.entry.length);
  apr_table_setn(request->headers_out, "x-laghu-cache", "hit");
  if (request->header_only) return OK;
  return ap_rwrite(body, (int)record.entry.length, request) ==
                 (int)record.entry.length
             ? OK
             : HTTP_INTERNAL_SERVER_ERROR;
}

int laghu_apache_variant_handler(request_rec *request) {
  laghu_apache_config *server_config;
  laghu_apache_config *directory_config;
  laghu_apache_config *config;
  bool administration_candidate;
  if (request->uri == NULL) return DECLINED;
  server_config =
      ap_get_module_config(request->server->module_config, &laghu_module);
  directory_config =
      ap_get_module_config(request->per_dir_config, &laghu_module);
  config =
      laghu_apache_merge_config(request->pool, server_config, directory_config);
  administration_candidate = laghu_apache_administration_candidate(request, config);
  {
    int cache_status = laghu_apache_html_cache_handler(request, config);
    if (cache_status != DECLINED) return cache_status;
  }
  if (strncmp(request->uri, "/.laghu/", sizeof("/.laghu/") - 1U) != 0 &&
      !administration_candidate) {
    return DECLINED;
  }
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
