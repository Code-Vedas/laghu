// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "mod_laghu_internal.h"

static apr_time_t laghu_apache_beacon_window;
static unsigned int laghu_apache_beacon_count;

static bool laghu_apache_backend_available(laghu_apache_config *config) {
  uint64_t now = (uint64_t)apr_time_sec(apr_time_now());
  laghu_runtime_queue_snapshot snapshot;
  if (!laghu_runtime_queue_snapshot_get(&config->queue, &snapshot) &&
      (!laghu_runtime_queue_open(&config->queue,
                                 config->service.worker_queue[0] != '\0'
                                     ? config->service.worker_queue
                                     : LAGHU_DEFAULT_QUEUE) ||
       !laghu_runtime_queue_snapshot_get(&config->queue, &snapshot)))
    return false;
  return snapshot.capabilities != 0U && snapshot.worker_heartbeat != 0U &&
         snapshot.worker_heartbeat <= now &&
         now - snapshot.worker_heartbeat <= 45U;
}

int laghu_apache_beacon_endpoint(request_rec *request,
                                 laghu_apache_config *config) {
  laghu_http_beacon_options options;
  laghu_http_beacon_plan plan;
  laghu_buffer method = {NULL, 0U};
  laghu_buffer target = {NULL, 0U};
  {
    int admin_status = laghu_apache_admin_endpoint(request, config);
    if (admin_status != DECLINED) return admin_status;
  }
  if (request->method != NULL)
    method = (laghu_buffer){(const unsigned char *)request->method,
                            strlen(request->method)};
  if (request->uri != NULL)
    target = (laghu_buffer){(const unsigned char *)request->uri,
                            strlen(request->uri)};
  laghu_http_beacon_options_init(&options);
  options.image_enabled = config->core.image_beacon == LAGHU_MODE_ON;
  options.critical_css_enabled =
      config->core.critical_css_beacon == LAGHU_MODE_ON;
  options.instrumentation_enabled =
      config->core.instrumentation_beacon == LAGHU_MODE_ON;
  if (!laghu_http_beacon_plan_build(&plan, method, target, &options) ||
      !plan.recognized)
    return DECLINED;
  if (plan.status != 0U) return (int)plan.status;

  if (plan.route == LAGHU_HTTP_BEACON_ROUTE_IMAGE_SCRIPT) {
    const char *script = laghu_runtime_image_beacon_script();
    size_t length = strlen(script);
    ap_set_content_type(request, "application/javascript");
    ap_set_content_length(request, (apr_off_t)length);
    return request->header_only || ap_rwrite(script, length, request) >= 0
               ? OK
               : HTTP_INTERNAL_SERVER_ERROR;
  }
  if (plan.route == LAGHU_HTTP_BEACON_ROUTE_IMAGE_REPORT) {
    const char *type = apr_table_get(request->headers_in, "Content-Type");
    const char *site = apr_table_get(request->headers_in, "Sec-Fetch-Site");
    const char *content_length =
        apr_table_get(request->headers_in, "Content-Length");
    char body_buffer[16385U];
    long length;
    long total = 0;
    laghu_image_beacon_record beacon;
    laghu_runtime_queue_snapshot queue_snapshot;
    laghu_policy policy;
    char policy_key[LAGHU_RUNTIME_KEY_SIZE];
    apr_time_t now = apr_time_sec(apr_time_now());
    char *content_length_end = NULL;
    unsigned long declared_length =
        content_length != NULL
            ? strtoul(content_length, &content_length_end, 10)
            : 0U;
    if (type == NULL || ap_cstr_casecmpn(type, "application/json", 16U) != 0 ||
        site == NULL || ap_cstr_casecmp(site, "same-origin") != 0 ||
        content_length == NULL || content_length_end == content_length ||
        *content_length_end != '\0' || declared_length == 0U ||
        declared_length > 16384U ||
        ap_setup_client_block(request, REQUEST_CHUNKED_ERROR) != OK ||
        !ap_should_client_block(request)) {
      return HTTP_BAD_REQUEST;
    }
    if (laghu_apache_beacon_window != now) {
      laghu_apache_beacon_window = now;
      laghu_apache_beacon_count = 0U;
    }
    if (++laghu_apache_beacon_count > 32U) {
      return HTTP_TOO_MANY_REQUESTS;
    }
    while ((length = ap_get_client_block(request, body_buffer + total,
                                         16384U - (size_t)total)) > 0) {
      total += length;
      if (total > 16384) {
        return HTTP_REQUEST_ENTITY_TOO_LARGE;
      }
    }
    if (length < 0 ||
        !laghu_runtime_parse_image_beacon(
            (laghu_buffer){(const unsigned char *)body_buffer, (size_t)total},
            &beacon) ||
        !laghu_resolve_config_policy(&config->core, &policy) ||
        !laghu_variant_key((laghu_buffer){NULL, 0U}, &policy, policy_key) ||
        !laghu_apache_backend_available(config) ||
        !laghu_runtime_queue_snapshot_get(&config->queue, &queue_snapshot) ||
        !laghu_catalog_apply_beacon(
            laghu_apache_rum,
            config->service.image_cache[0] != '\0' ? config->service.image_cache
                                                   : LAGHU_DEFAULT_CACHE,
            policy_key, queue_snapshot.capabilities, (uint64_t)now,
            config->core.image_metadata_ttl, &beacon)) {
      return HTTP_BAD_REQUEST;
    }
    request->status = HTTP_NO_CONTENT;
    return OK;
  }
  if (plan.route == LAGHU_HTTP_BEACON_ROUTE_CRITICAL_CSS_SCRIPT) {
    const char *critical_script = laghu_runtime_critical_css_beacon_script();
    size_t length = strlen(critical_script);
    ap_set_content_type(request, "application/javascript");
    ap_set_content_length(request, (apr_off_t)length);
    return request->header_only ||
                   ap_rwrite(critical_script, length, request) >= 0
               ? OK
               : HTTP_INTERNAL_SERVER_ERROR;
  }
  if (plan.route == LAGHU_HTTP_BEACON_ROUTE_CRITICAL_CSS_REPORT) {
    const char *type = apr_table_get(request->headers_in, "Content-Type");
    const char *site = apr_table_get(request->headers_in, "Sec-Fetch-Site");
    const char *content_length =
        apr_table_get(request->headers_in, "Content-Length");
    char body_buffer[16385U];
    long length;
    long total = 0;
    char *content_length_end = NULL;
    unsigned long declared_length =
        content_length != NULL
            ? strtoul(content_length, &content_length_end, 10)
            : 0U;
    laghu_critical_css_beacon critical;
    laghu_policy policy;
    char policy_key[LAGHU_RUNTIME_KEY_SIZE];
    uint64_t now = (uint64_t)apr_time_sec(apr_time_now());
    if (type == NULL || ap_cstr_casecmpn(type, "application/json", 16U) != 0 ||
        site == NULL || ap_cstr_casecmp(site, "same-origin") != 0 ||
        content_length == NULL || content_length_end == content_length ||
        *content_length_end != '\0' || declared_length == 0U ||
        declared_length > 16384U ||
        ap_setup_client_block(request, REQUEST_CHUNKED_ERROR) != OK ||
        !ap_should_client_block(request))
      return HTTP_BAD_REQUEST;
    if (laghu_apache_beacon_window != (apr_time_t)now) {
      laghu_apache_beacon_window = (apr_time_t)now;
      laghu_apache_beacon_count = 0U;
    }
    if (++laghu_apache_beacon_count > 32U) return HTTP_TOO_MANY_REQUESTS;
    while ((length = ap_get_client_block(request, body_buffer + total,
                                         16384U - (size_t)total)) > 0) {
      total += length;
      if (total > 16384) return HTTP_REQUEST_ENTITY_TOO_LARGE;
    }
    if (length < 0 ||
        !laghu_runtime_parse_critical_css_beacon(
            (laghu_buffer){(const unsigned char *)body_buffer, (size_t)total},
            &critical) ||
        !laghu_resolve_config_policy(&config->core, &policy) ||
        !laghu_variant_key((laghu_buffer){NULL, 0U}, &policy, policy_key) ||
        !laghu_critical_css_apply_beacon(
            laghu_apache_rum,
            config->service.image_cache[0] != '\0' ? config->service.image_cache
                                                   : LAGHU_DEFAULT_CACHE,
            policy_key, now, config->core.image_metadata_ttl, &critical))
      return HTTP_BAD_REQUEST;
    request->status = HTTP_NO_CONTENT;
    return OK;
  }
  if (plan.route == LAGHU_HTTP_BEACON_ROUTE_INSTRUMENTATION_SCRIPT) {
    const char *rum_script = laghu_runtime_instrumentation_script();
    size_t length = strlen(rum_script);
    ap_set_content_type(request, "application/javascript");
    ap_set_content_length(request, (apr_off_t)length);
    return request->header_only || ap_rwrite(rum_script, length, request) >= 0
               ? OK
               : HTTP_INTERNAL_SERVER_ERROR;
  }
  if (plan.route == LAGHU_HTTP_BEACON_ROUTE_INSTRUMENTATION_REPORT) {
    const char *type = apr_table_get(request->headers_in, "Content-Type");
    const char *site = apr_table_get(request->headers_in, "Sec-Fetch-Site");
    const char *content_length =
        apr_table_get(request->headers_in, "Content-Length");
    char body_buffer[16385U];
    char *content_length_end = NULL;
    unsigned long declared_length =
        content_length != NULL
            ? strtoul(content_length, &content_length_end, 10)
            : 0U;
    long length, total = 0;
    uint64_t now = (uint64_t)apr_time_sec(apr_time_now());
    laghu_instrumentation_beacon rum;
    if (type == NULL || ap_cstr_casecmpn(type, "application/json", 16U) != 0 ||
        site == NULL || ap_cstr_casecmp(site, "same-origin") != 0 ||
        content_length == NULL || content_length_end == content_length ||
        *content_length_end != '\0' || declared_length == 0U ||
        declared_length > 16384U ||
        ap_setup_client_block(request, REQUEST_CHUNKED_ERROR) != OK ||
        !ap_should_client_block(request))
      return HTTP_BAD_REQUEST;
    if (laghu_apache_beacon_window != (apr_time_t)now) {
      laghu_apache_beacon_window = (apr_time_t)now;
      laghu_apache_beacon_count = 0U;
    }
    if (++laghu_apache_beacon_count > 32U) return HTTP_TOO_MANY_REQUESTS;
    while ((length = ap_get_client_block(request, body_buffer + total,
                                         16384U - (size_t)total)) > 0) {
      total += length;
      if (total > 16384) return HTTP_REQUEST_ENTITY_TOO_LARGE;
    }
    if (length < 0 ||
        !laghu_runtime_parse_instrumentation_beacon(
            (laghu_buffer){(const unsigned char *)body_buffer, (size_t)total},
            &rum) ||
        !laghu_instrumentation_apply_beacon(
            laghu_apache_rum,
            config->service.image_cache[0] != '\0' ? config->service.image_cache
                                                   : LAGHU_DEFAULT_CACHE,
            now, config->core.image_metadata_ttl, &rum))
      return HTTP_BAD_REQUEST;
    request->status = HTTP_NO_CONTENT;
    return OK;
  }

  return DECLINED;
}
