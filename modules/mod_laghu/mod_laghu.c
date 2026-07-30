// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <httpd.h>
#include <stdlib.h>

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
#include "laghu/http.h"
#include "laghu/image.h"
#include "laghu/runtime.h"

#define LAGHU_APACHE_FILTER "LAGHU"
#define LAGHU_APACHE_INITIAL_CAPTURE (64U * 1024U)

#ifdef _WIN32
#define LAGHU_DEFAULT_QUEUE "C:/ProgramData/Laghu/jobs.queue"
#define LAGHU_DEFAULT_CACHE "C:/ProgramData/Laghu/images"
#define LAGHU_DEFAULT_FONT_QUEUE "C:/ProgramData/Laghu/fonts.queue"
#define LAGHU_DEFAULT_JAVASCRIPT_QUEUE "C:/ProgramData/Laghu/javascript.queue"
#else
#define LAGHU_DEFAULT_QUEUE "/run/laghu/jobs.queue"
#define LAGHU_DEFAULT_CACHE "/var/cache/laghu/images"
#define LAGHU_DEFAULT_FONT_QUEUE "/run/laghu/fonts.queue"
#define LAGHU_DEFAULT_JAVASCRIPT_QUEUE "/run/laghu/javascript.queue"
#endif

typedef struct {
  laghu_config core;
  const char *worker_queue;
  const char *font_fetch_queue;
  const char *font_provider_config;
  const char *javascript_queue;
  const char *javascript_target;
  const char *javascript_observation_config;
  const char *image_cache;
  laghu_runtime_queue queue;
  laghu_runtime_queue font_queue;
  laghu_runtime_queue javascript_runtime_queue;
  laghu_font_provider_set font_providers;
  laghu_javascript_observation_set javascript_observations;
  bool font_providers_loaded;
  bool javascript_observations_loaded;
} laghu_apache_config;

typedef struct {
  laghu_apache_config *config;
  laghu_http_request request;
  laghu_http_response response;
  laghu_http_environment environment;
  laghu_http_header request_headers[LAGHU_HTTP_MAX_REQUEST_HEADERS];
  laghu_http_header response_headers[LAGHU_HTTP_MAX_RESPONSE_HEADERS];
  laghu_http_transaction transaction;
  laghu_policy policy;
  laghu_runtime_cache_entry cache_entry;
  char policy_key[LAGHU_SHA256_HEX_SIZE];
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  unsigned char *capture;
  size_t capture_length;
  size_t capture_capacity;
  unsigned char *selected_body;
  size_t selected_length;
  laghu_http_action action;
  laghu_image_filter_mask filters;
  bool accept_webp;
  bool decided;
  bool capture_enabled;
  bool cache_hit;
  bool cache_sent;
  unsigned int target_count;
  unsigned int target_width[LAGHU_RUNTIME_MAX_TARGETS];
  unsigned int target_height[LAGHU_RUNTIME_MAX_TARGETS];
  uint64_t resize_filter[LAGHU_RUNTIME_MAX_TARGETS];
  bool html_capture;
  bool css_capture;
} laghu_apache_context;

module AP_MODULE_DECLARE_DATA laghu_module;
static apr_time_t laghu_apache_beacon_window;
static unsigned int laghu_apache_beacon_count;

static unsigned int laghu_apache_unsigned_header(request_rec *request,
                                                 const char *name,
                                                 unsigned int minimum,
                                                 unsigned int maximum) {
  const char *value = apr_table_get(request->headers_in, name);
  apr_int64_t parsed;
  if (value == NULL || *value == '\0') {
    return 0U;
  }
  parsed = apr_atoi64(value);
  return parsed >= minimum && parsed <= maximum ? (unsigned int)parsed : 0U;
}

static unsigned int laghu_apache_dpr_header(request_rec *request) {
  const char *value = apr_table_get(request->headers_in, "DPR");
  char *end = NULL;
  double parsed;
  if (value == NULL || *value == '\0') {
    return 100U;
  }
  parsed = strtod(value, &end);
  return end != value && *end == '\0' && parsed >= 1.0 && parsed <= 4.0
             ? (unsigned int)(parsed * 100.0 + 0.5)
             : 100U;
}

static unsigned int laghu_apache_viewport_header(request_rec *request) {
  unsigned int width = laghu_apache_unsigned_header(
      request, "Sec-CH-Viewport-Width", 1U, LAGHU_IMAGE_MAX_DIMENSION);
  return width != 0U
             ? width
             : laghu_apache_unsigned_header(request, "Viewport-Width", 1U,
                                            LAGHU_IMAGE_MAX_DIMENSION);
}

static apr_status_t laghu_apache_queue_cleanup(void *data) {
  laghu_apache_config *config = data;
  laghu_runtime_queue_close(&config->queue);
  laghu_runtime_queue_close(&config->font_queue);
  laghu_runtime_queue_close(&config->javascript_runtime_queue);
  return APR_SUCCESS;
}

