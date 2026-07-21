// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_IMAGE_H
#define LAGHU_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_IMAGE_MAX_INPUT_BYTES (10U * 1024U * 1024U)
#define LAGHU_IMAGE_MAX_DIMENSION 8192U
#define LAGHU_IMAGE_MAX_PIXELS 64000000U
#define LAGHU_IMAGE_MAX_FRAMES 300U
#define LAGHU_IMAGE_BACKEND_ID_SIZE 128U
#define LAGHU_IMAGE_MAX_PAGE_RESOURCES 256U
#define LAGHU_IMAGE_URL_SIZE 2048U
#define LAGHU_IMAGE_MAX_TARGETS 2U
#define LAGHU_IMAGE_MAX_WIDTHS_PER_SOURCE 8U
#define LAGHU_IMAGE_DEFAULT_INLINE_LIMIT 2048U
#define LAGHU_IMAGE_PREVIEW_DIMENSION 24U

typedef uint64_t laghu_image_filter_mask;

enum {
  LAGHU_IMAGE_REWRITE_IMAGES = UINT64_C(1) << 0,
  LAGHU_IMAGE_RECOMPRESS_IMAGES = UINT64_C(1) << 1,
  LAGHU_IMAGE_RECOMPRESS_JPEG = UINT64_C(1) << 2,
  LAGHU_IMAGE_RECOMPRESS_PNG = UINT64_C(1) << 3,
  LAGHU_IMAGE_RECOMPRESS_WEBP = UINT64_C(1) << 4,
  LAGHU_IMAGE_JPEG_PROGRESSIVE = UINT64_C(1) << 5,
  LAGHU_IMAGE_JPEG_TO_WEBP = UINT64_C(1) << 6,
  LAGHU_IMAGE_PNG_TO_JPEG = UINT64_C(1) << 7,
  LAGHU_IMAGE_GIF_TO_PNG = UINT64_C(1) << 8,
  LAGHU_IMAGE_TO_WEBP_LOSSLESS = UINT64_C(1) << 9,
  LAGHU_IMAGE_TO_WEBP_ANIMATED = UINT64_C(1) << 10,
  LAGHU_IMAGE_JPEG_SAMPLING = UINT64_C(1) << 11,
  LAGHU_IMAGE_RESIZE_ATTRIBUTE = UINT64_C(1) << 12,
  LAGHU_IMAGE_RESIZE_RENDERED = UINT64_C(1) << 13,
  LAGHU_IMAGE_RESIZE_MOBILE = UINT64_C(1) << 14,
  LAGHU_IMAGE_RESPONSIVE = UINT64_C(1) << 15,
  LAGHU_IMAGE_RESPONSIVE_ZOOM = UINT64_C(1) << 16,
  LAGHU_IMAGE_INSERT_DIMENSIONS = UINT64_C(1) << 17,
  LAGHU_IMAGE_INLINE = UINT64_C(1) << 18,
  LAGHU_IMAGE_INLINE_PREVIEW = UINT64_C(1) << 19,
  LAGHU_IMAGE_DEDUP_INLINE = UINT64_C(1) << 20,
  LAGHU_IMAGE_SPRITE = UINT64_C(1) << 21,
  LAGHU_IMAGE_LAZYLOAD = UINT64_C(1) << 22,
  LAGHU_IMAGE_STRIP_METADATA = UINT64_C(1) << 23,
  LAGHU_IMAGE_STRIP_COLOR_PROFILE = UINT64_C(1) << 24,
  LAGHU_IMAGE_IN_PLACE_BROWSER = UINT64_C(1) << 25
};

#define LAGHU_IMAGE_FILTER_ALL ((UINT64_C(1) << 26) - UINT64_C(1))

typedef enum {
  LAGHU_IMAGE_FORMAT_UNKNOWN = 0,
  LAGHU_IMAGE_FORMAT_JPEG,
  LAGHU_IMAGE_FORMAT_PNG,
  LAGHU_IMAGE_FORMAT_GIF,
  LAGHU_IMAGE_FORMAT_WEBP
} laghu_image_format;

typedef uint32_t laghu_image_capability_mask;

enum {
  LAGHU_IMAGE_CAP_JPEG_LOAD = UINT32_C(1) << 0,
  LAGHU_IMAGE_CAP_JPEG_SAVE = UINT32_C(1) << 1,
  LAGHU_IMAGE_CAP_PNG_LOAD = UINT32_C(1) << 2,
  LAGHU_IMAGE_CAP_PNG_SAVE = UINT32_C(1) << 3,
  LAGHU_IMAGE_CAP_GIF_LOAD = UINT32_C(1) << 4,
  LAGHU_IMAGE_CAP_GIF_SAVE = UINT32_C(1) << 5,
  LAGHU_IMAGE_CAP_WEBP_LOAD = UINT32_C(1) << 6,
  LAGHU_IMAGE_CAP_WEBP_SAVE = UINT32_C(1) << 7,
  LAGHU_IMAGE_CAP_ANIMATION = UINT32_C(1) << 8
};

#define LAGHU_IMAGE_CAP_ALL ((UINT32_C(1) << 9) - UINT32_C(1))

typedef struct {
  bool available;
  laghu_image_capability_mask capabilities;
  char backend_id[LAGHU_IMAGE_BACKEND_ID_SIZE];
} laghu_image_backend;

typedef struct {
  laghu_buffer original;
  laghu_image_filter_mask filters;
  bool allow_lossy;
  bool accept_webp;
  unsigned int quality;
  unsigned int target_width;
  unsigned int target_height;
  laghu_image_filter_mask resize_filter;
  unsigned int max_input_bytes;
  unsigned int max_dimension;
  uint64_t max_pixels;
  unsigned int max_frames;
} laghu_image_request;

