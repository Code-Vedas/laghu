// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>

#include "ngx_http_laghu_internal.h"

ngx_int_t ngx_http_laghu_variant_handler(ngx_http_request_t *request) {
  ngx_http_laghu_loc_conf_t *conf =
      ngx_http_get_module_loc_conf(request, ngx_http_laghu_module);
  ngx_int_t status = ngx_http_laghu_admin_endpoint(request, conf);

  if (status != NGX_DECLINED) return status;
  status = ngx_http_laghu_beacon_endpoint(request, conf);
  if (status != NGX_DECLINED) return status;
  return ngx_http_laghu_asset_endpoint(request, conf);
}