static void *laghu_apache_create_config(apr_pool_t *pool, char *path) {
  laghu_apache_config *config = apr_pcalloc(pool, sizeof(*config));
  (void)path;
  if (config != NULL) {
    laghu_config_init(&config->core);
    laghu_runtime_queue_init(&config->queue);
    laghu_runtime_queue_init(&config->font_queue);
    laghu_runtime_queue_init(&config->javascript_runtime_queue);
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
  merged->font_fetch_queue = child->font_fetch_queue != NULL
                                 ? child->font_fetch_queue
                                 : parent->font_fetch_queue;
  merged->font_provider_config = child->font_provider_config != NULL
                                     ? child->font_provider_config
                                     : parent->font_provider_config;
  merged->javascript_queue = child->javascript_queue != NULL
                                 ? child->javascript_queue
                                 : parent->javascript_queue;
  merged->javascript_target = child->javascript_target != NULL
                                  ? child->javascript_target
                                  : parent->javascript_target;
  merged->javascript_observation_config =
      child->javascript_observation_config != NULL
          ? child->javascript_observation_config
          : parent->javascript_observation_config;
  if (child->font_providers_loaded) {
    merged->font_providers = child->font_providers;
    merged->font_providers_loaded = true;
  } else if (parent->font_providers_loaded) {
    merged->font_providers = parent->font_providers;
    merged->font_providers_loaded = true;
  }
  if (child->javascript_observations_loaded) {
    merged->javascript_observations = child->javascript_observations;
    merged->javascript_observations_loaded = true;
  } else if (parent->javascript_observations_loaded) {
    merged->javascript_observations = parent->javascript_observations;
    merged->javascript_observations_loaded = true;
  }
  laghu_runtime_queue_init(&merged->queue);
  laghu_runtime_queue_init(&merged->font_queue);
  laghu_runtime_queue_init(&merged->javascript_runtime_queue);
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
  if (ap_cstr_casecmp(name, "ImageBeacon") == 0) {
    if (config->core.image_beacon != LAGHU_MODE_UNSET ||
        !laghu_apache_on_off(parameter, &config->core.image_beacon)) {
      return "Laghu ImageBeacon expects On or Off exactly once in this scope";
    }
    return NULL;
  }
  if (ap_cstr_casecmp(name, "CriticalCssBeacon") == 0) {
    if (config->core.critical_css_beacon != LAGHU_MODE_UNSET ||
        !laghu_apache_on_off(parameter, &config->core.critical_css_beacon)) {
      return "Laghu CriticalCssBeacon expects On or Off exactly once in this "
             "scope";
    }
    return NULL;
  }
  if (ap_cstr_casecmp(name, "InstrumentationBeacon") == 0) {
    if (config->core.instrumentation_beacon != LAGHU_MODE_UNSET ||
        !laghu_apache_on_off(parameter, &config->core.instrumentation_beacon))
      return "Laghu InstrumentationBeacon expects On or Off exactly once";
    return NULL;
  }
  if (ap_cstr_casecmp(name, "InstrumentationSampleRate") == 0) {
    quality = strtoul(parameter, &end, 10);
    if (config->core.instrumentation_sample_rate !=
            LAGHU_INSTRUMENTATION_SAMPLE_RATE_UNSET ||
        end == parameter || *end != '\0' || quality > 100U)
      return "Laghu InstrumentationSampleRate expects 0 through 100 exactly "
             "once";
    config->core.instrumentation_sample_rate = (unsigned int)quality;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "ImageInlineLimit") == 0) {
    quality = strtoul(parameter, &end, 10);
    if (config->core.image_inline_limit != LAGHU_IMAGE_INLINE_LIMIT_UNSET ||
        end == parameter || *end != '\0' || quality > 16384U) {
      return "Laghu ImageInlineLimit expects 0 through 16384 exactly once";
    }
    config->core.image_inline_limit = (unsigned int)quality;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "ImageMetadataLimit") == 0) {
    quality = strtoul(parameter, &end, 10);
    if (config->core.image_metadata_limit != LAGHU_IMAGE_METADATA_LIMIT_UNSET ||
        end == parameter || *end != '\0' || quality < 1U || quality > 100000U) {
      return "Laghu ImageMetadataLimit expects 1 through 100000 exactly once";
    }
    config->core.image_metadata_limit = (unsigned int)quality;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "ImageMetadataTtl") == 0) {
    char unit;
    unsigned long seconds;
    quality = strtoul(parameter, &end, 10);
    unit = end != NULL ? *end : '\0';
    if (unit == 'h' && end[1] == '\0') {
      seconds = quality * 3600U;
    } else if (unit == 'd' && end[1] == '\0') {
      seconds = quality * 86400U;
    } else {
      return "Laghu ImageMetadataTtl expects a duration from 1h through 30d";
    }
    if (config->core.image_metadata_ttl != LAGHU_IMAGE_METADATA_TTL_UNSET ||
        seconds < 3600U || seconds > 2592000U) {
      return "Laghu ImageMetadataTtl expects a duration from 1h through 30d";
    }
    config->core.image_metadata_ttl = (unsigned int)seconds;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "CssInlineLimit") == 0) {
    quality = strtoul(parameter, &end, 10);
    if (config->core.css_inline_limit != LAGHU_CSS_INLINE_LIMIT_UNSET ||
        end == parameter || *end != '\0' || quality > 65536U) {
      return "Laghu CssInlineLimit expects 0 through 65536 exactly once";
    }
    config->core.css_inline_limit = (unsigned int)quality;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "CssOutlineThreshold") == 0) {
    quality = strtoul(parameter, &end, 10);
    if (config->core.css_outline_threshold !=
            LAGHU_CSS_OUTLINE_THRESHOLD_UNSET ||
        end == parameter || *end != '\0' || quality < 1024U ||
        quality > 1048576U) {
      return "Laghu CssOutlineThreshold expects 1024 through 1048576 exactly "
             "once";
    }
    config->core.css_outline_threshold = (unsigned int)quality;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "JavaScriptInlineLimit") == 0) {
    quality = strtoul(parameter, &end, 10);
    if (config->core.javascript_inline_limit !=
            LAGHU_JAVASCRIPT_INLINE_LIMIT_UNSET ||
        end == parameter || *end != '\0' || quality > 65536U) {
      return "Laghu JavaScriptInlineLimit expects 0 through 65536 exactly once";
    }
    config->core.javascript_inline_limit = (unsigned int)quality;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "JavaScriptOutlineThreshold") == 0) {
    quality = strtoul(parameter, &end, 10);
    if (config->core.javascript_outline_threshold !=
            LAGHU_JAVASCRIPT_OUTLINE_THRESHOLD_UNSET ||
        end == parameter || *end != '\0' || quality < 1024U ||
        quality > 1048576U) {
      return "Laghu JavaScriptOutlineThreshold expects 1024 through 1048576 "
             "exactly once";
    }
    config->core.javascript_outline_threshold = (unsigned int)quality;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "WorkerQueue") == 0) {
    if (config->worker_queue != NULL) {
      return "Laghu WorkerQueue may appear only once in this scope";
    }
    config->worker_queue = apr_pstrdup(command->pool, parameter);
    return NULL;
  }
  if (ap_cstr_casecmp(name, "FontFetchQueue") == 0) {
    if (config->font_fetch_queue != NULL) {
      return "Laghu FontFetchQueue may appear only once in this scope";
    }
    config->font_fetch_queue = apr_pstrdup(command->pool, parameter);
    return NULL;
  }
  if (ap_cstr_casecmp(name, "FontProviderConfig") == 0) {
    char error[256] = "configuration appears more than once";
    if (config->font_provider_config != NULL ||
        !laghu_font_providers_load(parameter, &config->font_providers, error,
                                   sizeof(error))) {
      return apr_psprintf(command->pool,
                          "Laghu FontProviderConfig is invalid: %s", error);
    }
    config->font_provider_config = apr_pstrdup(command->pool, parameter);
    config->font_providers_loaded = true;
    return NULL;
  }
  if (ap_cstr_casecmp(name, "JavaScriptQueue") == 0) {
    if (config->javascript_queue != NULL)
      return "Laghu JavaScriptQueue may appear only once in this scope";
    config->javascript_queue = apr_pstrdup(command->pool, parameter);
    return NULL;
  }
  if (ap_cstr_casecmp(name, "JavaScriptTarget") == 0) {
    char normalized[LAGHU_JAVASCRIPT_TARGET_SIZE];
    if (config->javascript_target != NULL ||
        !laghu_javascript_target_normalize(parameter, normalized))
      return "Laghu JavaScriptTarget expects a bounded Browserslist query";
    config->javascript_target = apr_pstrdup(command->pool, normalized);
    return NULL;
  }
  if (ap_cstr_casecmp(name, "JavaScriptObservationConfig") == 0) {
    char error[256] = "configuration appears more than once";
    if (config->javascript_observation_config != NULL ||
        !laghu_javascript_observations_load(
            parameter, &config->javascript_observations, error, sizeof(error)))
      return apr_psprintf(command->pool,
                          "Laghu JavaScriptObservationConfig is invalid: %s",
                          error);
    config->javascript_observation_config =
        apr_pstrdup(command->pool, parameter);
    config->javascript_observations_loaded = true;
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
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_DIMENSIONS) != 0U) {
    filters |= LAGHU_IMAGE_INSERT_DIMENSIONS;
  }
  if ((policy->filter_families & LAGHU_FILTER_IMAGE_LAZYLOAD) != 0U) {
    filters |= LAGHU_IMAGE_LAZYLOAD | LAGHU_IMAGE_INLINE_PREVIEW;
  }
  if ((policy->filter_families & LAGHU_FILTER_RESOURCE_INLINE) != 0U) {
    filters |= LAGHU_IMAGE_INLINE | LAGHU_IMAGE_DEDUP_INLINE;
  }
  return filters;
}

