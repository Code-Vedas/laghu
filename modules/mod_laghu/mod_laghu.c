// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <httpd.h>

/* Apache requires httpd.h to define its public record types first. */
#include <apr_buckets.h>
#include <apr_strings.h>
#include <apr_tables.h>
#include <http_config.h>
#include <http_log.h>
#include <http_protocol.h>
#include <http_request.h>
#include <util_filter.h>

#include "laghu/core.h"
#include "laghu/image.h"
#include "laghu/runtime.h"

#define LAGHU_APACHE_FILTER "LAGHU"
#define LAGHU_APACHE_INITIAL_CAPTURE (64U * 1024U)

#ifdef _WIN32
#define LAGHU_DEFAULT_QUEUE "C:/ProgramData/Laghu/jobs.queue"
#define LAGHU_DEFAULT_CACHE "C:/ProgramData/Laghu/images"
#else
#define LAGHU_DEFAULT_QUEUE "/run/laghu/jobs.queue"
#define LAGHU_DEFAULT_CACHE "/var/cache/laghu/images"
#endif

typedef struct {
  laghu_config core;
  const char *worker_queue;
  const char *image_cache;
  laghu_runtime_queue queue;
} laghu_apache_config;

typedef struct {
  laghu_apache_config *config;
  laghu_policy policy;
  laghu_runtime_cache_entry cache_entry;
  char policy_key[LAGHU_SHA256_HEX_SIZE];
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  unsigned char *capture;
  size_t capture_length;
  size_t capture_capacity;
  laghu_image_filter_mask filters;
  bool accept_webp;
  bool decided;
  bool capture_enabled;
  bool cache_hit;
  bool cache_sent;
} laghu_apache_context;

module AP_MODULE_DECLARE_DATA laghu_module;

static apr_status_t laghu_apache_queue_cleanup(void *data) {
  laghu_apache_config *config = data;
  laghu_runtime_queue_close(&config->queue);
  return APR_SUCCESS;
}

static void *laghu_apache_create_config(apr_pool_t *pool, char *path) {
  laghu_apache_config *config = apr_pcalloc(pool, sizeof(*config));
  (void)path;
  if (config != NULL) {
    laghu_config_init(&config->core);
    laghu_runtime_queue_init(&config->queue);
    apr_pool_cleanup_register(pool, config, laghu_apache_queue_cleanup,
                              apr_pool_cleanup_null);
  }
  return config;
}

static void *laghu_apache_create_server_config(apr_pool_t *pool,
                                               server_rec *server) {
  (void)server;
  return laghu_apache_create_config(pool, NULL);
}

static void *laghu_apache_merge_config(apr_pool_t *pool, void *parent_value,
                                       void *child_value) {
  laghu_apache_config *parent = parent_value;
  laghu_apache_config *child = child_value;
  laghu_apache_config *merged = apr_pcalloc(pool, sizeof(*merged));
  if (merged == NULL || parent == NULL || child == NULL) {
    return NULL;
  }
  laghu_config_merge(&merged->core, &parent->core, &child->core);
  merged->worker_queue =
      child->worker_queue != NULL ? child->worker_queue : parent->worker_queue;
  merged->image_cache =
      child->image_cache != NULL ? child->image_cache : parent->image_cache;
  laghu_runtime_queue_init(&merged->queue);
  apr_pool_cleanup_register(pool, merged, laghu_apache_queue_cleanup,
                            apr_pool_cleanup_null);
  return merged;
}

static bool laghu_apache_on_off(const char *value, laghu_mode *mode) {
  if (value != NULL && ap_cstr_casecmp(value, "on") == 0) {
    *mode = LAGHU_MODE_ON;
    return true;
  }
  if (value != NULL && ap_cstr_casecmp(value, "off") == 0) {
    *mode = LAGHU_MODE_OFF;
    return true;
  }
  return false;
}

