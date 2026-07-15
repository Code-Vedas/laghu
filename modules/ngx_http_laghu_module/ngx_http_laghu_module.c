// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "laghu/core.h"
#include "laghu/image.h"
#include "laghu/runtime.h"

typedef struct {
  laghu_config core;
  laghu_runtime_queue runtime_queue;
  ngx_str_t worker_queue;
  ngx_str_t image_cache;
} ngx_http_laghu_loc_conf_t;

typedef struct {
  laghu_policy policy;
  laghu_runtime_cache_entry cache_entry;
  char policy_key[LAGHU_SHA256_HEX_SIZE];
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  unsigned char *capture;
  size_t capture_length;
  size_t capture_capacity;
  unsigned char *cached_body;
  ngx_chain_t *cached_output;
  bool accept_webp;
  bool capture_enabled;
  bool cache_hit;
  bool cache_sent;
} ngx_http_laghu_request_ctx_t;

static ngx_http_output_header_filter_pt ngx_http_laghu_next_header_filter;
static ngx_http_output_body_filter_pt ngx_http_laghu_next_body_filter;
static ngx_uint_t ngx_http_laghu_backend_warning_emitted;
static time_t ngx_http_laghu_last_queue_warning;

static ngx_int_t ngx_http_laghu_header_filter(ngx_http_request_t *request);
static ngx_int_t ngx_http_laghu_body_filter(ngx_http_request_t *request,
                                            ngx_chain_t *chain);
static ngx_int_t ngx_http_laghu_filter_init(ngx_conf_t *configuration);
static void *ngx_http_laghu_create_loc_conf(ngx_conf_t *configuration);
static char *ngx_http_laghu_merge_loc_conf(ngx_conf_t *configuration,
                                           void *parent, void *child);
static char *ngx_http_laghu_command(ngx_conf_t *configuration,
                                    ngx_command_t *command, void *conf);

static void ngx_http_laghu_queue_cleanup(void *data) {
  laghu_runtime_queue_close(data);
}

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

#if nginx_version >= 1023000
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
#endif

#if nginx_version < 1023000
static char *ngx_http_laghu_copy_header_array(ngx_http_request_t *request,
                                              ngx_array_t *array) {
  ngx_table_elt_t *headers;
  size_t index;
  size_t count = 0;
  size_t length = 0;
  u_char *copy;
  u_char *cursor;

  if (array->elts == NULL || array->nelts == 0U) {
    return NULL;
  }
  headers = array->elts;
  for (index = 0; index < array->nelts; ++index) {
    if (headers[index].hash != 0U) {
      length += headers[index].value.len;
      ++count;
    }
  }
  if (count == 0U) {
    return NULL;
  }
  copy = ngx_pnalloc(request->pool, length + count);
  if (copy == NULL) {
    return NULL;
  }
  cursor = copy;
  count = 0U;
  for (index = 0; index < array->nelts; ++index) {
    if (headers[index].hash == 0U) {
      continue;
    }
    if (count != 0U) {
      *cursor++ = ',';
    }
    cursor =
        ngx_cpymem(cursor, headers[index].value.data, headers[index].value.len);
    ++count;
  }
  *cursor = '\0';
  return (char *)copy;
}
#endif

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

static bool ngx_http_laghu_accepts_webp(ngx_table_elt_t *header) {
  static const u_char media_type[] = "image/webp";

  while (header != NULL) {
    size_t start = 0U;
    while (start < header->value.len) {
      size_t end = start;
      size_t type_end;
      size_t parameter;
      bool accepted = true;

      while (end < header->value.len && header->value.data[end] != ',') {
        ++end;
      }
      while (start < end && (header->value.data[start] == ' ' ||
                             header->value.data[start] == '\t')) {
        ++start;
      }
      type_end = start;
      while (type_end < end && header->value.data[type_end] != ';' &&
             header->value.data[type_end] != ' ' &&
             header->value.data[type_end] != '\t') {
        ++type_end;
      }
      if (type_end - start == sizeof(media_type) - 1U &&
          ngx_strncasecmp(header->value.data + start, (u_char *)media_type,
                          sizeof(media_type) - 1U) == 0) {
        for (parameter = type_end; parameter + 2U < end; ++parameter) {
          if ((header->value.data[parameter] == 'q' ||
               header->value.data[parameter] == 'Q') &&
              header->value.data[parameter + 1U] == '=' &&
              (parameter == type_end ||
               header->value.data[parameter - 1U] == ';' ||
               header->value.data[parameter - 1U] == ' ' ||
               header->value.data[parameter - 1U] == '\t')) {
            size_t quality = parameter + 2U;
            accepted = false;
            while (quality < end && header->value.data[quality] != ';' &&
                   header->value.data[quality] != ' ' &&
                   header->value.data[quality] != '\t') {
              if (header->value.data[quality] >= '1' &&
                  header->value.data[quality] <= '9') {
                accepted = true;
              }
              ++quality;
            }
            break;
          }
        }
        if (accepted) {
          return true;
        }
      }
      start = end + 1U;
    }
#if nginx_version >= 1023000
    header = header->next;
#else
    break;
#endif
  }
  return false;
}

