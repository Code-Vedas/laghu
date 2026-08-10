// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "mod_laghu_internal.h"

extern laghu_operational_registry laghu_apache_operational;

static bool laghu_apache_admin_authorized(request_rec *request,
                                          laghu_apache_config *config) {
  const char *provided =
      apr_table_get(request->headers_in, "X-Laghu-Purge-Token");
  bool peer_allowed = false;
  bool token_allowed = false;
  peer_allowed = laghu_apache_peer_matches(request, config->service.purge_allow,
                                           config->service.purge_allow_count);
  if (provided != NULL && config->service.purge_token_file[0] != '\0') {
    FILE *token_file = fopen(config->service.purge_token_file, "rb");
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
          (unsigned char)((token_index < length ? expected[token_index] : 0U) ^
                          (token_index < supplied
                               ? (unsigned char)provided[token_index]
                               : 0U));
    token_allowed =
        length >= 16U && length < sizeof(expected) && difference == 0U;
  }
  return peer_allowed && token_allowed;
}

static int laghu_apache_admin_write(
    request_rec *request, const laghu_http_administrative_response *response,
    const char *rendered) {
  if (response->content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_PROMETHEUS)
    ap_set_content_type(request, "text/plain; version=0.0.4; charset=utf-8");
  else if (response->content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_HTML)
    ap_set_content_type(request, "text/html; charset=utf-8");
  else
    ap_set_content_type(request, "application/json");
  ap_set_content_length(request, (apr_off_t)response->length);
  request->status = (int)response->status;
  return request->header_only ||
                 ap_rwrite(rendered, (int)response->length, request) >= 0
             ? OK
             : HTTP_INTERNAL_SERVER_ERROR;
}

