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
                                                    : LAGHU_DEFAULT_QUEUE)) {
    return false;
  }
  return laghu_runtime_queue_refresh(&config->queue) &&
         config->queue.capabilities != 0U &&
         config->queue.worker_heartbeat != 0U &&
         config->queue.worker_heartbeat <= now &&
         now - config->queue.worker_heartbeat <= 45U;
}

int laghu_apache_variant_handler(request_rec *request) {
  static const char prefix[] = "/.laghu/image/";
  static const char css_prefix[] = "/.laghu/css/";
  static const char javascript_prefix[] = "/.laghu/js/";
  static const char media_prefix[] = "/.laghu/media/";
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
      "forEach"
      "(i=>{const r=i.getBoundingClientRect();if(r.width<1||r.height<1)return;"
      "fetch('/.laghu/beacon/images',{method:'POST',headers:{'Content-Type':"
      "'application/json'},body:JSON.stringify({url:new "
      "URL(i.currentSrc||i.src,"
      "location.href).pathname,width:Math.round(r.width),height:Math.round(r."
      "height),viewport_width:innerWidth,dpr_hundredths:Math.min(400,Math.max("
      "100,Math.round(devicePixelRatio*100))),above_fold:r.top<innerHeight,"
      "mobile:innerWidth<768}),keepalive:true})})});";
  laghu_apache_config *server_config;
  laghu_apache_config *directory_config;
  laghu_apache_config *config;
  laghu_runtime_cache_entry entry;
  unsigned char *body;
  const char *key;
  bool css_asset;
  bool javascript_asset;
  bool javascript_map;
  bool media_asset = false;
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
  if (config->cache_flush_file != NULL)
    (void)laghu_cache_flush_file_poll(
        config->image_cache, config->cache_flush_file,
        (uint64_t)apr_time_sec(apr_time_now()), NULL);
  {
    bool purge_control = false;
    char normalized[LAGHU_RUNTIME_PATH_SIZE];
    bool normalized_ok = laghu_cache_source_normalize(
        request->unparsed_uri, normalized, sizeof(normalized), &purge_control);
    bool purge_request = strcmp(request->method, "PURGE") == 0 ||
                         purge_control ||
                         (request->unparsed_uri != NULL &&
                          strstr(request->unparsed_uri, "laghu=purge") != NULL);
    bool stats_request =
        normalized_ok && strcmp(normalized, "/.laghu/stats") == 0;
    bool metrics_request =
        normalized_ok && strcmp(normalized, "/.laghu/metrics") == 0;
    bool readiness_request =
        normalized_ok && strcmp(normalized, "/.laghu/ready") == 0;
    if ((metrics_request && !config->metrics) ||
        (readiness_request && !config->readiness))
      return HTTP_NOT_FOUND;
    if (purge_request || stats_request || metrics_request ||
        readiness_request) {
      const char *provided =
          apr_table_get(request->headers_in, "X-Laghu-Purge-Token");
      bool peer_allowed = false, token_allowed = false;
      int subnet_index;
      if (config->purge_allow != NULL)
        for (subnet_index = 0; subnet_index < config->purge_allow->nelts;
             ++subnet_index)
          if (apr_ipsubnet_test(APR_ARRAY_IDX(config->purge_allow, subnet_index,
                                              apr_ipsubnet_t *),
                                request->connection->client_addr)) {
            peer_allowed = true;
            break;
          }
      if (provided != NULL && config->purge_token_file != NULL) {
        FILE *token_file = fopen(config->purge_token_file, "rb");
        char expected[257U];
        size_t length = token_file == NULL
                            ? 0U
                            : fread(expected, 1U, sizeof(expected), token_file);
        size_t supplied = strlen(provided), maximum, token_index;
        unsigned char difference = 0U;
        if (token_file != NULL) (void)fclose(token_file);
        while (length != 0U &&
               (expected[length - 1U] == '\n' || expected[length - 1U] == '\r'))
          --length;
        maximum = length > supplied ? length : supplied;
        difference = (unsigned char)(length ^ supplied);
        for (token_index = 0U; token_index < maximum; ++token_index)
          difference |=
              (unsigned char)((token_index < length ? expected[token_index]
                                                    : 0U) ^
                              (token_index < supplied
                                   ? (unsigned char)provided[token_index]
                                   : 0U));
        token_allowed =
            length >= 16U && length < sizeof(expected) && difference == 0U;
      }
      apr_table_setn(request->headers_out, "Cache-Control", "no-store");
      ap_set_content_type(request, "application/json");
      if (!normalized_ok) {
        request->status = HTTP_BAD_REQUEST;
        ap_rputs("{\"status\":\"malformed\"}", request);
        return OK;
      }
      if (!peer_allowed || !token_allowed) {
        request->status = HTTP_FORBIDDEN;
        ap_rputs("{\"status\":\"forbidden\"}", request);
        return OK;
      }
      if (!laghu_cache_backend_register_path(config->image_cache,
                                             &config->cache_limits)) {
        request->status = HTTP_SERVICE_UNAVAILABLE;
        ap_rputs("{\"status\":\"unavailable\"}", request);
        return OK;
      }
      if (stats_request) {
        laghu_cache_stats stats = {0};
        if (!config->statistics ||
            (request->method_number != M_GET && !request->header_only))
          return HTTP_METHOD_NOT_ALLOWED;
        if (!laghu_cache_backend_health_path(config->image_cache, &stats))
          return HTTP_SERVICE_UNAVAILABLE;
        ap_rprintf(
            request,
            "{\"schema\":\"laghu-cache-stats-v1\","
            "\"backend\":\"file\",\"bytes\":%llu,"
            "\"files\":%llu,\"hits\":%llu,\"misses\":%llu,"
            "\"publications\":%llu,\"rejected_writes\":%llu,"
            "\"evictions\":%llu,\"url_purges\":%llu,"
            "\"full_purges\":%llu,\"generation\":%llu}",
            (unsigned long long)stats.bytes, (unsigned long long)stats.files,
            (unsigned long long)stats.hits, (unsigned long long)stats.misses,
            (unsigned long long)stats.publications,
            (unsigned long long)stats.rejected_publications,
            (unsigned long long)stats.evictions,
            (unsigned long long)stats.url_purges,
            (unsigned long long)stats.full_purges,
            (unsigned long long)stats.cache_generation);
        return OK;
      }
      if (metrics_request || readiness_request) {
        laghu_operational_snapshot *snapshot;
        char *rendered;
        size_t length = 0U;
        bool enabled = metrics_request ? config->metrics : config->readiness;
        (void)laghu_operational_registry_heartbeat(
            &laghu_apache_operational, (uint64_t)apr_time_sec(apr_time_now()),
            true, 0U, 0U);
        if (!enabled ||
            (request->method_number != M_GET && !request->header_only))
          return HTTP_METHOD_NOT_ALLOWED;
        snapshot = apr_pcalloc(request->pool, sizeof(*snapshot));
        rendered = apr_palloc(request->pool, LAGHU_OPERATIONAL_RENDER_SIZE);
        if (snapshot == NULL || rendered == NULL)
          return HTTP_INTERNAL_SERVER_ERROR;
        if (!laghu_operational_registry_snapshot(&laghu_apache_operational,
                                                 snapshot))
          return HTTP_SERVICE_UNAVAILABLE;
        if (metrics_request) {
          if (!laghu_operational_render_prometheus(
                  snapshot, (uint64_t)apr_time_sec(apr_time_now()), rendered,
                  LAGHU_OPERATIONAL_RENDER_SIZE, &length))
            return HTTP_INTERNAL_SERVER_ERROR;
          ap_set_content_type(request,
                              "text/plain; version=0.0.4; charset=utf-8");
          ap_set_content_length(request, (apr_off_t)length);
          request->status = HTTP_OK;
          return request->header_only ||
                         ap_rwrite(rendered, (int)length, request) >= 0
                     ? OK
                     : HTTP_INTERNAL_SERVER_ERROR;
        } else {
          laghu_operational_readiness readiness;
          laghu_cache_stats stats = {0};
          bool cache_ready =
              laghu_cache_backend_health_path(config->image_cache, &stats);
          laghu_operational_registry_cache(&laghu_apache_operational, &stats);
          if (!laghu_operational_readiness_evaluate(
                  snapshot, (uint64_t)apr_time_sec(apr_time_now()), true,
                  cache_ready, config->readiness_strict, &readiness) ||
              !laghu_operational_render_readiness(
                  &readiness, config->readiness_strict, rendered,
                  LAGHU_OPERATIONAL_RENDER_SIZE, &length))
            return HTTP_INTERNAL_SERVER_ERROR;
          request->status = readiness.runtime_ready && readiness.cache_ready &&
                                    readiness.workers_ready
                                ? HTTP_OK
                                : HTTP_SERVICE_UNAVAILABLE;
          ap_set_content_length(request, (apr_off_t)length);
          return request->header_only ||
                         ap_rwrite(rendered, (int)length, request) >= 0
                     ? OK
                     : HTTP_INTERNAL_SERVER_ERROR;
        }
      }
      if ((strcmp(request->method, "PURGE") == 0 && !config->purge_method) ||
          (purge_control && !config->purge_query))
        return HTTP_METHOD_NOT_ALLOWED;
      {
        uint64_t matched = 0U;
        laghu_cache_purge_result result = laghu_cache_backend_purge_url_path(
            config->image_cache, normalized,
            (uint64_t)apr_time_sec(apr_time_now()), &matched);
        request->status = result == LAGHU_CACHE_PURGE_ACCEPTED
                              ? HTTP_ACCEPTED
                              : (result == LAGHU_CACHE_PURGE_SATURATED
                                     ? HTTP_TOO_MANY_REQUESTS
                                     : HTTP_SERVICE_UNAVAILABLE);
        ap_rprintf(
            request, "{\"status\":\"%s\",\"matched_artifacts\":%llu}",
            result == LAGHU_CACHE_PURGE_ACCEPTED ? "accepted" : "rejected",
            (unsigned long long)matched);
        return OK;
      }
    }
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
  css_asset = strlen(request->uri) ==
                  sizeof(css_prefix) - 1U + LAGHU_SHA256_HEX_LENGTH &&
              strncmp(request->uri, css_prefix, sizeof(css_prefix) - 1U) == 0;
  javascript_asset = strlen(request->uri) == sizeof(javascript_prefix) - 1U +
                                                 LAGHU_SHA256_HEX_LENGTH &&
                     strncmp(request->uri, javascript_prefix,
                             sizeof(javascript_prefix) - 1U) == 0;
  javascript_map =
      strlen(request->uri) ==
          sizeof(javascript_prefix) - 1U + LAGHU_SHA256_HEX_LENGTH + 4U &&
      strncmp(request->uri, javascript_prefix,
              sizeof(javascript_prefix) - 1U) == 0 &&
      strcmp(request->uri + strlen(request->uri) - 4U, ".map") == 0;
  media_asset =
      strlen(request->uri) ==
          sizeof(media_prefix) - 1U + LAGHU_SHA256_HEX_LENGTH &&
      strncmp(request->uri, media_prefix, sizeof(media_prefix) - 1U) == 0;
  if (!css_asset && !javascript_asset && !javascript_map && !media_asset &&
      (strlen(request->uri) != sizeof(prefix) - 1U + LAGHU_SHA256_HEX_LENGTH ||
       strncmp(request->uri, prefix, sizeof(prefix) - 1U) != 0)) {
    return HTTP_NOT_FOUND;
  }
  if (request->method_number != M_GET) {
    return HTTP_METHOD_NOT_ALLOWED;
  }
  key = request->uri + (css_asset ? sizeof(css_prefix) - 1U
                        : (javascript_asset || javascript_map)
                            ? sizeof(javascript_prefix) - 1U
                        : media_asset ? sizeof(media_prefix) - 1U
                                      : sizeof(prefix) - 1U);
  {
    size_t offset;
    for (offset = 0U; offset < LAGHU_SHA256_HEX_LENGTH; ++offset) {
      if (!((key[offset] >= '0' && key[offset] <= '9') ||
            (key[offset] >= 'a' && key[offset] <= 'f'))) {
        return HTTP_NOT_FOUND;
      }
    }
  }
  {
    laghu_apache_context transaction_context;
    laghu_http_transaction_result result;
    const laghu_http_header_operation *operation;
    size_t index;
    memset(&transaction_context, 0, sizeof(transaction_context));
    memset(&result, 0, sizeof(result));
    transaction_context.config = config;
    request->status = HTTP_OK;
    request->clength = 0;
    if (!laghu_apache_normalize(request, &transaction_context) ||
        !laghu_http_transaction_prepare(
            &transaction_context.transaction, &transaction_context.request,
            &transaction_context.response, &transaction_context.environment,
            &result) ||
        result.action != LAGHU_HTTP_ACTION_SERVE_CACHED ||
        result.selected.length == 0U ||
        result.selected.length > LAGHU_IMAGE_MAX_INPUT_BYTES) {
      laghu_http_transaction_result_release(&result);
      return HTTP_NOT_FOUND;
    }
    body = apr_pmemdup(request->pool, result.selected.data,
                       result.selected.length);
    if (body == NULL || !laghu_apache_apply_result(request, &result)) {
      laghu_http_transaction_result_release(&result);
      return HTTP_INTERNAL_SERVER_ERROR;
    }
    entry.length = result.selected.length;
    entry.content_type[0] = '\0';
    for (index = 0U; index < result.header_operation_count; ++index) {
      operation = &result.header_operations[index];
      if (strcmp(operation->name, "Content-Type") == 0 &&
          operation->value != NULL) {
        apr_cpystrn(entry.content_type, operation->value,
                    sizeof(entry.content_type));
      }
    }
    laghu_http_transaction_result_release(&result);
  }
  ap_set_content_type(request, css_asset        ? "text/css"
                               : javascript_map ? "application/json"
                                                : entry.content_type);
  ap_set_content_length(request, (apr_off_t)entry.length);
  apr_table_setn(request->headers_out, "Cache-Control",
                 "public, max-age=31536000, immutable");
  apr_table_set(request->headers_out, "ETag",
                apr_psprintf(request->pool, "\"%s\"", key));
  if (!request->header_only &&
      ap_rwrite(body, (int)entry.length, request) < 0) {
    return HTTP_INTERNAL_SERVER_ERROR;
  }
  return OK;
}