static laghu_html_planner_mask laghu_apache_html_plan(
    const laghu_policy *policy) {
  laghu_html_planner_mask plan = 0U;
  bool html = (policy->filter_families & LAGHU_FILTER_HTML_MINIFY) != 0U;
  bool css = (policy->filter_families & LAGHU_FILTER_CSS_MINIFY) != 0U;
  if (html) {
    plan |= LAGHU_HTML_PLAN_LEXICAL | LAGHU_HTML_PLAN_CONVERT_META_TAGS;
  }
  if ((policy->filter_families & LAGHU_FILTER_RESOURCE_HINTS) != 0U) {
    plan |= LAGHU_HTML_PLAN_RESOURCE_HINTS;
  }
  if (html && policy->allow_structural_rewrite) {
    plan |= LAGHU_HTML_PLAN_ADD_COMBINE_HEAD;
  }
  if (html && css && policy->allow_structural_rewrite) {
    plan |= LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD;
    if (policy->allow_script_reordering) {
      plan |= LAGHU_HTML_PLAN_MOVE_CSS_ABOVE_SCRIPTS;
    }
  }
  return plan;
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
    if (decision != LAGHU_DECISION_PASS || request->content_type == NULL) {
      return decision;
    }
  }
  if (ap_cstr_casecmpn(request->content_type, "text/html", 9U) == 0) {
    if (apr_table_get(request->headers_out, "Content-Encoding") != NULL ||
        request->clength <= 0 ||
        request->clength > (apr_off_t)LAGHU_IMAGE_MAX_INPUT_BYTES ||
        !laghu_resolve_config_policy(&config->core, &context->policy) ||
        !laghu_variant_key((laghu_buffer){NULL, 0U}, &context->policy,
                           context->policy_key)) {
      return LAGHU_DECISION_PASS;
    }
    (void)laghu_apache_backend_available(config);
    context->filters = laghu_apache_image_filters(&context->policy);
    context->capture_capacity = (size_t)request->clength;
    context->capture = apr_palloc(request->pool, context->capture_capacity);
    context->capture_enabled = context->capture != NULL;
    context->html_capture = context->capture_enabled;
    return LAGHU_DECISION_PASS;
  }
  if (ap_cstr_casecmpn(request->content_type, "text/css", 8U) == 0) {
    if (apr_table_get(request->headers_out, "Content-Encoding") != NULL ||
        request->clength <= 0 ||
        request->clength > (apr_off_t)LAGHU_CSS_MAX_INPUT_BYTES ||
        !laghu_resolve_config_policy(&config->core, &context->policy) ||
        !laghu_variant_key((laghu_buffer){NULL, 0U}, &context->policy,
                           context->policy_key) ||
        ((context->policy.filter_families & LAGHU_FILTER_CSS_MINIFY) == 0U &&
         !context->policy.allow_structural_rewrite)) {
      return LAGHU_DECISION_PASS;
    }
    context->capture_capacity = (size_t)request->clength;
    (void)laghu_apache_backend_available(config);
    context->capture = apr_palloc(request->pool, context->capture_capacity);
    context->capture_enabled = context->capture != NULL;
    context->css_capture = context->capture_enabled;
    return LAGHU_DECISION_PASS;
  }
  if (ap_cstr_casecmpn(request->content_type, "image/", 6U) != 0) {
    return LAGHU_DECISION_PASS;
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
  {
    bool backend_available = laghu_apache_backend_available(config);
    if (backend_available) {
      laghu_catalog_record catalog;
      if (laghu_catalog_lookup_url(
              config->image_cache != NULL ? config->image_cache
                                          : LAGHU_DEFAULT_CACHE,
              request->uri, context->policy_key, config->queue.capabilities,
              (uint64_t)apr_time_sec(apr_time_now()),
              config->core.image_metadata_ttl, &catalog)) {
        unsigned int index;
        for (index = 0U; index < catalog.variant_count &&
                         context->target_count < LAGHU_RUNTIME_MAX_TARGETS;
             ++index) {
          if (!catalog.variants[index].ready &&
              !catalog.variants[index].terminally_excluded &&
              catalog.variants[index].width > 0U) {
            unsigned int target = context->target_count++;
            context->target_width[target] = catalog.variants[index].width;
            context->target_height[target] = catalog.variants[index].height;
            context->resize_filter[target] = LAGHU_IMAGE_RESIZE_ATTRIBUTE;
          }
        }
      }
    }
    if (context->validator[0] != '\0' && context->target_count == 0U &&
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
    if (context->filters == 0U || !backend_available) {
      return LAGHU_DECISION_BYPASS_IMAGE_BACKEND;
    }
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

static bool laghu_apache_collect_table(const apr_table_t *table,
                                       laghu_http_header *headers,
                                       size_t capacity, size_t *count) {
  const apr_array_header_t *array = apr_table_elts(table);
  const apr_table_entry_t *entries = (const apr_table_entry_t *)array->elts;
  int index;
  *count = 0U;
  for (index = 0; index < array->nelts; ++index) {
    if (entries[index].key == NULL || entries[index].val == NULL) {
      continue;
    }
    if (*count >= capacity) {
      return false;
    }
    headers[*count].name = (laghu_buffer){
        (const unsigned char *)entries[index].key, strlen(entries[index].key)};
    headers[*count].value = (laghu_buffer){
        (const unsigned char *)entries[index].val, strlen(entries[index].val)};
    ++*count;
  }
  return true;
}

static bool laghu_apache_normalize(request_rec *request,
                                   laghu_apache_context *context) {
  const char *authority = request->hostname != NULL
                              ? request->hostname
                              : request->server->server_hostname;
  const char *validator = apr_table_get(request->headers_out, "ETag");
  const char *file_validator = NULL;
  laghu_http_transaction_init(&context->transaction);
  if (!laghu_apache_collect_table(request->headers_in, context->request_headers,
                                  LAGHU_HTTP_MAX_REQUEST_HEADERS,
                                  &context->request.header_count) ||
      !laghu_apache_collect_table(
          request->headers_out, context->response_headers,
          LAGHU_HTTP_MAX_RESPONSE_HEADERS, &context->response.header_count)) {
    return false;
  }
  if (request->content_type != NULL &&
      apr_table_get(request->headers_out, "Content-Type") == NULL) {
    size_t index = context->response.header_count++;
    if (index >= LAGHU_HTTP_MAX_RESPONSE_HEADERS) {
      return false;
    }
    context->response_headers[index].name =
        (laghu_buffer){(const unsigned char *)"Content-Type", 12U};
    context->response_headers[index].value =
        (laghu_buffer){(const unsigned char *)request->content_type,
                       strlen(request->content_type)};
  }
  if ((validator == NULL || ap_cstr_casecmpn(validator, "W/", 2U) == 0) &&
      request->finfo.filetype != APR_NOFILE && request->mtime > 0) {
    file_validator =
        apr_psprintf(request->pool, "file-%" APR_TIME_T_FMT "-%" APR_OFF_T_FMT,
                     request->mtime, request->finfo.size);
  }
  context->request.version = LAGHU_HTTP_ABI_VERSION;
  context->request.struct_size = sizeof(context->request);
  context->request.method = (laghu_buffer){
      (const unsigned char *)request->method, strlen(request->method)};
  context->request.scheme =
      (laghu_buffer){(const unsigned char *)ap_http_scheme(request),
                     strlen(ap_http_scheme(request))};
  context->request.authority =
      (laghu_buffer){(const unsigned char *)authority,
                     authority == NULL ? 0U : strlen(authority)};
  context->request.normalized_path =
      (laghu_buffer){(const unsigned char *)request->uri,
                     request->uri == NULL ? 0U : strlen(request->uri)};
  context->request.headers = context->request_headers;
  context->response.version = LAGHU_HTTP_ABI_VERSION;
  context->response.struct_size = sizeof(context->response);
  context->response.status = (unsigned int)request->status;
  context->response.headers = context->response_headers;
  context->response.has_declared_length = request->clength >= 0;
  context->response.declared_length =
      request->clength >= 0 ? (size_t)request->clength : 0U;
  context->response.complete = true;
  context->response.partial = request->status == HTTP_PARTIAL_CONTENT;
  if (file_validator != NULL) {
    context->response.source_validator = (laghu_buffer){
        (const unsigned char *)file_validator, strlen(file_validator)};
  }
  context->environment.version = LAGHU_HTTP_ABI_VERSION;
  context->environment.struct_size = sizeof(context->environment);
  context->environment.config = context->config->core;
  context->environment.cache_path = context->config->image_cache != NULL
                                        ? context->config->image_cache
                                        : LAGHU_DEFAULT_CACHE;
  context->environment.worker_queue_path = context->config->worker_queue != NULL
                                               ? context->config->worker_queue
                                               : LAGHU_DEFAULT_QUEUE;
  context->environment.queue = &context->config->queue;
  context->environment.font_fetch_queue_path =
      context->config->font_fetch_queue != NULL
          ? context->config->font_fetch_queue
          : LAGHU_DEFAULT_FONT_QUEUE;
  if (context->config->font_providers_loaded &&
      ((context->config->font_queue.mapping != NULL &&
        laghu_runtime_queue_refresh(&context->config->font_queue)) ||
       (context->config->font_queue.mapping == NULL &&
        laghu_runtime_queue_open(
            &context->config->font_queue,
            context->environment.font_fetch_queue_path)))) {
    context->environment.font_fetch_queue = &context->config->font_queue;
    context->environment.font_providers = &context->config->font_providers;
  }
  context->environment.javascript_queue_path =
      context->config->javascript_queue != NULL
          ? context->config->javascript_queue
          : LAGHU_DEFAULT_JAVASCRIPT_QUEUE;
  if ((context->config->javascript_runtime_queue.mapping != NULL &&
       laghu_runtime_queue_refresh(
           &context->config->javascript_runtime_queue)) ||
      (context->config->javascript_runtime_queue.mapping == NULL &&
       laghu_runtime_queue_open(&context->config->javascript_runtime_queue,
                                context->environment.javascript_queue_path)))
    context->environment.javascript_queue =
        &context->config->javascript_runtime_queue;
  context->environment.javascript_target =
      context->config->javascript_target != NULL
          ? context->config->javascript_target
          : "defaults and supports es6-module and not dead";
  context->environment.javascript_observations =
      context->config->javascript_observations_loaded
          ? &context->config->javascript_observations
          : NULL;
  context->environment.now = (uint64_t)apr_time_sec(apr_time_now());
  return true;
}

static bool laghu_apache_apply_result(
    request_rec *request, const laghu_http_transaction_result *result) {
  const char **names = apr_pcalloc(
      request->pool, result->header_operation_count * sizeof(*names));
  const char **values = apr_pcalloc(
      request->pool, result->header_operation_count * sizeof(*values));
  size_t index;
  if (result->header_operation_count != 0U &&
      (names == NULL || values == NULL)) {
    return false;
  }
  for (index = 0U; index < result->header_operation_count; ++index) {
    names[index] =
        apr_pstrdup(request->pool, result->header_operations[index].name);
    if (result->header_operations[index].kind != LAGHU_HTTP_HEADER_REMOVE) {
      values[index] =
          apr_pstrdup(request->pool, result->header_operations[index].value);
    }
    if (names[index] == NULL ||
        (result->header_operations[index].kind != LAGHU_HTTP_HEADER_REMOVE &&
         values[index] == NULL)) {
      return false;
    }
  }
  for (index = 0U; index < result->header_operation_count; ++index) {
    switch (result->header_operations[index].kind) {
      case LAGHU_HTTP_HEADER_SET:
        apr_table_set(request->headers_out, names[index], values[index]);
        break;
      case LAGHU_HTTP_HEADER_APPEND:
        apr_table_add(request->headers_out, names[index], values[index]);
        break;
      case LAGHU_HTTP_HEADER_REMOVE:
        apr_table_unset(request->headers_out, names[index]);
        break;
    }
  }
  return true;
}

apr_status_t laghu_apache_filter(ap_filter_t *filter,
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
  if (context->css_capture) {
    if (!eos) {
      apr_brigade_cleanup(brigade);
      return APR_SUCCESS;
    }
    if (context->capture_enabled) {
      laghu_runtime_css_result rewritten;
      const char *host = request->hostname != NULL
                             ? request->hostname
                             : request->server->server_hostname;
      const char *origin =
          apr_psprintf(request->pool, "%s://%s", ap_http_scheme(request), host);
      unsigned char *selected = context->capture;
      size_t selected_length = context->capture_length;
      apr_bucket_brigade *replacement;
      if (laghu_runtime_rewrite_css(
              &context->config->queue,
              context->config->image_cache != NULL
                  ? context->config->image_cache
                  : LAGHU_DEFAULT_CACHE,
              (laghu_buffer){context->capture, context->capture_length},
              request->uri, origin, context->policy_key,
              context->config->queue.capabilities,
              (uint64_t)apr_time_sec(apr_time_now()),
              context->config->core.image_metadata_ttl,
              (context->policy.filter_families & LAGHU_FILTER_CSS_MINIFY) != 0U,
              context->policy.allow_structural_rewrite,
              context->config->core.css_inline_limit,
              context->config->core.css_outline_threshold, &rewritten)) {
        if (rewritten.rewritten) {
          selected =
              apr_pmemdup(request->pool, rewritten.data, rewritten.length);
          if (selected != NULL) {
            selected_length = rewritten.length;
            apr_table_set(request->headers_out, "ETag",
                          apr_psprintf(request->pool, "\"laghu-css-%s\"",
                                       rewritten.dependency_key));
            apr_table_unset(request->headers_out, "Content-MD5");
            apr_table_unset(request->headers_out, "Digest");
          }
        }
        laghu_runtime_css_result_release(&rewritten);
      }
      ap_set_content_length(request, (apr_off_t)selected_length);
      replacement =
          apr_brigade_create(request->pool, request->connection->bucket_alloc);
      if (replacement == NULL) {
        return ap_pass_brigade(filter->next, brigade);
      }
      APR_BRIGADE_INSERT_TAIL(
          replacement, apr_bucket_pool_create(
                           (const char *)selected, selected_length,
                           request->pool, request->connection->bucket_alloc));
      APR_BRIGADE_INSERT_TAIL(
          replacement,
          apr_bucket_eos_create(request->connection->bucket_alloc));
      apr_brigade_cleanup(brigade);
      context->capture_enabled = false;
      return ap_pass_brigade(filter->next, replacement);
    }
    return ap_pass_brigade(filter->next, brigade);
  }
  if (context->html_capture) {
    if (!eos) {
      apr_brigade_cleanup(brigade);
      return APR_SUCCESS;
    }
    if (context->capture_enabled) {
      laghu_runtime_html_result rewritten;
      const char *host = request->hostname != NULL
                             ? request->hostname
                             : request->server->server_hostname;
      const char *origin =
          apr_psprintf(request->pool, "%s://%s", ap_http_scheme(request), host);
      const char *csp =
          apr_table_get(request->headers_out, "Content-Security-Policy");
      bool csp_allows_data = csp == NULL || ap_strcasestr(csp, "data:") != NULL;
      bool csp_allows_inline =
          csp == NULL || ap_strcasestr(csp, "'unsafe-inline'") != NULL;
      bool csp_allows_self = laghu_runtime_csp_allows_self_styles(csp, origin);
      unsigned char *selected = context->capture;
      size_t selected_length = context->capture_length;
      apr_bucket_brigade *replacement;
      if (laghu_runtime_rewrite_html(
              context->config->image_cache != NULL
                  ? context->config->image_cache
                  : LAGHU_DEFAULT_CACHE,
              (laghu_buffer){context->capture, context->capture_length},
              request->uri, origin, context->policy_key,
              context->config->queue.capabilities,
              (uint64_t)apr_time_sec(apr_time_now()),
              context->config->core.image_metadata_ttl, context->filters,
              context->policy.allow_resource_inlining,
              (context->policy.filter_families &
               LAGHU_FILTER_RESOURCE_INLINE) != 0U &&
                  context->policy.allow_resource_inlining,
              context->policy.allow_structural_rewrite,
              (context->policy.filter_families & LAGHU_FILTER_CSS_MINIFY) !=
                      0U &&
                  context->policy.allow_structural_rewrite,
              laghu_apache_html_plan(&context->policy), csp_allows_data,
              csp_allows_inline, csp_allows_self,
              context->config->core.image_beacon == LAGHU_MODE_ON,
              context->config->core.image_inline_limit,
              context->config->core.css_inline_limit,
              context->config->core.css_outline_threshold,
              laghu_apache_viewport_header(request),
              laghu_apache_dpr_header(request), &rewritten)) {
        laghu_runtime_html_result critical = {0};
        bool base_rewritten = rewritten.rewritten;
        char base_dependency[LAGHU_RUNTIME_KEY_SIZE];
        laghu_runtime_html_result finalized;
        laghu_runtime_html_result hinted;
        bool finalized_ok;
        bool hinted_ok;
        memcpy(base_dependency, rewritten.dependency_key,
               sizeof(base_dependency));
        if (rewritten.rewritten) {
          selected =
              apr_pmemdup(request->pool, rewritten.data, rewritten.length);
          if (selected != NULL) {
            selected_length = rewritten.length;
          }
        }
        if ((context->policy.filter_families & LAGHU_FILTER_CRITICAL_CSS) !=
                0U &&
            laghu_runtime_prioritize_critical_css(
                context->config->image_cache != NULL
                    ? context->config->image_cache
                    : LAGHU_DEFAULT_CACHE,
                (laghu_buffer){selected, selected_length}, request->uri, origin,
                context->policy_key, context->config->queue.capabilities,
                (uint64_t)apr_time_sec(apr_time_now()),
                context->config->core.image_metadata_ttl,
                context->config->core.css_inline_limit,
                context->config->core.css_outline_threshold,
                laghu_apache_viewport_header(request),
                context->config->core.critical_css_beacon == LAGHU_MODE_ON,
                csp_allows_inline, csp_allows_self,
                laghu_runtime_csp_allows_self_scripts(csp, origin),
                &critical) &&
            critical.rewritten) {
          selected = apr_pmemdup(request->pool, critical.data, critical.length);
          if (selected != NULL) {
            selected_length = critical.length;
            base_rewritten = true;
            {
              char material[LAGHU_RUNTIME_KEY_SIZE * 2U + 2U];
              int length = snprintf(material, sizeof(material), "%s\n%s",
                                    base_dependency, critical.dependency_key);
              if (length > 0 && (size_t)length < sizeof(material))
                (void)laghu_sha256_hex(
                    (laghu_buffer){(const unsigned char *)material,
                                   (size_t)length},
                    base_dependency);
            }
          }
        }
        laghu_runtime_html_result_release(&critical);
        laghu_runtime_html_result_release(&rewritten);
        hinted_ok = laghu_runtime_finalize_html_headers(
            context->config->image_cache != NULL ? context->config->image_cache
                                                 : LAGHU_DEFAULT_CACHE,
            (laghu_buffer){context->capture, context->capture_length},
            request->uri, origin, context->policy_key,
            context->config->queue.capabilities,
            (uint64_t)apr_time_sec(apr_time_now()),
            context->config->core.image_metadata_ttl,
            laghu_apache_html_plan(&context->policy) &
                LAGHU_HTML_PLAN_RESOURCE_HINTS,
            apr_table_get(request->headers_out, "Content-Language"),
            apr_table_get(request->headers_out, "Link"),
            context->config->core.css_inline_limit,
            context->config->core.css_outline_threshold, base_rewritten,
            &hinted);
        finalized_ok = laghu_runtime_finalize_html_headers(
            context->config->image_cache != NULL ? context->config->image_cache
                                                 : LAGHU_DEFAULT_CACHE,
            (laghu_buffer){selected, selected_length}, request->uri, origin,
            context->policy_key, context->config->queue.capabilities,
            (uint64_t)apr_time_sec(apr_time_now()),
            context->config->core.image_metadata_ttl,
            laghu_apache_html_plan(&context->policy) &
                ~LAGHU_HTML_PLAN_RESOURCE_HINTS,
            apr_table_get(request->headers_out, "Content-Language"),
            apr_table_get(request->headers_out, "Link"),
            context->config->core.css_inline_limit,
            context->config->core.css_outline_threshold, base_rewritten,
            &finalized);
        if (hinted_ok && finalized_ok) {
          const char *dependency = base_dependency;
          if (hinted.invalid || hinted.dependencies_pending ||
              finalized.invalid || finalized.dependencies_pending) {
            selected = context->capture;
            selected_length = context->capture_length;
          } else {
            unsigned int header_index;
            if (finalized.rewritten) {
              unsigned char *copy =
                  apr_pmemdup(request->pool, finalized.data, finalized.length);
              if (copy != NULL) {
                selected = copy;
                selected_length = finalized.length;
              }
            }
            for (header_index = 0U; header_index < hinted.link_header_count;
                 ++header_index) {
              memcpy(finalized.link_headers[header_index],
                     hinted.link_headers[header_index],
                     LAGHU_HTML_HEADER_VALUE_SIZE);
            }
            finalized.link_header_count = hinted.link_header_count;
            {
              char material[LAGHU_RUNTIME_KEY_SIZE * 3U + 4U];
              int material_length = snprintf(
                  material, sizeof(material), "%s\n%s\n%s", base_dependency,
                  hinted.dependency_key, finalized.dependency_key);
              if (material_length > 0 &&
                  (size_t)material_length < sizeof(material) &&
                  laghu_sha256_hex(
                      (laghu_buffer){(const unsigned char *)material,
                                     (size_t)material_length},
                      finalized.dependency_key)) {
                dependency = finalized.dependency_key;
              }
            }
            if (finalized.set_content_language) {
              apr_table_set(request->headers_out, "Content-Language",
                            finalized.content_language);
            }
            for (header_index = 0U; header_index < finalized.link_header_count;
                 ++header_index) {
              apr_table_add(request->headers_out, "Link",
                            finalized.link_headers[header_index]);
            }
            if ((base_rewritten || finalized.rewritten ||
                 finalized.set_content_language ||
                 finalized.link_header_count != 0U) &&
                dependency[0] != '\0') {
              apr_table_set(
                  request->headers_out, "ETag",
                  apr_psprintf(request->pool, "\"laghu-html-%s\"", dependency));
              apr_table_unset(request->headers_out, "Content-MD5");
              apr_table_unset(request->headers_out, "Digest");
            }
          }
          laghu_runtime_html_result_release(&hinted);
          laghu_runtime_html_result_release(&finalized);
        } else {
          laghu_runtime_html_result_release(&hinted);
          laghu_runtime_html_result_release(&finalized);
          selected = context->capture;
          selected_length = context->capture_length;
        }
      }
      ap_set_content_length(request, (apr_off_t)selected_length);
      replacement =
          apr_brigade_create(request->pool, request->connection->bucket_alloc);
      if (replacement == NULL) {
        return ap_pass_brigade(filter->next, brigade);
      }
      APR_BRIGADE_INSERT_TAIL(
          replacement, apr_bucket_pool_create(
                           (const char *)selected, selected_length,
                           request->pool, request->connection->bucket_alloc));
      APR_BRIGADE_INSERT_TAIL(
          replacement,
          apr_bucket_eos_create(request->connection->bucket_alloc));
      apr_brigade_cleanup(brigade);
      context->capture_enabled = false;
      return ap_pass_brigade(filter->next, replacement);
    }
    return ap_pass_brigade(filter->next, brigade);
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
      job.metadata_limit = context->config->core.image_metadata_limit;
      job.metadata_ttl = context->config->core.image_metadata_ttl;
      job.allow_lossy = context->policy.allow_lossy;
      job.accept_webp = context->accept_webp;
      job.target_count = context->target_count;
      memcpy(job.target_width, context->target_width, sizeof(job.target_width));
      memcpy(job.target_height, context->target_height,
             sizeof(job.target_height));
      memcpy(job.resize_filter, context->resize_filter,
             sizeof(job.resize_filter));
      job.payload = (laghu_buffer){context->capture, context->capture_length};
      (void)laghu_runtime_queue_try_publish(&context->config->queue, &job);
    }
    context->capture_enabled = false;
  }
  return ap_pass_brigade(filter->next, brigade);
}

static apr_status_t laghu_apache_transaction_filter(
    ap_filter_t *filter, apr_bucket_brigade *brigade) {
  request_rec *request = filter->r;
  laghu_apache_context *context = filter->ctx;
  apr_bucket *bucket;
  bool eos = false;
  if (context == NULL) {
    laghu_apache_config *server_config =
        ap_get_module_config(request->server->module_config, &laghu_module);
    laghu_apache_config *directory_config =
        ap_get_module_config(request->per_dir_config, &laghu_module);
    laghu_http_transaction_result prepared;
    bool ok;
    context = apr_pcalloc(request->pool, sizeof(*context));
    if (context == NULL) {
      ap_remove_output_filter(filter);
      return ap_pass_brigade(filter->next, brigade);
    }
    context->config = laghu_apache_merge_config(request->pool, server_config,
                                                directory_config);
    if (context->config == NULL || !laghu_apache_normalize(request, context)) {
      ap_remove_output_filter(filter);
      return ap_pass_brigade(filter->next, brigade);
    }
    ok = laghu_http_transaction_prepare(&context->transaction,
                                        &context->request, &context->response,
                                        &context->environment, &prepared);
    if (!laghu_apache_apply_result(request, &prepared)) {
      laghu_http_transaction_result_release(&prepared);
      ap_remove_output_filter(filter);
      return ap_pass_brigade(filter->next, brigade);
    }
    context->action = prepared.action;
    if (!ok || prepared.action == LAGHU_HTTP_ACTION_BYPASS) {
      laghu_http_transaction_result_release(&prepared);
      ap_remove_output_filter(filter);
      return ap_pass_brigade(filter->next, brigade);
    }
    if (prepared.action == LAGHU_HTTP_ACTION_SERVE_CACHED) {
      context->selected_body = apr_pmemdup(
          request->pool, prepared.selected.data, prepared.selected.length);
      context->selected_length = prepared.selected.length;
      if (context->selected_body == NULL) {
        laghu_http_transaction_result_release(&prepared);
        ap_remove_output_filter(filter);
        return ap_pass_brigade(filter->next, brigade);
      }
      context->cache_hit = true;
    } else {
      context->capture_capacity = prepared.capture_limit;
      if (context->response.has_declared_length &&
          context->response.declared_length < context->capture_capacity) {
        context->capture_capacity = context->response.declared_length;
      }
      context->capture = apr_palloc(request->pool, context->capture_capacity);
      context->capture_enabled = context->capture != NULL;
      if (!context->capture_enabled) {
        laghu_http_transaction_result_release(&prepared);
        ap_remove_output_filter(filter);
        return ap_pass_brigade(filter->next, brigade);
      }
    }
    laghu_http_transaction_result_release(&prepared);
    filter->ctx = context;
  }
  if (context->cache_hit) {
    apr_bucket_brigade *replacement;
    if (context->cache_sent) {
      apr_brigade_cleanup(brigade);
      return APR_SUCCESS;
    }
    replacement =
        apr_brigade_create(request->pool, request->connection->bucket_alloc);
    if (replacement == NULL) {
      return ap_pass_brigade(filter->next, brigade);
    }
    APR_BRIGADE_INSERT_TAIL(
        replacement,
        apr_bucket_pool_create((const char *)context->selected_body,
                               context->selected_length, request->pool,
                               request->connection->bucket_alloc));
    APR_BRIGADE_INSERT_TAIL(
        replacement, apr_bucket_eos_create(request->connection->bucket_alloc));
    context->cache_sent = true;
    apr_brigade_cleanup(brigade);
    ap_remove_output_filter(filter);
    return ap_pass_brigade(filter->next, replacement);
  }
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
        length > context->capture_capacity - context->capture_length) {
      context->capture_enabled = false;
      break;
    }
    memcpy(context->capture + context->capture_length, data, length);
    context->capture_length += length;
  }
  if (eos && context->capture_enabled) {
    laghu_http_transaction_result result;
    (void)laghu_http_transaction_finalize(
        &context->transaction,
        (laghu_buffer){context->capture, context->capture_length}, &result);
    if (context->action == LAGHU_HTTP_ACTION_CAPTURE_HTML ||
        context->action == LAGHU_HTTP_ACTION_CAPTURE_CSS ||
        context->action == LAGHU_HTTP_ACTION_CAPTURE_JAVASCRIPT) {
      apr_bucket_brigade *replacement;
      unsigned char *selected = apr_pmemdup(request->pool, result.selected.data,
                                            result.selected.length);
      if (selected == NULL || !laghu_apache_apply_result(request, &result)) {
        laghu_http_transaction_result_release(&result);
        ap_remove_output_filter(filter);
        return ap_pass_brigade(filter->next, brigade);
      }
      context->selected_length = result.selected.length;
      laghu_http_transaction_result_release(&result);
      replacement =
          apr_brigade_create(request->pool, request->connection->bucket_alloc);
      if (replacement == NULL) {
        return ap_pass_brigade(filter->next, brigade);
      }
      APR_BRIGADE_INSERT_TAIL(
          replacement, apr_bucket_pool_create(
                           (const char *)selected, context->selected_length,
                           request->pool, request->connection->bucket_alloc));
      APR_BRIGADE_INSERT_TAIL(
          replacement,
          apr_bucket_eos_create(request->connection->bucket_alloc));
      apr_brigade_cleanup(brigade);
      ap_remove_output_filter(filter);
      return ap_pass_brigade(filter->next, replacement);
    }
    laghu_http_transaction_result_release(&result);
    context->capture_enabled = false;
  }
  if (context->action == LAGHU_HTTP_ACTION_CAPTURE_HTML ||
      context->action == LAGHU_HTTP_ACTION_CAPTURE_CSS ||
      context->action == LAGHU_HTTP_ACTION_CAPTURE_JAVASCRIPT) {
    apr_bucket_brigade *metadata =
        apr_brigade_create(request->pool, request->connection->bucket_alloc);
    for (bucket = APR_BRIGADE_FIRST(brigade);
         bucket != APR_BRIGADE_SENTINEL(brigade);
         bucket = APR_BUCKET_NEXT(bucket)) {
      if (APR_BUCKET_IS_METADATA(bucket) && !APR_BUCKET_IS_EOS(bucket)) {
        apr_bucket *copy = NULL;
        if (apr_bucket_copy(bucket, &copy) == APR_SUCCESS) {
          APR_BRIGADE_INSERT_TAIL(metadata, copy);
        }
      }
    }
    apr_brigade_cleanup(brigade);
    if (!APR_BRIGADE_EMPTY(metadata)) {
      return ap_pass_brigade(filter->next, metadata);
    }
    return APR_SUCCESS;
  }
  if (eos) {
    ap_remove_output_filter(filter);
  }
  return ap_pass_brigade(filter->next, brigade);
}