static bool ngx_http_laghu_is_image_type(const ngx_str_t *content_type) {
  return content_type != NULL && content_type->len >= 6U &&
         ngx_strncasecmp(content_type->data, (u_char *)"image/", 6U) == 0;
}

static laghu_image_filter_mask ngx_http_laghu_image_filters(
    const laghu_policy *policy) {
  laghu_image_filter_mask filters =
      LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_RECOMPRESS_IMAGES |
      LAGHU_IMAGE_RECOMPRESS_JPEG | LAGHU_IMAGE_RECOMPRESS_PNG |
      LAGHU_IMAGE_RECOMPRESS_WEBP;

  if ((policy->filter_families & LAGHU_FILTER_IMAGE_METADATA) != 0U) {
    filters |= LAGHU_IMAGE_STRIP_METADATA | LAGHU_IMAGE_STRIP_COLOR_PROFILE;
  }
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_MODERN) != 0U) {
    filters |= LAGHU_IMAGE_JPEG_PROGRESSIVE | LAGHU_IMAGE_JPEG_TO_WEBP |
               LAGHU_IMAGE_PNG_TO_JPEG | LAGHU_IMAGE_GIF_TO_PNG |
               LAGHU_IMAGE_TO_WEBP_LOSSLESS | LAGHU_IMAGE_TO_WEBP_ANIMATED |
               LAGHU_IMAGE_JPEG_SAMPLING | LAGHU_IMAGE_IN_PLACE_BROWSER;
  }
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_RESPONSIVE) != 0U) {
    filters |= LAGHU_IMAGE_RESIZE_ATTRIBUTE | LAGHU_IMAGE_RESIZE_RENDERED |
               LAGHU_IMAGE_RESIZE_MOBILE | LAGHU_IMAGE_RESPONSIVE |
               LAGHU_IMAGE_RESPONSIVE_ZOOM;
  }
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_DIMENSIONS) != 0U) {
    filters |= LAGHU_IMAGE_INSERT_DIMENSIONS;
  }
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_LAZYLOAD) != 0U) {
    filters |= LAGHU_IMAGE_LAZYLOAD | LAGHU_IMAGE_INLINE_PREVIEW;
  }
  if ((policy->filter_families & LAGHU_FILTER_RESOURCE_INLINE) != 0U) {
    filters |=
        LAGHU_IMAGE_INLINE | LAGHU_IMAGE_DEDUP_INLINE | LAGHU_IMAGE_SPRITE;
  }
  return filters;
}

static bool ngx_http_laghu_validator(
    ngx_http_request_t *request, char output[LAGHU_RUNTIME_VALIDATOR_SIZE]) {
  size_t length;

  if (request->headers_out.etag != NULL &&
      request->headers_out.etag->value.len > 0U &&
      !(request->headers_out.etag->value.len >= 2U &&
        (request->headers_out.etag->value.data[0] == 'W' ||
         request->headers_out.etag->value.data[0] == 'w') &&
        request->headers_out.etag->value.data[1] == '/') &&
      request->headers_out.etag->value.len < LAGHU_RUNTIME_VALIDATOR_SIZE) {
    length = request->headers_out.etag->value.len;
    ngx_memcpy(output, request->headers_out.etag->value.data, length);
    output[length] = '\0';
    return true;
  }
  output[0] = '\0';
  return false;
}