static const char *laghu_apache_command(cmd_parms *command, void *value,
                                        const char *arguments) {
  laghu_apache_config *config = value;
  const char *cursor = arguments;
  const char *name = ap_getword_conf(command->pool, &cursor);
  const char *parameter = ap_getword_conf(command->pool, &cursor);
  const char *extra = ap_getword_conf(command->pool, &cursor);
  laghu_preset preset;
  laghu_rewrite_level level;
  char *end = NULL;
  unsigned long quality;

  if (name[0] == '\0' || extra[0] != '\0') {
    return "Laghu expects one setting and, where required, one value";
  }
  if (parameter[0] == '\0') {
    laghu_mode mode;
    if (!laghu_apache_on_off(name, &mode) ||
        config->core.mode != LAGHU_MODE_UNSET) {
      return "Laghu expects On or Off exactly once in this scope";
    }
    config->core.mode = mode;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "Preset") == 0) {
    if (config->core.preset != LAGHU_PRESET_UNSET ||
        config->core.rewrite_level != LAGHU_REWRITE_LEVEL_UNSET ||
        !laghu_parse_preset(parameter, &preset)) {
      return "Laghu Preset is invalid, duplicated, or conflicts with "
             "RewriteLevel";
    }
    config->core.preset = preset;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "RewriteLevel") == 0) {
    if (config->core.rewrite_level != LAGHU_REWRITE_LEVEL_UNSET ||
        config->core.preset != LAGHU_PRESET_UNSET ||
        !laghu_parse_rewrite_level(parameter, &level)) {
      return "Laghu RewriteLevel is invalid, duplicated, or conflicts with "
             "Preset";
    }
    config->core.rewrite_level = level;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "AllowApi") == 0) {
    if (config->core.allow_api != LAGHU_MODE_UNSET ||
        !laghu_apache_on_off(parameter, &config->core.allow_api)) {
      return "Laghu AllowApi expects On or Off exactly once in this scope";
    }
    return NULL;
  }
  if (ap_cstr_casecmp(name, "ImageQuality") == 0) {
    quality = strtoul(parameter, &end, 10);
    if (config->core.image_quality != LAGHU_IMAGE_QUALITY_UNSET ||
        end == parameter || *end != '\0' || quality == 0U || quality > 100U) {
      return "Laghu ImageQuality expects an integer from 1 through 100 exactly "
             "once";
    }
    config->core.image_quality = (unsigned int)quality;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "WorkerQueue") == 0) {
    if (config->worker_queue != NULL) {
      return "Laghu WorkerQueue may appear only once in this scope";
    }
    config->worker_queue = apr_pstrdup(command->pool, parameter);
    return NULL;
  }
  if (ap_cstr_casecmp(name, "ImageCache") == 0) {
    if (config->image_cache != NULL) {
      return "Laghu ImageCache may appear only once in this scope";
    }
    config->image_cache = apr_pstrdup(command->pool, parameter);
    return NULL;
  }
  return "unknown Laghu setting";
}

static laghu_image_filter_mask laghu_apache_image_filters(
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
  return filters;
}

static bool laghu_apache_accepts_webp(request_rec *request) {
  const char *accept = apr_table_get(request->headers_in, "Accept");
  const char *match =
      accept == NULL ? NULL : ap_strcasestr(accept, "image/webp");
  return match != NULL && ap_strcasestr(match, "q=0") == NULL;
}

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

static void laghu_apache_status(request_rec *request, laghu_decision decision) {
  apr_table_setn(request->headers_out, "X-Laghu",
                 laghu_decision_name(decision));
}