static void laghu_apache_insert_filter(request_rec *request) {
  laghu_apache_config *server_config =
      ap_get_module_config(request->server->module_config, &laghu_module);
  laghu_apache_config *directory_config =
      ap_get_module_config(request->per_dir_config, &laghu_module);
  laghu_config effective;
  if (server_config == NULL || directory_config == NULL ||
      (request->uri != NULL &&
       strncmp(request->uri, "/.laghu/", sizeof("/.laghu/") - 1U) == 0)) {
    return;
  }
  laghu_config_merge(&effective, &server_config->core, &directory_config->core);
  if (effective.mode == LAGHU_MODE_ON) {
    ap_add_output_filter(LAGHU_APACHE_FILTER, NULL, request,
                         request->connection);
  }
}

static int laghu_apache_variant_handler(request_rec *request) {
  static const char prefix[] = "/.laghu/image/";
  static const char css_prefix[] = "/.laghu/css/";
  static const char javascript_prefix[] = "/.laghu/js/";
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
  if (request->uri == NULL ||
      strncmp(request->uri, "/.laghu/", sizeof("/.laghu/") - 1U) != 0) {
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
  if (!css_asset && !javascript_asset &&
      (strlen(request->uri) != sizeof(prefix) - 1U + LAGHU_SHA256_HEX_LENGTH ||
       strncmp(request->uri, prefix, sizeof(prefix) - 1U) != 0)) {
    return HTTP_NOT_FOUND;
  }
  if (request->method_number != M_GET) {
    return HTTP_METHOD_NOT_ALLOWED;
  }
  key = request->uri + (css_asset          ? sizeof(css_prefix) - 1U
                        : javascript_asset ? sizeof(javascript_prefix) - 1U
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
  ap_set_content_type(request, css_asset ? "text/css" : entry.content_type);
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

static void laghu_apache_register(apr_pool_t *pool) {
  (void)pool;
  ap_register_output_filter(LAGHU_APACHE_FILTER,
                            laghu_apache_transaction_filter, NULL,
                            AP_FTYPE_RESOURCE);
  ap_hook_insert_filter(laghu_apache_insert_filter, NULL, NULL,
                        APR_HOOK_MIDDLE);
  ap_hook_handler(laghu_apache_variant_handler, NULL, NULL, APR_HOOK_MIDDLE);
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
