// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "mod_laghu_internal.h"

extern laghu_operational_registry laghu_apache_operational;

int laghu_apache_admin_endpoint(request_rec *request,
                                laghu_apache_config *config) {
  bool purge_control = false;
  char normalized[LAGHU_RUNTIME_PATH_SIZE];
  bool normalized_ok = laghu_cache_source_normalize(
      request->unparsed_uri, normalized, sizeof(normalized), &purge_control);
  bool purge_request = strcmp(request->method, "PURGE") == 0 || purge_control ||
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
  if (purge_request || stats_request || metrics_request || readiness_request) {
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
      ap_rprintf(request, "{\"status\":\"%s\",\"matched_artifacts\":%llu}",
                 result == LAGHU_CACHE_PURGE_ACCEPTED ? "accepted" : "rejected",
                 (unsigned long long)matched);
      return OK;
    }
  }
  return DECLINED;
}