static laghu_decision laghu_apache_decide(ap_filter_t *filter,
                                          laghu_apache_context *context) {
  request_rec *request = filter->r;
  laghu_apache_config *config = context->config;
  laghu_response response;
  const char *validator;

  response.status = (unsigned int)request->status;
  response.request_path = request->uri;
  response.content_type = request->content_type;
  response.cache_control = apr_table_get(request->headers_out, "Cache-Control");
  response.has_authorization =
      apr_table_get(request->headers_in, "Authorization") != NULL;
  {
    laghu_decision decision = laghu_decide(&config->core, &response);
    if (decision != LAGHU_DECISION_PASS || request->content_type == NULL ||
        ap_cstr_casecmpn(request->content_type, "image/", 6U) != 0) {
      return decision;
    }
  }
  if (apr_table_get(request->headers_out, "Content-Encoding") != NULL) {
    return LAGHU_DECISION_BYPASS_ENCODED;
  }
  if (!laghu_resolve_config_policy(&config->core, &context->policy) ||
      !laghu_variant_key((laghu_buffer){NULL, 0U}, &context->policy,
                         context->policy_key)) {
    return LAGHU_DECISION_BYPASS_ERROR;
  }
  context->filters = laghu_apache_image_filters(&context->policy);
  context->accept_webp = laghu_apache_accepts_webp(request);
  validator = apr_table_get(request->headers_out, "ETag");
  if (validator != NULL && ap_cstr_casecmpn(validator, "W/", 2U) != 0 &&
      strlen(validator) < sizeof(context->validator)) {
    memcpy(context->validator, validator, strlen(validator) + 1U);
  } else if (request->finfo.filetype != APR_NOFILE && request->mtime > 0) {
    int length = apr_snprintf(context->validator, sizeof(context->validator),
                              "file-%" APR_TIME_T_FMT "-%" APR_OFF_T_FMT,
                              request->mtime, request->finfo.size);
    if (length <= 0 || (size_t)length >= sizeof(context->validator)) {
      context->validator[0] = '\0';
    }
  }
  if (!laghu_runtime_index_key(request->uri != NULL ? request->uri : "",
                               context->validator, context->policy_key,
                               context->accept_webp, context->index_key)) {
    return LAGHU_DECISION_BYPASS_ERROR;
  }
  if (context->validator[0] != '\0' &&
      laghu_runtime_cache_lookup(
          config->image_cache != NULL ? config->image_cache
                                      : LAGHU_DEFAULT_CACHE,
          context->index_key, context->validator, &context->cache_entry) &&
      context->cache_entry.length <= LAGHU_IMAGE_MAX_INPUT_BYTES) {
    context->cache_hit = true;
    ap_set_content_type(request, context->cache_entry.content_type);
    ap_set_content_length(request, (apr_off_t)context->cache_entry.length);
    apr_table_setn(request->headers_out, "Vary", "Accept");
    apr_table_set(request->headers_out, "ETag",
                  apr_psprintf(request->pool, "\"laghu-%s\"",
                               context->cache_entry.payload_hash));
    apr_table_unset(request->headers_out, "Content-MD5");
    apr_table_unset(request->headers_out, "Digest");
    return LAGHU_DECISION_IMAGE_HIT;
  }
  if (context->filters == 0U || !laghu_apache_backend_available(config)) {
    return LAGHU_DECISION_BYPASS_IMAGE_BACKEND;
  }
  if (request->clength <= (apr_off_t)LAGHU_IMAGE_MAX_INPUT_BYTES) {
    context->capture_capacity = request->clength > 0
                                    ? (size_t)request->clength
                                    : LAGHU_APACHE_INITIAL_CAPTURE;
    context->capture = apr_palloc(request->pool, context->capture_capacity);
    context->capture_enabled = context->capture != NULL;
  }
  return LAGHU_DECISION_PASS;
}

