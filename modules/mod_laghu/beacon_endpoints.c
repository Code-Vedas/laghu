// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "mod_laghu_internal.h"

static apr_time_t laghu_apache_beacon_window;
static unsigned int laghu_apache_beacon_count;

static bool laghu_apache_backend_available(laghu_apache_config *config) {
  uint64_t now = (uint64_t)apr_time_sec(apr_time_now());
  if (config->queue.mapping == NULL &&
      !laghu_runtime_queue_open(&config->queue, config->worker_queue != NULL
                                                    ? config->worker_queue
                                                    : LAGHU_DEFAULT_QUEUE))
    return false;
  return laghu_runtime_queue_refresh(&config->queue) &&
         config->queue.capabilities != 0U &&
         config->queue.worker_heartbeat != 0U &&
         config->queue.worker_heartbeat <= now &&
         now - config->queue.worker_heartbeat <= 45U;
}

int laghu_apache_beacon_endpoint(request_rec *request,
                                 laghu_apache_config *config) {
  static const char script_path[] = "/.laghu/beacon/images.js";
  static const char post_path[] = "/.laghu/beacon/images";
  static const char critical_script_path[] = "/.laghu/beacon/critical-css.js";
  static const char critical_post_path[] = "/.laghu/beacon/critical-css";
  static const char instrumentation_script_path[] =
      "/.laghu/beacon/instrumentation.js";
  static const char instrumentation_post_path[] =
      "/.laghu/beacon/instrumentation";
  static const char script[] =
      "addEventListener('load',()=>{document.querySelectorAll('img[src]')."
      "forEach(i=>{const "
      "r=i.getBoundingClientRect();if(r.width<1||r.height<1)return;fetch('/"
      ".laghu/beacon/"
      "images',{method:'POST',headers:{'Content-Type':'application/"
      "json'},body:JSON.stringify({url:new "
      "URL(i.currentSrc||i.src,location.href).pathname,width:Math.round(r."
      "width),height:Math.round(r.height),viewport_width:innerWidth,dpr_"
      "hundredths:Math.min(400,Math.max(100,Math.round(devicePixelRatio*100))),"
      "above_fold:r.top<innerHeight,mobile:innerWidth<768}),keepalive:true})})}"
      ");";
  {
    int admin_status = laghu_apache_admin_endpoint(request, config);
    if (admin_status != DECLINED) return admin_status;
  }
  if (strcmp(request->uri, script_path) == 0) {
    if (config->core.image_beacon != LAGHU_MODE_ON ||
        request->method_number != M_GET) {
      return HTTP_NOT_FOUND;
    }
    ap_set_content_type(request, "application/javascript");
    ap_set_content_length(request, (apr_off_t)(sizeof(script) - 1U));
    return request->header_only ||
                   ap_rwrite(script, sizeof(script) - 1U, request) >= 0
               ? OK
               : HTTP_INTERNAL_SERVER_ERROR;
  }
  if (strcmp(request->uri, post_path) == 0) {
    const char *type = apr_table_get(request->headers_in, "Content-Type");
    const char *site = apr_table_get(request->headers_in, "Sec-Fetch-Site");
    const char *content_length =
        apr_table_get(request->headers_in, "Content-Length");
    char body_buffer[16385U];
    long length;
    long total = 0;
    laghu_image_beacon_record beacon;
    laghu_policy policy;
    char policy_key[LAGHU_RUNTIME_KEY_SIZE];
    apr_time_t now = apr_time_sec(apr_time_now());
    char *content_length_end = NULL;
    unsigned long declared_length =
        content_length != NULL
            ? strtoul(content_length, &content_length_end, 10)
            : 0U;
    if (config->core.image_beacon != LAGHU_MODE_ON ||
        request->method_number != M_POST || type == NULL ||
        ap_cstr_casecmpn(type, "application/json", 16U) != 0 || site == NULL ||
        ap_cstr_casecmp(site, "same-origin") != 0 || content_length == NULL ||
        content_length_end == content_length || *content_length_end != '\0' ||
        declared_length == 0U || declared_length > 16384U ||
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
        !laghu_catalog_apply_beacon(
            laghu_apache_rum,
            config->image_cache != NULL ? config->image_cache
                                        : LAGHU_DEFAULT_CACHE,
            policy_key, config->queue.capabilities, (uint64_t)now,
            config->core.image_metadata_ttl, &beacon)) {
      return HTTP_BAD_REQUEST;
    }
    request->status = HTTP_NO_CONTENT;
    return OK;
  }
  if (strcmp(request->uri, critical_script_path) == 0) {
    const char *critical_script = laghu_runtime_critical_css_beacon_script();
    size_t length = strlen(critical_script);
    if (config->core.critical_css_beacon != LAGHU_MODE_ON ||
        request->method_number != M_GET)
      return HTTP_NOT_FOUND;
    ap_set_content_type(request, "application/javascript");
    ap_set_content_length(request, (apr_off_t)length);
    return request->header_only ||
                   ap_rwrite(critical_script, length, request) >= 0
               ? OK
               : HTTP_INTERNAL_SERVER_ERROR;
  }
  if (strcmp(request->uri, critical_post_path) == 0) {
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
    if (config->core.critical_css_beacon != LAGHU_MODE_ON ||
        request->method_number != M_POST || type == NULL ||
        ap_cstr_casecmpn(type, "application/json", 16U) != 0 || site == NULL ||
        ap_cstr_casecmp(site, "same-origin") != 0 || content_length == NULL ||
        content_length_end == content_length || *content_length_end != '\0' ||
        declared_length == 0U || declared_length > 16384U ||
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
            config->image_cache != NULL ? config->image_cache
                                        : LAGHU_DEFAULT_CACHE,
            policy_key, now, config->core.image_metadata_ttl, &critical))
      return HTTP_BAD_REQUEST;
    request->status = HTTP_NO_CONTENT;
    return OK;
  }
  if (strcmp(request->uri, instrumentation_script_path) == 0) {
    const char *rum_script = laghu_runtime_instrumentation_script();
    size_t length = strlen(rum_script);
    if (config->core.instrumentation_beacon != LAGHU_MODE_ON ||
        request->method_number != M_GET)
      return HTTP_NOT_FOUND;
    ap_set_content_type(request, "application/javascript");
    ap_set_content_length(request, (apr_off_t)length);
    return request->header_only || ap_rwrite(rum_script, length, request) >= 0
               ? OK
               : HTTP_INTERNAL_SERVER_ERROR;
  }
  if (strcmp(request->uri, instrumentation_post_path) == 0) {
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
    if (config->core.instrumentation_beacon != LAGHU_MODE_ON ||
        request->method_number != M_POST || type == NULL ||
        ap_cstr_casecmpn(type, "application/json", 16U) != 0 || site == NULL ||
        ap_cstr_casecmp(site, "same-origin") != 0 || content_length == NULL ||
        content_length_end == content_length || *content_length_end != '\0' ||
        declared_length == 0U || declared_length > 16384U ||
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
            config->image_cache != NULL ? config->image_cache
                                        : LAGHU_DEFAULT_CACHE,
            now, config->core.image_metadata_ttl, &rum))
      return HTTP_BAD_REQUEST;
    request->status = HTTP_NO_CONTENT;
    return OK;
  }

  return DECLINED;
}
