// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>

#include "laghu/catalog.h"
#include "laghu/instrumentation.h"
#include "ngx_http_laghu_internal.h"

void ngx_http_laghu_beacon_body(ngx_http_request_t *request) {
  ngx_http_laghu_loc_conf_t *conf =
      ngx_http_get_module_loc_conf(request, ngx_http_laghu_module);
  laghu_image_beacon_record beacon;
  laghu_critical_css_beacon critical;
  laghu_instrumentation_beacon instrumentation;
  laghu_policy policy;
  laghu_runtime_queue_snapshot queue_snapshot;
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned char *body;
  size_t length = (size_t)request->headers_in.content_length_n;
  size_t offset = 0U;
  ngx_chain_t *chain;
  bool valid = false;
  body = ngx_pnalloc(request->pool, length);
  if (body != NULL && request->request_body != NULL) {
    for (chain = request->request_body->bufs; chain != NULL;
         chain = chain->next) {
      size_t part = (size_t)ngx_buf_size(chain->buf);
      if (!ngx_buf_in_memory(chain->buf) || part > length - offset) {
        offset = 0U;
        break;
      }
      ngx_memcpy(body + offset, chain->buf->pos, part);
      offset += part;
    }
    valid = offset == length &&
            laghu_resolve_config_policy(&conf->core, &policy) &&
            laghu_variant_key((laghu_buffer){NULL, 0U}, &policy, policy_key);
    if (valid &&
        request->uri.len == sizeof("/.laghu/beacon/instrumentation") - 1U)
      valid = laghu_runtime_parse_instrumentation_beacon(
                  (laghu_buffer){body, length}, &instrumentation) &&
              laghu_instrumentation_apply_beacon(
                  ngx_http_laghu_rum, conf->service.image_cache,
                  (uint64_t)ngx_time(), conf->core.image_metadata_ttl,
                  &instrumentation);
    else if (valid &&
             request->uri.len == sizeof("/.laghu/beacon/critical-css") - 1U)
      valid =
          laghu_runtime_parse_critical_css_beacon((laghu_buffer){body, length},
                                                  &critical) &&
          laghu_critical_css_apply_beacon(
              ngx_http_laghu_rum, conf->service.image_cache, policy_key,
              (uint64_t)ngx_time(), conf->core.image_metadata_ttl, &critical);
    else if (valid) {
      laghu_runtime_queue *queue = ngx_http_laghu_image_queue(conf);
      memset(&queue_snapshot, 0, sizeof(queue_snapshot));
      valid = laghu_runtime_parse_image_beacon((laghu_buffer){body, length},
                                               &beacon) &&
              ngx_http_laghu_queue_refresh(conf) && queue != NULL &&
              laghu_runtime_queue_snapshot_get(queue, &queue_snapshot) &&
              laghu_catalog_apply_beacon(
                  ngx_http_laghu_rum, conf->service.image_cache, policy_key,
                  queue_snapshot.capabilities, (uint64_t)ngx_time(),
                  conf->core.image_metadata_ttl, &beacon);
    }
  }
  request->headers_out.status =
      valid ? NGX_HTTP_NO_CONTENT : NGX_HTTP_BAD_REQUEST;
  request->headers_out.content_length_n = 0;
  ngx_http_finalize_request(request, ngx_http_send_header(request));
}