static apr_status_t laghu_apache_filter(ap_filter_t *filter,
                                        apr_bucket_brigade *brigade) {
  request_rec *request = filter->r;
  laghu_apache_context *context = filter->ctx;
  apr_bucket *bucket;
  bool eos = false;

  if (context == NULL) {
    laghu_apache_config *server_config;
    laghu_apache_config *directory_config;
    context = apr_pcalloc(request->pool, sizeof(*context));
    if (context == NULL) {
      ap_remove_output_filter(filter);
      return ap_pass_brigade(filter->next, brigade);
    }
    server_config =
        ap_get_module_config(request->server->module_config, &laghu_module);
    directory_config =
        ap_get_module_config(request->per_dir_config, &laghu_module);
    context->config = laghu_apache_merge_config(request->pool, server_config,
                                                directory_config);
    if (context->config == NULL) {
      ap_remove_output_filter(filter);
      return ap_pass_brigade(filter->next, brigade);
    }
    filter->ctx = context;
  }
  if (!context->decided) {
    laghu_decision decision = laghu_apache_decide(filter, context);
    context->decided = true;
    laghu_apache_status(request, decision);
  }
  if (context->cache_hit) {
    apr_bucket_brigade *replacement;
    unsigned char *body;
    if (context->cache_sent) {
      apr_brigade_cleanup(brigade);
      return APR_SUCCESS;
    }
    body = apr_palloc(request->pool, context->cache_entry.length);
    replacement =
        apr_brigade_create(request->pool, request->connection->bucket_alloc);
    if (body == NULL || replacement == NULL ||
        !laghu_runtime_cache_read(&context->cache_entry, body,
                                  context->cache_entry.length)) {
      context->cache_hit = false;
      return ap_pass_brigade(filter->next, brigade);
    }
    APR_BRIGADE_INSERT_TAIL(
        replacement, apr_bucket_pool_create(
                         (const char *)body, context->cache_entry.length,
                         request->pool, request->connection->bucket_alloc));
    APR_BRIGADE_INSERT_TAIL(
        replacement, apr_bucket_eos_create(request->connection->bucket_alloc));
    context->cache_sent = true;
    apr_brigade_cleanup(brigade);
    return ap_pass_brigade(filter->next, replacement);
  }
  if (context->capture_enabled) {
    for (bucket = APR_BRIGADE_FIRST(brigade);
         bucket != APR_BRIGADE_SENTINEL(brigade);
         bucket = APR_BUCKET_NEXT(bucket)) {
      const char *data;
      apr_size_t length;
      apr_status_t status;
      if (APR_BUCKET_IS_EOS(bucket)) {
        eos = true;
        continue;
      }
      if (APR_BUCKET_IS_METADATA(bucket)) {
        continue;
      }
      status = apr_bucket_read(bucket, &data, &length, APR_NONBLOCK_READ);
      if (status != APR_SUCCESS ||
          length > LAGHU_IMAGE_MAX_INPUT_BYTES - context->capture_length) {
        context->capture_enabled = false;
        break;
      }
      if (length > context->capture_capacity - context->capture_length) {
        size_t required = context->capture_length + length;
        size_t capacity = context->capture_capacity;
        unsigned char *expanded;
        while (capacity < required && capacity < LAGHU_IMAGE_MAX_INPUT_BYTES) {
          capacity = capacity > LAGHU_IMAGE_MAX_INPUT_BYTES / 2U
                         ? LAGHU_IMAGE_MAX_INPUT_BYTES
                         : capacity * 2U;
        }
        expanded = apr_palloc(request->pool, capacity);
        if (expanded == NULL) {
          context->capture_enabled = false;
          break;
        }
        memcpy(expanded, context->capture, context->capture_length);
        context->capture = expanded;
        context->capture_capacity = capacity;
      }
      memcpy(context->capture + context->capture_length, data, length);
      context->capture_length += length;
    }
  }
  if (context->capture_enabled && eos && context->capture_length != 0U) {
    laghu_runtime_job job;
    memset(&job, 0, sizeof(job));
    if (request->uri != NULL && request->content_type != NULL &&
        strlen(request->uri) < sizeof(job.request_path) &&
        strlen(request->content_type) < sizeof(job.content_type)) {
      strcpy(job.request_path, request->uri);
      strcpy(job.content_type, request->content_type);
      strcpy(job.index_key, context->index_key);
      strcpy(job.validator, context->validator);
      strcpy(job.policy_key, context->policy_key);
      job.filters = context->filters;
      job.quality = context->policy.image_quality;
      job.allow_lossy = context->policy.allow_lossy;
      job.accept_webp = context->accept_webp;
      job.payload = (laghu_buffer){context->capture, context->capture_length};
      (void)laghu_runtime_queue_try_publish(&context->config->queue, &job);
    }
    context->capture_enabled = false;
  }
  return ap_pass_brigade(filter->next, brigade);
}

static void laghu_apache_insert_filter(request_rec *request) {
  laghu_apache_config *server_config =
      ap_get_module_config(request->server->module_config, &laghu_module);
  laghu_apache_config *directory_config =
      ap_get_module_config(request->per_dir_config, &laghu_module);
  laghu_config effective;
  if (server_config == NULL || directory_config == NULL) {
    return;
  }
  laghu_config_merge(&effective, &server_config->core, &directory_config->core);
  if (effective.mode == LAGHU_MODE_ON) {
    ap_add_output_filter(LAGHU_APACHE_FILTER, NULL, request,
                         request->connection);
  }
}

static void laghu_apache_register(apr_pool_t *pool) {
  (void)pool;
  ap_register_output_filter(LAGHU_APACHE_FILTER, laghu_apache_filter, NULL,
                            AP_FTYPE_RESOURCE);
  ap_hook_insert_filter(laghu_apache_insert_filter, NULL, NULL,
                        APR_HOOK_MIDDLE);
}

static const command_rec laghu_apache_commands[] = {
    AP_INIT_RAW_ARGS("Laghu", laghu_apache_command, NULL,
                     RSRC_CONF | ACCESS_CONF,
                     "Laghu On|Off or Laghu <Setting> <Value>"),
    {NULL}};

module AP_MODULE_DECLARE_DATA laghu_module = {
    STANDARD20_MODULE_STUFF,   laghu_apache_create_config,
    laghu_apache_merge_config, laghu_apache_create_server_config,
    laghu_apache_merge_config, laghu_apache_commands,
    laghu_apache_register};