int laghu_apache_admin_endpoint(request_rec *request,
                                laghu_apache_config *config) {
  laghu_http_administrative_options options;
  laghu_http_administrative_plan plan;
  laghu_buffer method = {NULL, 0U};
  laghu_buffer target = {NULL, 0U};
  if (request->method != NULL)
    method = (laghu_buffer){(const unsigned char *)request->method,
                            strlen(request->method)};
  if (request->unparsed_uri != NULL)
    target = (laghu_buffer){(const unsigned char *)request->unparsed_uri,
                            strlen(request->unparsed_uri)};
  laghu_http_administrative_options_init(&options);
  options.metrics_enabled = config->service.metrics;
  options.readiness_enabled = config->service.readiness;
  options.statistics_enabled = config->service.statistics;
  options.purge_method_enabled = config->service.purge_method;
  options.purge_query_enabled = config->service.purge_query;
  options.purge_query_get_only = false;
  if (!laghu_http_administrative_plan_build(&plan, method, target, &options) ||
      !plan.recognized)
    return DECLINED;
  if (plan.status == HTTP_NOT_FOUND) return HTTP_NOT_FOUND;

  if (plan.requires_authorization &&
      !laghu_apache_admin_authorized(request, config)) {
    apr_table_setn(request->headers_out, "Cache-Control", "no-store");
    ap_set_content_type(request, "application/json");
    request->status = HTTP_FORBIDDEN;
    ap_rputs("{\"status\":\"forbidden\"}", request);
    return OK;
  }
  apr_table_setn(request->headers_out, "Cache-Control", "no-store");
  ap_set_content_type(request, "application/json");
  if (!laghu_cache_backend_register_path(config->service.image_cache,
                                         &config->service.cache_limits)) {
    request->status = HTTP_SERVICE_UNAVAILABLE;
    ap_rputs("{\"status\":\"unavailable\"}", request);
    return OK;
  }
  if (plan.status != 0U) {
    if (plan.status == HTTP_BAD_REQUEST) {
      request->status = HTTP_BAD_REQUEST;
      ap_rputs("{\"status\":\"malformed\"}", request);
      return OK;
    }
    return (int)plan.status;
  }

  if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_STATS) {
    laghu_cache_stats stats = {0};
    laghu_http_administrative_response response;
    char *rendered = apr_palloc(request->pool, LAGHU_OPERATIONAL_RENDER_SIZE);
    if (rendered == NULL) return HTTP_INTERNAL_SERVER_ERROR;
    if (!laghu_cache_backend_health_path(config->service.image_cache, &stats))
      return HTTP_SERVICE_UNAVAILABLE;
    if (!laghu_http_administrative_render_stats(
            &config->service.cache_limits, &stats, rendered,
            LAGHU_OPERATIONAL_RENDER_SIZE, &response))
      return HTTP_INTERNAL_SERVER_ERROR;
    return laghu_apache_admin_write(request, &response, rendered);
  }
  if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_CONSOLE) {
    laghu_cache_stats stats = {0};
    laghu_operational_snapshot snapshot;
    laghu_operational_readiness readiness;
    laghu_http_administrative_response response;
    char *rendered = apr_palloc(request->pool, LAGHU_OPERATIONAL_RENDER_SIZE);
    if (rendered == NULL ||
        !laghu_cache_backend_health_path(config->service.image_cache, &stats) ||
        !laghu_operational_registry_snapshot(&laghu_apache_operational,
                                             &snapshot) ||
        !laghu_operational_readiness_evaluate(
            &snapshot, (uint64_t)apr_time_sec(apr_time_now()), true, true,
            config->service.readiness_strict, &readiness) ||
        !laghu_http_administrative_render_console(&stats, &readiness, rendered,
                                                  LAGHU_OPERATIONAL_RENDER_SIZE,
                                                  &response))
      return HTTP_SERVICE_UNAVAILABLE;
    return laghu_apache_admin_write(request, &response, rendered);
  }
  if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_HISTORY) {
    laghu_operational_snapshot snapshot;
    laghu_http_administrative_history_model history;
    laghu_http_administrative_response response;
    char *rendered = apr_palloc(request->pool, LAGHU_OPERATIONAL_RENDER_SIZE);
    if (rendered == NULL ||
        !laghu_operational_registry_snapshot(&laghu_apache_operational,
                                            &snapshot) ||
        !laghu_http_administrative_build_history_model(
            &snapshot, &plan.history_query, &history) ||
        !laghu_http_administrative_render_history(
            &history, rendered, LAGHU_OPERATIONAL_RENDER_SIZE, &response))
      return HTTP_SERVICE_UNAVAILABLE;
    return laghu_apache_admin_write(request, &response, rendered);
  }
  if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_EXPLAIN) {
    laghu_operational_snapshot snapshot;
    laghu_operational_readiness readiness;
    laghu_http_administrative_explain_model explain;
    laghu_http_administrative_response response;
    laghu_cache_stats stats = {0};
    char *rendered = apr_palloc(request->pool, LAGHU_OPERATIONAL_RENDER_SIZE);
    if (rendered == NULL ||
        !laghu_cache_backend_health_path(config->service.image_cache, &stats) ||
        !laghu_operational_registry_snapshot(&laghu_apache_operational,
                                            &snapshot) ||
        !laghu_operational_readiness_evaluate(
            &snapshot, (uint64_t)apr_time_sec(apr_time_now()), true, true,
            config->service.readiness_strict, &readiness) ||
        !laghu_http_administrative_build_explain_model(
            &plan.explain_query, &readiness, &stats, &explain) ||
        !laghu_http_administrative_render_explain(
            &explain, rendered, LAGHU_OPERATIONAL_RENDER_SIZE, &response))
      return HTTP_SERVICE_UNAVAILABLE;
    return laghu_apache_admin_write(request, &response, rendered);
  }
  if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_METRICS ||
      plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_READINESS) {
    laghu_operational_snapshot *snapshot;
    laghu_http_administrative_response response;
    char *rendered;
    uint64_t now = (uint64_t)apr_time_sec(apr_time_now());
    bool cache_ready = true;
    (void)laghu_operational_registry_heartbeat(&laghu_apache_operational, now,
                                               true, 0U, 0U);
    snapshot = apr_pcalloc(request->pool, sizeof(*snapshot));
    rendered = apr_palloc(request->pool, LAGHU_OPERATIONAL_RENDER_SIZE);
    if (snapshot == NULL || rendered == NULL) return HTTP_INTERNAL_SERVER_ERROR;
    if (!laghu_operational_registry_snapshot(&laghu_apache_operational,
                                             snapshot))
      return HTTP_SERVICE_UNAVAILABLE;
    if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_READINESS) {
      laghu_cache_stats stats = {0};
      cache_ready =
          laghu_cache_backend_health_path(config->service.image_cache, &stats);
      laghu_operational_registry_cache(&laghu_apache_operational, &stats);
    }
    if (!laghu_http_administrative_render_operational(
            &plan, snapshot, now, true, cache_ready,
            config->service.readiness_strict, rendered,
            LAGHU_OPERATIONAL_RENDER_SIZE, &response))
      return HTTP_INTERNAL_SERVER_ERROR;
    return laghu_apache_admin_write(request, &response, rendered);
  }
  if (plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_PURGE) {
    laghu_cache_purge_result purge_result;
    laghu_http_administrative_response response;
    char *rendered = apr_palloc(request->pool, LAGHU_OPERATIONAL_RENDER_SIZE);
    uint64_t matched = 0U;
    if (rendered == NULL) return HTTP_INTERNAL_SERVER_ERROR;
    purge_result = laghu_cache_backend_purge_url_path(
        config->service.image_cache, plan.normalized_path,
        (uint64_t)apr_time_sec(apr_time_now()), &matched);
    if (!laghu_http_administrative_render_purge(purge_result, matched, rendered,
                                                LAGHU_OPERATIONAL_RENDER_SIZE,
                                                &response))
      return HTTP_INTERNAL_SERVER_ERROR;
    return laghu_apache_admin_write(request, &response, rendered);
  }
  return HTTP_INTERNAL_SERVER_ERROR;
}
