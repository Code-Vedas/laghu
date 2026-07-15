#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "laghu/core.h"

typedef struct {
  laghu_config core;
} ngx_http_laghu_loc_conf_t;

static ngx_http_output_header_filter_pt ngx_http_laghu_next_header_filter;
static ngx_http_output_body_filter_pt ngx_http_laghu_next_body_filter;

static ngx_int_t ngx_http_laghu_header_filter(ngx_http_request_t *request);
static ngx_int_t ngx_http_laghu_body_filter(ngx_http_request_t *request,
                                            ngx_chain_t *chain);
static ngx_int_t ngx_http_laghu_filter_init(ngx_conf_t *configuration);
static void *ngx_http_laghu_create_loc_conf(ngx_conf_t *configuration);
static char *ngx_http_laghu_merge_loc_conf(ngx_conf_t *configuration,
                                           void *parent, void *child);
static char *ngx_http_laghu_command(ngx_conf_t *configuration,
                                    ngx_command_t *command, void *conf);

static ngx_command_t ngx_http_laghu_commands[] = {
    {ngx_string("laghu"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
         NGX_CONF_TAKE1 | NGX_CONF_TAKE2,
     ngx_http_laghu_command, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL},
    ngx_null_command};

static ngx_http_module_t ngx_http_laghu_module_context = {
    NULL,
    ngx_http_laghu_filter_init,
    NULL,
    NULL,
    NULL,
    NULL,
    ngx_http_laghu_create_loc_conf,
    ngx_http_laghu_merge_loc_conf};

ngx_module_t ngx_http_laghu_module = {NGX_MODULE_V1,
                                      &ngx_http_laghu_module_context,
                                      ngx_http_laghu_commands,
                                      NGX_HTTP_MODULE,
                                      NULL,
                                      NULL,
                                      NULL,
                                      NULL,
                                      NULL,
                                      NULL,
                                      NULL,
                                      NGX_MODULE_V1_PADDING};

static char *ngx_http_laghu_copy_string(ngx_http_request_t *request,
                                        const ngx_str_t *value) {
  u_char *copy;

  if (value == NULL || value->len == 0) {
    return NULL;
  }

  copy = ngx_pnalloc(request->pool, value->len + 1);
  if (copy == NULL) {
    return NULL;
  }

  ngx_memcpy(copy, value->data, value->len);
  copy[value->len] = '\0';
  return (char *)copy;
}

static char *ngx_http_laghu_copy_header_chain(ngx_http_request_t *request,
                                              ngx_table_elt_t *header) {
  ngx_table_elt_t *current;
  size_t count = 0;
  size_t length = 0;
  u_char *copy;
  u_char *cursor;

  for (current = header; current != NULL; current = current->next) {
    if (current->hash == 0) {
      continue;
    }
    length += current->value.len;
    ++count;
  }

  if (count == 0) {
    return NULL;
  }

  copy = ngx_pnalloc(request->pool, length + count);
  if (copy == NULL) {
    return NULL;
  }

  cursor = copy;
  count = 0;
  for (current = header; current != NULL; current = current->next) {
    if (current->hash == 0) {
      continue;
    }
    if (count != 0) {
      *cursor++ = ',';
    }
    cursor = ngx_cpymem(cursor, current->value.data, current->value.len);
    ++count;
  }
  *cursor = '\0';
  return (char *)copy;
}

static ngx_int_t ngx_http_laghu_add_status_header(ngx_http_request_t *request,
                                                  laghu_decision decision) {
  const char *status;
  ngx_table_elt_t *header;

  header = ngx_list_push(&request->headers_out.headers);
  if (header == NULL) {
    return NGX_ERROR;
  }

  status = laghu_decision_name(decision);
  header->hash = 1;
  ngx_str_set(&header->key, "X-Laghu");
  header->value.len = ngx_strlen(status);
  header->value.data = (u_char *)status;
  return NGX_OK;
}

static ngx_int_t ngx_http_laghu_header_filter(ngx_http_request_t *request) {
  ngx_http_laghu_loc_conf_t *conf;
  laghu_response response;
  laghu_decision decision;

  conf = ngx_http_get_module_loc_conf(request, ngx_http_laghu_module);
  if (conf->core.mode != LAGHU_MODE_ON) {
    return ngx_http_laghu_next_header_filter(request);
  }

  response.status = (unsigned int)request->headers_out.status;
  response.content_type =
      ngx_http_laghu_copy_string(request, &request->headers_out.content_type);
  response.cache_control = ngx_http_laghu_copy_header_chain(
      request, request->headers_out.cache_control);
  response.has_authorization = request->headers_in.authorization != NULL;

  if (request->headers_out.cache_control != NULL &&
      response.cache_control == NULL) {
    decision = LAGHU_DECISION_BYPASS_ERROR;
  } else {
    decision = laghu_decide(&conf->core, &response);
  }
  if (ngx_http_laghu_add_status_header(request, decision) != NGX_OK) {
    ngx_log_error(
        NGX_LOG_WARN, request->connection->log, 0,
        "laghu could not allocate its response status header; serving the "
        "original response");
  }

  return ngx_http_laghu_next_header_filter(request);
}

static ngx_int_t ngx_http_laghu_body_filter(ngx_http_request_t *request,
                                            ngx_chain_t *chain) {
  /*
   * The initial module is intentionally a pass-through. This hook is the seam
   * for buffered transforms and worker-backed cache delivery. It must remain
   * fail-open: an unavailable optimizer never prevents the original chain from
   * reaching the next filter.
   */
  return ngx_http_laghu_next_body_filter(request, chain);
}

static ngx_int_t ngx_http_laghu_filter_init(ngx_conf_t *configuration) {
  (void)configuration;

  ngx_http_laghu_next_header_filter = ngx_http_top_header_filter;
  ngx_http_top_header_filter = ngx_http_laghu_header_filter;

  ngx_http_laghu_next_body_filter = ngx_http_top_body_filter;
  ngx_http_top_body_filter = ngx_http_laghu_body_filter;

  return NGX_OK;
}

static void *ngx_http_laghu_create_loc_conf(ngx_conf_t *configuration) {
  ngx_http_laghu_loc_conf_t *conf;

  conf = ngx_pcalloc(configuration->pool, sizeof(ngx_http_laghu_loc_conf_t));
  if (conf == NULL) {
    return NULL;
  }

  laghu_config_init(&conf->core);
  return conf;
}

static char *ngx_http_laghu_merge_loc_conf(ngx_conf_t *configuration,
                                           void *parent, void *child) {
  ngx_http_laghu_loc_conf_t *parent_conf = parent;
  ngx_http_laghu_loc_conf_t *child_conf = child;
  laghu_config merged;

  (void)configuration;

  laghu_config_merge(&merged, &parent_conf->core, &child_conf->core);
  child_conf->core = merged;
  return NGX_CONF_OK;
}

static char *ngx_http_laghu_command(ngx_conf_t *configuration,
                                    ngx_command_t *command, void *conf) {
  ngx_http_laghu_loc_conf_t *location = conf;
  ngx_str_t *values = configuration->args->elts;

  (void)command;

  if (configuration->args->nelts == 2) {
    if (location->core.mode != LAGHU_MODE_UNSET) {
      return "is duplicate";
    }

    if (ngx_strcmp(values[1].data, "on") == 0) {
      location->core.mode = LAGHU_MODE_ON;
      return NGX_CONF_OK;
    }

    if (ngx_strcmp(values[1].data, "off") == 0) {
      location->core.mode = LAGHU_MODE_OFF;
      return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                       "laghu expects 'on', 'off', or 'preset <name>'");
    return NGX_CONF_ERROR;
  }

  if (ngx_strcmp(values[1].data, "preset") == 0) {
    laghu_preset preset;

    if (location->core.preset != LAGHU_PRESET_UNSET) {
      return "is duplicate";
    }

    if (laghu_parse_preset((const char *)values[2].data, &preset)) {
      location->core.preset = preset;
      return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                       "unknown laghu preset \"%V\"", &values[2]);
    return NGX_CONF_ERROR;
  }

  ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                     "unsupported laghu command \"%V\"", &values[1]);
  return NGX_CONF_ERROR;
}