typedef struct {
  laghu_buffer original;
  laghu_buffer selected;
  unsigned char *owned_candidate;
  laghu_image_format input_format;
  laghu_image_format output_format;
  laghu_image_filter_mask applied_filters;
  unsigned int width;
  unsigned int height;
  unsigned int natural_width;
  unsigned int natural_height;
  unsigned int frames;
  bool used_candidate;
  bool backend_unavailable;
  bool input_rejected;
} laghu_image_result;

typedef struct {
  const char *source_url;
  const char *source_hash;
  const char *optimized_url;
  const char *responsive_1x_url;
  const char *responsive_2x_url;
  const char *inline_data_uri;
  const char *preview_data_uri;
  const char *sprite_url;
  unsigned int width;
  unsigned int height;
  unsigned int sprite_x;
  unsigned int sprite_y;
  unsigned int responsive_1x_width;
  unsigned int responsive_2x_width;
  unsigned int declared_width;
  unsigned int declared_height;
  bool above_fold;
  bool terminally_excluded;
  size_t inline_payload_length;
} laghu_image_resource;

typedef struct {
  const laghu_image_resource *resources;
  size_t resource_count;
  bool insert_dimensions;
  bool responsive;
  bool responsive_zoom;
  bool lazyload;
  bool inline_images;
  bool inline_previews;
  bool deduplicate_inline;
  bool sprites;
  bool csp_allows_data_images;
  bool enforce_bundle_gate;
  size_t inline_limit;
  size_t unique_variant_savings_1x;
  size_t unique_variant_savings_2x;
} laghu_image_markup_options;

typedef struct {
  unsigned char *data;
  size_t length;
  laghu_image_filter_mask applied_filters;
  char dependency_key[LAGHU_SHA256_HEX_SIZE];
} laghu_image_markup_result;

typedef struct {
  char source_url[LAGHU_IMAGE_URL_SIZE];
  unsigned int declared_width;
  unsigned int declared_height;
  bool has_loading;
  bool has_srcset;
  bool has_sizes;
  bool fetchpriority_high;
} laghu_image_discovery;

typedef struct {
  laghu_image_discovery resources[LAGHU_IMAGE_MAX_PAGE_RESOURCES];
  size_t resource_count;
  bool truncated;
} laghu_image_discovery_result;

typedef struct {
  unsigned int natural_width;
  unsigned int natural_height;
  unsigned int declared_width;
  unsigned int declared_height;
  unsigned int learned_width;
  unsigned int learned_height;
  unsigned int viewport_width;
  unsigned int dpr_hundredths;
  bool use_rendered_dimensions;
  bool use_mobile_dimensions;
} laghu_image_geometry_input;

typedef struct {
  unsigned int width[LAGHU_IMAGE_MAX_TARGETS];
  unsigned int height[LAGHU_IMAGE_MAX_TARGETS];
  unsigned int count;
} laghu_image_geometry_plan;

typedef struct {
  laghu_buffer original;
  unsigned int x;
  unsigned int y;
  unsigned int width;
  unsigned int height;
} laghu_image_sprite_item;

typedef struct {
  unsigned char *data;
  size_t length;
  laghu_image_format format;
  unsigned int width;
  unsigned int height;
} laghu_image_sprite_result;

void laghu_image_request_init(laghu_image_request *request);
bool laghu_image_backend_probe(laghu_image_backend *backend);
laghu_image_format laghu_image_detect_format(laghu_buffer input);
const char *laghu_image_format_name(laghu_image_format format);
const char *laghu_image_content_type(laghu_image_format format);
laghu_image_filter_mask laghu_image_effective_filters(
    laghu_image_filter_mask requested,
    laghu_image_capability_mask capabilities);
bool laghu_image_optimize(const laghu_image_backend *backend,
                          const laghu_image_request *request,
                          laghu_image_result *result);
bool laghu_image_variant_key(const laghu_image_backend *backend,
                             const laghu_image_request *request,
                             const char policy_key[LAGHU_SHA256_HEX_SIZE],
                             char output[LAGHU_SHA256_HEX_SIZE]);
void laghu_image_result_release(laghu_image_result *result);
bool laghu_image_rewrite_html(laghu_buffer input,
                              const laghu_image_markup_options *options,
                              laghu_image_markup_result *result);
bool laghu_image_discover_html(laghu_buffer input, const char *page_path,
                               const char *page_origin,
                               laghu_image_discovery_result *result);
bool laghu_image_variant_url(
    const char *hash,
    char output[sizeof("/.laghu/image/") + LAGHU_SHA256_HEX_SIZE]);
bool laghu_image_plan_geometry(const laghu_image_geometry_input *input,
                               laghu_image_geometry_plan *plan);
bool laghu_image_rewrite_css(laghu_buffer input,
                             const laghu_image_markup_options *options,
                             laghu_image_markup_result *result);
bool laghu_image_data_uri(laghu_image_format format, laghu_buffer input,
                          size_t max_input_bytes,
                          laghu_image_markup_result *result);
bool laghu_image_preview_data_uri(const laghu_image_backend *backend,
                                  const laghu_image_request *request,
                                  unsigned int max_dimension,
                                  laghu_image_markup_result *result);
bool laghu_image_build_sprite(const laghu_image_backend *backend,
                              laghu_image_sprite_item *items, size_t item_count,
                              laghu_image_format output_format,
                              laghu_image_sprite_result *result);
void laghu_image_sprite_result_release(laghu_image_sprite_result *result);
void laghu_image_markup_result_release(laghu_image_markup_result *result);

#ifdef __cplusplus
}
#endif

#endif