static bool ngx_http_laghu_type_equals(const ngx_str_t *content_type,
                                       const char *expected) {
  size_t length = ngx_strlen(expected);

  return content_type != NULL && content_type->len == length &&
         ngx_strncasecmp(content_type->data, (u_char *)expected, length) == 0;
}

static bool ngx_http_laghu_backend_supports(
    const ngx_str_t *content_type, laghu_image_filter_mask filters,
    laghu_image_capability_mask capabilities, bool allow_lossy) {
  if (ngx_http_laghu_type_equals(content_type, "image/jpeg")) {
    return allow_lossy && (capabilities & LAGHU_IMAGE_CAP_JPEG_LOAD) != 0U &&
           (((capabilities & LAGHU_IMAGE_CAP_JPEG_SAVE) != 0U &&
             (filters &
              (LAGHU_IMAGE_RECOMPRESS_IMAGES | LAGHU_IMAGE_RECOMPRESS_JPEG |
               LAGHU_IMAGE_JPEG_PROGRESSIVE | LAGHU_IMAGE_JPEG_SAMPLING)) !=
                 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U &&
             (filters & LAGHU_IMAGE_JPEG_TO_WEBP) != 0U));
  }
  if (ngx_http_laghu_type_equals(content_type, "image/png")) {
    return (capabilities & LAGHU_IMAGE_CAP_PNG_LOAD) != 0U &&
           (((capabilities & LAGHU_IMAGE_CAP_PNG_SAVE) != 0U &&
             (filters & (LAGHU_IMAGE_RECOMPRESS_IMAGES |
                         LAGHU_IMAGE_RECOMPRESS_PNG)) != 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_JPEG_SAVE) != 0U && allow_lossy &&
             (filters & LAGHU_IMAGE_PNG_TO_JPEG) != 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U &&
             (filters & LAGHU_IMAGE_TO_WEBP_LOSSLESS) != 0U));
  }
  if (ngx_http_laghu_type_equals(content_type, "image/gif")) {
    return (capabilities & LAGHU_IMAGE_CAP_GIF_LOAD) != 0U &&
           (((capabilities & LAGHU_IMAGE_CAP_PNG_SAVE) != 0U &&
             (filters & LAGHU_IMAGE_GIF_TO_PNG) != 0U) ||
            ((capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U &&
             (filters & (LAGHU_IMAGE_TO_WEBP_LOSSLESS |
                         LAGHU_IMAGE_TO_WEBP_ANIMATED)) != 0U));
  }
  if (ngx_http_laghu_type_equals(content_type, "image/webp")) {
    return (capabilities & LAGHU_IMAGE_CAP_WEBP_LOAD) != 0U &&
           (capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U;
  }
  return false;
}

static bool ngx_http_laghu_queue_available(ngx_http_laghu_loc_conf_t *conf,
                                           const ngx_str_t *content_type,
                                           laghu_image_filter_mask filters,
                                           bool allow_lossy) {
  laghu_runtime_queue *queue = &conf->runtime_queue;
  bool available = false;
  uint64_t now;

  if (conf->worker_queue.len == 0U) {
    return false;
  }
  available = queue->mapping != NULL && laghu_runtime_queue_refresh(queue);
  if (available) {
    now = (uint64_t)ngx_time();
    available = queue->worker_heartbeat != 0U &&
                queue->worker_heartbeat <= now &&
                now - queue->worker_heartbeat <= 45U &&
                ngx_http_laghu_backend_supports(
                    content_type, filters, queue->capabilities, allow_lossy);
    if (queue->worker_heartbeat <= now && now - queue->worker_heartbeat > 45U) {
      laghu_runtime_queue_close(queue);
    }
  }
  if (queue->mapping == NULL &&
      laghu_runtime_queue_open(queue, (const char *)conf->worker_queue.data) &&
      laghu_runtime_queue_refresh(queue)) {
    now = (uint64_t)ngx_time();
    available = queue->worker_heartbeat != 0U &&
                queue->worker_heartbeat <= now &&
                now - queue->worker_heartbeat <= 45U &&
                ngx_http_laghu_backend_supports(
                    content_type, filters, queue->capabilities, allow_lossy);
  }
  return available;
}

static bool ngx_http_laghu_load_cache(ngx_http_request_t *request,
                                      const ngx_http_laghu_loc_conf_t *conf,
                                      ngx_http_laghu_request_ctx_t *context) {
  ngx_buf_t *buffer;

  if (context->validator[0] == '\0' ||
      !laghu_runtime_cache_lookup((const char *)conf->image_cache.data,
                                  context->index_key, context->validator,
                                  &context->cache_entry) ||
      context->cache_entry.length == 0U ||
      context->cache_entry.length > LAGHU_IMAGE_MAX_INPUT_BYTES) {
    return false;
  }
  context->cached_body =
      ngx_pnalloc(request->pool, context->cache_entry.length);
  if (context->cached_body == NULL ||
      !laghu_runtime_cache_read(&context->cache_entry, context->cached_body,
                                context->cache_entry.length)) {
    return false;
  }
  buffer = ngx_calloc_buf(request->pool);
  context->cached_output = ngx_alloc_chain_link(request->pool);
  if (buffer == NULL || context->cached_output == NULL) {
    context->cached_output = NULL;
    return false;
  }
  buffer->pos = context->cached_body;
  buffer->last = context->cached_body + context->cache_entry.length;
  buffer->memory = 1;
  buffer->last_in_chain = 1;
  buffer->last_buf = request == request->main;
  context->cached_output->buf = buffer;
  context->cached_output->next = NULL;
  return true;
}

static bool ngx_http_laghu_prepare_variant_headers(
    ngx_http_request_t *request, ngx_http_laghu_request_ctx_t *context) {
  static const char prefix[] = "\"laghu-";
  ngx_table_elt_t *etag = request->headers_out.etag;
  ngx_table_elt_t *vary;
  ngx_list_part_t *part;
  ngx_table_elt_t *headers;
  ngx_uint_t index;
  u_char *etag_value;
  u_char *cursor;
  size_t key_length = ngx_strlen(context->cache_entry.payload_hash);

  etag_value =
      ngx_pnalloc(request->pool, sizeof(prefix) - 1U + key_length + 2U);
  if (etag_value == NULL) {
    return false;
  }
  vary = ngx_list_push(&request->headers_out.headers);
  if (vary == NULL) {
    return false;
  }
  vary->hash = 0;
  if (etag == NULL) {
    etag = ngx_list_push(&request->headers_out.headers);
    if (etag == NULL) {
      return false;
    }
    etag->hash = 0;
  }
  cursor = ngx_cpymem(etag_value, prefix, sizeof(prefix) - 1U);
  cursor = ngx_cpymem(cursor, context->cache_entry.payload_hash, key_length);
  *cursor++ = '"';
  *cursor = '\0';

  etag->hash = 1;
  ngx_str_set(&etag->key, "ETag");
  etag->value.data = etag_value;
  etag->value.len = (size_t)(cursor - etag_value);
  request->headers_out.etag = etag;
  vary->hash = 1;
  ngx_str_set(&vary->key, "Vary");
  ngx_str_set(&vary->value, "Accept");

  part = &request->headers_out.headers.part;
  headers = part->elts;
  for (index = 0U;; ++index) {
    if (index >= part->nelts) {
      if (part->next == NULL) {
        break;
      }
      part = part->next;
      headers = part->elts;
      index = 0U;
    }
    if (headers[index].hash != 0U &&
        ((headers[index].key.len == sizeof("Content-MD5") - 1U &&
          ngx_strncasecmp(headers[index].key.data, (u_char *)"Content-MD5",
                          sizeof("Content-MD5") - 1U) == 0) ||
         (headers[index].key.len == sizeof("Digest") - 1U &&
          ngx_strncasecmp(headers[index].key.data, (u_char *)"Digest",
                          sizeof("Digest") - 1U) == 0))) {
      headers[index].hash = 0;
    }
  }
  return true;
}

static ngx_int_t ngx_http_laghu_header_filter(ngx_http_request_t *request) {
  ngx_http_laghu_loc_conf_t *conf;
  ngx_http_laghu_request_ctx_t *context;
  laghu_response response;
  laghu_decision decision;
  laghu_image_filter_mask image_filters;

  conf = ngx_http_get_module_loc_conf(request, ngx_http_laghu_module);
  if (conf->core.mode != LAGHU_MODE_ON) {
    return ngx_http_laghu_next_header_filter(request);
  }

  if (conf->core.rewrite_level == LAGHU_REWRITE_LEVEL_PASSTHROUGH) {
    decision = LAGHU_DECISION_BYPASS_PASSTHROUGH;
    if (ngx_http_laghu_add_status_header(request, decision) != NGX_OK) {
      ngx_log_error(
          NGX_LOG_WARN, request->connection->log, 0,
          "laghu could not allocate its response status header; serving the "
          "original response");
    }
    return ngx_http_laghu_next_header_filter(request);
  }

  response.status = (unsigned int)request->headers_out.status;
  response.request_path = ngx_http_laghu_copy_string(request, &request->uri);
  response.content_type =
      ngx_http_laghu_copy_string(request, &request->headers_out.content_type);
#if nginx_version >= 1023000
  response.cache_control = ngx_http_laghu_copy_header_chain(
      request, request->headers_out.cache_control);
#else
  response.cache_control = ngx_http_laghu_copy_header_array(
      request, &request->headers_out.cache_control);
#endif
  response.has_authorization = request->headers_in.authorization != NULL;

  if ((request->uri.len != 0 && response.request_path == NULL) ||
      (request->headers_out.content_type.len != 0 &&
       response.content_type == NULL) ||
#if nginx_version >= 1023000
      (request->headers_out.cache_control != NULL &&
       response.cache_control == NULL)
#else
      (request->headers_out.cache_control.nelts != 0U &&
       response.cache_control == NULL)
#endif
  ) {
    decision = LAGHU_DECISION_BYPASS_ERROR;
  } else {
    decision = laghu_decide(&conf->core, &response);
  }

  if (decision == LAGHU_DECISION_PASS &&
      ngx_http_laghu_is_image_type(&request->headers_out.content_type) &&
      request->headers_out.content_encoding != NULL &&
      request->headers_out.content_encoding->value.len != 0U) {
    decision = LAGHU_DECISION_BYPASS_ENCODED;
  } else if (decision == LAGHU_DECISION_PASS &&
             ngx_http_laghu_is_image_type(&request->headers_out.content_type)) {
    context = ngx_pcalloc(request->pool, sizeof(*context));
    if (context == NULL ||
        !laghu_resolve_config_policy(&conf->core, &context->policy) ||
        !laghu_variant_key((laghu_buffer){NULL, 0U}, &context->policy,
                           context->policy_key)) {
      decision = LAGHU_DECISION_BYPASS_ERROR;
    } else {
      context->accept_webp =
          ngx_http_laghu_accepts_webp(request->headers_in.accept);
      (void)ngx_http_laghu_validator(request, context->validator);
      if (!laghu_runtime_index_key(
              response.request_path != NULL ? response.request_path : "",
              context->validator, context->policy_key, context->accept_webp,
              context->index_key)) {
        decision = LAGHU_DECISION_BYPASS_ERROR;
      } else if (ngx_http_laghu_load_cache(request, conf, context) &&
                 ngx_http_laghu_prepare_variant_headers(request, context)) {
        context->cache_hit = true;
        request->headers_out.content_type.data =
            (u_char *)context->cache_entry.content_type;
        request->headers_out.content_type.len =
            ngx_strlen(context->cache_entry.content_type);
        request->headers_out.content_type_len =
            request->headers_out.content_type.len;
        request->headers_out.content_length_n =
            (off_t)context->cache_entry.length;
        if (request->headers_out.content_length != NULL) {
          request->headers_out.content_length->hash = 0;
          request->headers_out.content_length = NULL;
        }
        decision = LAGHU_DECISION_IMAGE_HIT;
        ngx_http_set_ctx(request, context, ngx_http_laghu_module);
      } else if ((image_filters =
                      ngx_http_laghu_image_filters(&context->policy)) == 0U ||
                 !ngx_http_laghu_queue_available(
                     conf, &request->headers_out.content_type, image_filters,
                     context->policy.allow_lossy)) {
        decision = LAGHU_DECISION_BYPASS_IMAGE_BACKEND;
        if (ngx_http_laghu_backend_warning_emitted == 0U) {
          ngx_http_laghu_backend_warning_emitted = 1U;
          ngx_log_error(NGX_LOG_WARN, request->connection->log, 0,
                        "laghu image worker or selected codec is unavailable; "
                        "preserving image originals");
        }
      } else if (request->headers_out.content_length_n > 0 &&
                 request->headers_out.content_length_n <=
                     (off_t)LAGHU_IMAGE_MAX_INPUT_BYTES) {
        context->capture_capacity =
            (size_t)request->headers_out.content_length_n;
        context->capture =
            ngx_pnalloc(request->pool, context->capture_capacity);
        if (context->capture == NULL) {
          decision = LAGHU_DECISION_BYPASS_ERROR;
        } else {
          context->capture_enabled = true;
          ngx_http_set_ctx(request, context, ngx_http_laghu_module);
        }
      }
    }
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
  ngx_http_laghu_loc_conf_t *conf;
  ngx_http_laghu_request_ctx_t *context;
  ngx_chain_t *current;
  bool final_buffer = false;

  context = ngx_http_get_module_ctx(request, ngx_http_laghu_module);
  if (context == NULL) {
    return ngx_http_laghu_next_body_filter(request, chain);
  }

  if (context->cache_hit) {
    if (context->cache_sent || chain == NULL) {
      return NGX_OK;
    }
    context->cache_sent = true;
    return ngx_http_laghu_next_body_filter(request, context->cached_output);
  }

  if (!context->capture_enabled || chain == NULL) {
    return ngx_http_laghu_next_body_filter(request, chain);
  }
  for (current = chain; current != NULL; current = current->next) {
    ngx_buf_t *buffer = current->buf;
    size_t length = (size_t)ngx_buf_size(buffer);

    if (length > context->capture_capacity - context->capture_length) {
      context->capture_enabled = false;
      break;
    }
    if (length != 0U && ngx_buf_in_memory(buffer)) {
      ngx_memcpy(context->capture + context->capture_length, buffer->pos,
                 length);
    } else if (length != 0U && buffer->in_file) {
      if (ngx_read_file(buffer->file,
                        context->capture + context->capture_length, length,
                        buffer->file_pos) != (ssize_t)length) {
        context->capture_enabled = false;
        break;
      }
    } else if (length != 0U) {
      context->capture_enabled = false;
      break;
    }
    context->capture_length += length;
    if (buffer->last_buf || buffer->last_in_chain) {
      final_buffer = true;
    }
  }

  if (context->capture_enabled && final_buffer &&
      context->capture_length != 0U) {
    laghu_runtime_job job;

    conf = ngx_http_get_module_loc_conf(request, ngx_http_laghu_module);
    memset(&job, 0, sizeof(job));
    if (request->uri.len < sizeof(job.request_path) &&
        request->headers_out.content_type.len < sizeof(job.content_type) &&
        conf->runtime_queue.mapping != NULL) {
      ngx_memcpy(job.request_path, request->uri.data, request->uri.len);
      job.request_path[request->uri.len] = '\0';
      ngx_memcpy(job.content_type, request->headers_out.content_type.data,
                 request->headers_out.content_type.len);
      job.content_type[request->headers_out.content_type.len] = '\0';
      ngx_cpystrn((u_char *)job.index_key, (u_char *)context->index_key,
                  sizeof(job.index_key));
      ngx_cpystrn((u_char *)job.validator, (u_char *)context->validator,
                  sizeof(job.validator));
      ngx_cpystrn((u_char *)job.policy_key, (u_char *)context->policy_key,
                  sizeof(job.policy_key));
      job.filters = ngx_http_laghu_image_filters(&context->policy);
      job.quality = context->policy.image_quality != 0U
                        ? context->policy.image_quality
                        : 100U;
      job.allow_lossy = context->policy.allow_lossy;
      job.accept_webp = context->accept_webp;
      job.payload = (laghu_buffer){context->capture, context->capture_length};
      if (!laghu_runtime_queue_try_publish(&conf->runtime_queue, &job)) {
        time_t now = ngx_time();
        if (ngx_http_laghu_last_queue_warning == 0 ||
            now - ngx_http_laghu_last_queue_warning >= 60) {
          ngx_http_laghu_last_queue_warning = now;
          ngx_log_error(NGX_LOG_WARN, request->connection->log, 0,
                        "laghu image queue is full; preserving the original");
        }
      }
    }
    context->capture_enabled = false;
  }
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
  ngx_pool_cleanup_t *cleanup;

  conf = ngx_pcalloc(configuration->pool, sizeof(ngx_http_laghu_loc_conf_t));
  if (conf == NULL) {
    return NULL;
  }

  laghu_config_init(&conf->core);
  laghu_runtime_queue_init(&conf->runtime_queue);
  cleanup = ngx_pool_cleanup_add(configuration->pool, 0);
  if (cleanup == NULL) {
    return NULL;
  }
  cleanup->handler = ngx_http_laghu_queue_cleanup;
  cleanup->data = &conf->runtime_queue;
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
  ngx_conf_merge_str_value(child_conf->worker_queue, parent_conf->worker_queue,
                           "/run/laghu/jobs.queue");
  ngx_conf_merge_str_value(child_conf->image_cache, parent_conf->image_cache,
                           "/var/cache/laghu/images");
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
                       "laghu expects 'on', 'off', 'preset <name>', or "
                       "'rewrite_level <name>', or 'allow_api on|off'");
    return NGX_CONF_ERROR;
  }

  if (ngx_strcmp(values[1].data, "preset") == 0) {
    laghu_preset preset;

    if (location->core.preset != LAGHU_PRESET_UNSET) {
      return "is duplicate";
    }
    if (location->core.rewrite_level != LAGHU_REWRITE_LEVEL_UNSET) {
      return "cannot combine 'laghu preset' and 'laghu rewrite_level' in the "
             "same context";
    }

    if (laghu_parse_preset((const char *)values[2].data, &preset)) {
      location->core.preset = preset;
      return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                       "unknown laghu preset \"%V\"", &values[2]);
    return NGX_CONF_ERROR;
  }

  if (ngx_strcmp(values[1].data, "rewrite_level") == 0) {
    laghu_rewrite_level rewrite_level;

    if (location->core.rewrite_level != LAGHU_REWRITE_LEVEL_UNSET) {
      return "is duplicate";
    }
    if (location->core.preset != LAGHU_PRESET_UNSET) {
      return "cannot combine 'laghu preset' and 'laghu rewrite_level' in the "
             "same context";
    }

    if (laghu_parse_rewrite_level((const char *)values[2].data,
                                  &rewrite_level)) {
      location->core.rewrite_level = rewrite_level;
      return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                       "unknown laghu rewrite level \"%V\"", &values[2]);
    return NGX_CONF_ERROR;
  }

  if (ngx_strcmp(values[1].data, "allow_api") == 0) {
    if (location->core.allow_api != LAGHU_MODE_UNSET) {
      return "is duplicate";
    }

    if (ngx_strcmp(values[2].data, "on") == 0) {
      location->core.allow_api = LAGHU_MODE_ON;
      return NGX_CONF_OK;
    }

    if (ngx_strcmp(values[2].data, "off") == 0) {
      location->core.allow_api = LAGHU_MODE_OFF;
      return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                       "laghu allow_api expects 'on' or 'off'");
    return NGX_CONF_ERROR;
  }

  if (ngx_strcmp(values[1].data, "image_quality") == 0) {
    ngx_int_t quality;

    if (location->core.image_quality != LAGHU_IMAGE_QUALITY_UNSET) {
      return "is duplicate";
    }
    quality = ngx_atoi(values[2].data, values[2].len);
    if (quality < 1 || quality > 100) {
      ngx_conf_log_error(
          NGX_LOG_EMERG, configuration, 0,
          "laghu image_quality expects an integer from 1 to 100");
      return NGX_CONF_ERROR;
    }
    location->core.image_quality = (unsigned int)quality;
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "worker_queue") == 0) {
    if (location->worker_queue.len != 0U) {
      return "is duplicate";
    }
    location->worker_queue = values[2];
    return NGX_CONF_OK;
  }

  if (ngx_strcmp(values[1].data, "image_cache") == 0) {
    if (location->image_cache.len != 0U) {
      return "is duplicate";
    }
    location->image_cache = values[2];
    return NGX_CONF_OK;
  }

  ngx_conf_log_error(NGX_LOG_EMERG, configuration, 0,
                     "unsupported laghu command \"%V\"", &values[1]);
  return NGX_CONF_ERROR;
}
