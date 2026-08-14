// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "laghu/assets.h"
#include "laghu/cache.h"
#include "laghu/catalog.h"
#include "laghu/config.h"
#include "laghu/core.h"
#include "laghu/css.h"
#include "laghu/fonts.h"
#include "laghu/http.h"
#include "laghu/image.h"
#include "laghu/instrumentation.h"
#include "laghu/javascript.h"
#include "laghu/operational.h"
#include "laghu/queue.h"
#include "laghu/rum.h"
#include "laghu/source.h"
#include "laghu/types.h"
#include "ngx_http_laghu_internal.h"

static ngx_int_t ngx_http_laghu_filter_init(ngx_conf_t *configuration);

laghu_runtime_queue *ngx_http_laghu_image_queue(ngx_http_laghu_loc_conf_t *conf) {
  return conf != NULL && conf->runtime_queue_attached ? &conf->runtime_queue : NULL;
}

uint32_t ngx_http_laghu_image_queue_capabilities(const ngx_http_laghu_loc_conf_t *conf) {
  return conf != NULL && conf->runtime_queue_snapshot_ready ? conf->runtime_queue_capabilities : 0U;
}

laghu_runtime_queue *ngx_http_laghu_font_queue(ngx_http_laghu_loc_conf_t *conf) {
  return conf != NULL && conf->font_fetch_runtime_queue_attached && conf->service.font_providers != NULL
             ? &conf->font_fetch_runtime_queue
             : NULL;
}

laghu_runtime_queue *ngx_http_laghu_javascript_queue(ngx_http_laghu_loc_conf_t *conf) {
  return conf != NULL && conf->javascript_runtime_queue_attached ? &conf->javascript_runtime_queue : NULL;
}

laghu_runtime_queue *ngx_http_laghu_html_refresh_queue(ngx_http_laghu_loc_conf_t *conf) {
  return conf != NULL && conf->html_refresh_runtime_queue_attached ? &conf->html_refresh_runtime_queue : NULL;
}

laghu_runtime_queue *ngx_http_laghu_chrome_analysis_queue(ngx_http_laghu_loc_conf_t *conf) {
  return conf != NULL && conf->chrome_analysis_runtime_queue_attached ? &conf->chrome_analysis_runtime_queue : NULL;
}

bool ngx_http_laghu_font_queue_refresh(ngx_http_laghu_loc_conf_t *conf) { return ngx_http_laghu_font_queue(conf) != NULL; }

bool ngx_http_laghu_javascript_queue_refresh(ngx_http_laghu_loc_conf_t *conf) { return ngx_http_laghu_javascript_queue(conf) != NULL; }

static ngx_command_t ngx_http_laghu_commands[] = {
    {ngx_string("laghu"), NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1 | NGX_CONF_TAKE2 | NGX_CONF_TAKE3,
     ngx_http_laghu_command, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL},
    ngx_null_command};

static ngx_http_module_t ngx_http_laghu_module_context = {NULL, ngx_http_laghu_filter_init,     ngx_http_laghu_create_main_conf, NULL, NULL,
                                                          NULL, ngx_http_laghu_create_loc_conf, ngx_http_laghu_merge_loc_conf};

ngx_module_t ngx_http_laghu_module = {NGX_MODULE_V1,
                                      &ngx_http_laghu_module_context,
                                      ngx_http_laghu_commands,
                                      NGX_HTTP_MODULE,
                                      NULL,
                                      NULL,
                                      ngx_http_laghu_init_process,
                                      NULL,
                                      NULL,
                                      ngx_http_laghu_exit_process,
                                      NULL,
                                      NGX_MODULE_V1_PADDING};

bool ngx_http_laghu_queue_refresh(ngx_http_laghu_loc_conf_t *conf) {
  return ngx_http_laghu_image_queue_capabilities(conf) != 0U;
}

static ngx_int_t ngx_http_laghu_filter_init(ngx_conf_t *configuration) {
  ngx_http_core_main_conf_t *core;
  ngx_http_handler_pt *handler;

  core = ngx_http_conf_get_module_main_conf(configuration, ngx_http_core_module);
  handler = ngx_array_push(&core->phases[NGX_HTTP_CONTENT_PHASE].handlers);
  if (handler == NULL) {
    return NGX_ERROR;
  }
  *handler = ngx_http_laghu_variant_handler;

  ngx_http_laghu_next_header_filter = ngx_http_top_header_filter;
  ngx_http_top_header_filter = ngx_http_laghu_transaction_header_filter;

  ngx_http_laghu_next_body_filter = ngx_http_top_body_filter;
  ngx_http_top_body_filter = ngx_http_laghu_transaction_body_filter;

  return NGX_OK;
}
