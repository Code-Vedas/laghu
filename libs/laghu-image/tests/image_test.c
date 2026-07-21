// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/image.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#if LAGHU_HAVE_VIPS
#include <vips/vips.h>

typedef struct {
  void *data;
  size_t length;
} test_encoded_image;

static VipsImage *test_pattern(unsigned int width, unsigned int height,
                               bool alpha) {
  VipsImage *xy = NULL;
  VipsImage *x = NULL;
  VipsImage *y = NULL;
  VipsImage *joined = NULL;
  VipsImage *pixels = NULL;
  VipsImage *alpha_base = NULL;
  VipsImage *alpha_linear = NULL;
  VipsImage *alpha_band = NULL;
  VipsImage *with_alpha = NULL;
  VipsImage *bands[3];

  assert(vips_xyz(&xy, (int)width, (int)height, NULL) == 0);
  assert(vips_extract_band(xy, &x, 0, NULL) == 0);
  assert(vips_extract_band(xy, &y, 1, NULL) == 0);
  bands[0] = x;
  bands[1] = y;
  bands[2] = x;
  assert(vips_bandjoin(bands, &joined, 3, NULL) == 0);
  assert(vips_cast(joined, &pixels, VIPS_FORMAT_UCHAR, NULL) == 0);
  if (alpha) {
    assert(vips_black(&alpha_base, (int)width, (int)height, NULL) == 0);
    assert(vips_linear1(alpha_base, &alpha_linear, 1.0, 128.0, NULL) == 0);
    assert(vips_cast(alpha_linear, &alpha_band, VIPS_FORMAT_UCHAR, NULL) == 0);
    assert(vips_bandjoin2(pixels, alpha_band, &with_alpha, NULL) == 0);
  }
  g_object_unref(xy);
  g_object_unref(x);
  g_object_unref(y);
  g_object_unref(joined);
  if (alpha) {
    g_object_unref(alpha_base);
    g_object_unref(alpha_linear);
    g_object_unref(alpha_band);
    g_object_unref(pixels);
    return with_alpha;
  }
  return pixels;
}

static test_encoded_image test_encode(VipsImage *image,
                                      laghu_image_format format) {
  test_encoded_image encoded = {0};
  int status = -1;

  switch (format) {
    case LAGHU_IMAGE_FORMAT_JPEG:
      status = vips_jpegsave_buffer(image, &encoded.data, &encoded.length, "Q",
                                    100, "optimize_coding", FALSE, "interlace",
                                    FALSE, "subsample_mode",
                                    VIPS_FOREIGN_SUBSAMPLE_OFF, NULL);
      break;
    case LAGHU_IMAGE_FORMAT_PNG:
      status = vips_pngsave_buffer(image, &encoded.data, &encoded.length,
                                   "compression", 0, NULL);
      break;
    case LAGHU_IMAGE_FORMAT_GIF:
      status = vips_gifsave_buffer(image, &encoded.data, &encoded.length,
                                   "effort", 1, NULL);
      break;
    case LAGHU_IMAGE_FORMAT_WEBP:
      status = vips_webpsave_buffer(image, &encoded.data, &encoded.length,
                                    "lossless", TRUE, "effort", 0, NULL);
      break;
    default:
      break;
  }
  assert(status == 0);
  assert(encoded.data != NULL && encoded.length != 0U);
  return encoded;
}

static test_encoded_image test_static_image(laghu_image_format format,
                                            bool alpha) {
  VipsImage *image = test_pattern(512U, 384U, alpha);

  if (format == LAGHU_IMAGE_FORMAT_JPEG) {
    VipsBlob *profile = NULL;
    const void *profile_data;
    size_t profile_length;
    assert(vips_profile_load("srgb", &profile, NULL) == 0);
    profile_data = vips_blob_get(profile, &profile_length);
    assert(profile_data != NULL && profile_length != 0U);
    vips_image_set_blob_copy(image, VIPS_META_ICC_NAME, profile_data,
                             profile_length);
    vips_area_unref((VipsArea *)profile);
  }
  test_encoded_image encoded = test_encode(image, format);
  g_object_unref(image);
  return encoded;
}

static test_encoded_image test_opaque_alpha_png(void) {
  VipsImage *rgb = test_pattern(512U, 384U, false);
  VipsImage *alpha_base = NULL;
  VipsImage *alpha_linear = NULL;
  VipsImage *alpha = NULL;
  VipsImage *rgba = NULL;
  test_encoded_image encoded;

  assert(vips_black(&alpha_base, 512, 384, NULL) == 0);
  assert(vips_linear1(alpha_base, &alpha_linear, 1.0, 255.0, NULL) == 0);
  assert(vips_cast(alpha_linear, &alpha, VIPS_FORMAT_UCHAR, NULL) == 0);
  assert(vips_bandjoin2(rgb, alpha, &rgba, NULL) == 0);
  encoded = test_encode(rgba, LAGHU_IMAGE_FORMAT_PNG);
  g_object_unref(rgba);
  g_object_unref(alpha);
  g_object_unref(alpha_linear);
  g_object_unref(alpha_base);
  g_object_unref(rgb);
  return encoded;
}

static VipsImage *test_pattern_with_alpha(unsigned int width,
                                          unsigned int height,
                                          double alpha_value,
                                          double color_offset) {
  VipsImage *pattern = test_pattern(width, height, false);
  VipsImage *colored = NULL;
  VipsImage *alpha_base = NULL;
  VipsImage *alpha_linear = NULL;
  VipsImage *alpha = NULL;
  VipsImage *rgba = NULL;

  assert(vips_linear1(pattern, &colored, 1.0, color_offset, NULL) == 0);
  assert(vips_black(&alpha_base, (int)width, (int)height, NULL) == 0);
  assert(vips_linear1(alpha_base, &alpha_linear, 1.0, alpha_value, NULL) == 0);
  assert(vips_cast(alpha_linear, &alpha, VIPS_FORMAT_UCHAR, NULL) == 0);
  assert(vips_bandjoin2(colored, alpha, &rgba, NULL) == 0);
  g_object_unref(alpha);
  g_object_unref(alpha_linear);
  g_object_unref(alpha_base);
  g_object_unref(colored);
  g_object_unref(pattern);
  return rgba;
}

static test_encoded_image test_animated_gif(void) {
  VipsImage *first = test_pattern_with_alpha(256U, 192U, 0.0, 0.0);
  VipsImage *second = test_pattern_with_alpha(256U, 192U, 255.0, 20.0);
  VipsImage *pages = NULL;
  int delays[] = {80, 140};
  test_encoded_image encoded;

  assert(vips_join(first, second, &pages, VIPS_DIRECTION_VERTICAL, NULL) == 0);
  vips_image_set_int(pages, "page-height", 192);
  vips_image_set_array_int(pages, "delay", delays, 2);
  vips_image_set_int(pages, "loop", 3);
  encoded = test_encode(pages, LAGHU_IMAGE_FORMAT_GIF);
  g_object_unref(pages);
  g_object_unref(second);
  g_object_unref(first);
  return encoded;
}

static test_encoded_image test_many_frame_gif(unsigned int frame_count) {
  VipsImage *pages = NULL;
  VipsImage **frames = calloc(frame_count, sizeof(*frames));
  int *delays = calloc(frame_count, sizeof(*delays));
  test_encoded_image encoded;
  unsigned int index;

  assert(delays != NULL && frames != NULL);
  for (index = 0U; index < frame_count; ++index) {
    VipsImage *noise = NULL;
    delays[index] = 10;
    assert(vips_gaussnoise(&noise, 8, 8, "seed", (int)index + 1, "mean", 128.0,
                           "sigma", 40.0, NULL) == 0);
    assert(vips_cast(noise, &frames[index], VIPS_FORMAT_UCHAR, NULL) == 0);
    g_object_unref(noise);
  }
  assert(vips_arrayjoin(frames, &pages, (int)frame_count, "across", 1, NULL) ==
         0);
  vips_image_set_int(pages, "page-height", 8);
  vips_image_set_int(pages, "n-pages", (int)frame_count);
  vips_image_set_array_int(pages, "delay", delays, (int)frame_count);
  vips_image_set_int(pages, "loop", 0);
  encoded = test_encode(pages, LAGHU_IMAGE_FORMAT_GIF);
  g_object_unref(pages);
  for (index = 0U; index < frame_count; ++index) {
    g_object_unref(frames[index]);
  }
  free(frames);
  free(delays);
  return encoded;
}

static laghu_image_result test_optimize(const laghu_image_backend *backend,
                                        const test_encoded_image *input,
                                        laghu_image_filter_mask filters,
                                        bool allow_lossy, bool accept_webp,
                                        laghu_image_filter_mask resize_filter,
                                        unsigned int target_width) {
  laghu_image_request request;
  laghu_image_result result;

  laghu_image_request_init(&request);
  request.original =
      (laghu_buffer){(const unsigned char *)input->data, input->length};
  request.filters = filters;
  request.allow_lossy = allow_lossy;
  request.accept_webp = accept_webp;
  request.resize_filter = resize_filter;
  request.target_width = target_width;
  assert(laghu_image_optimize(backend, &request, &result));
  return result;
}

static bool test_jpeg_progressive_420(laghu_buffer jpeg) {
  size_t index;

  for (index = 0U; index + 12U < jpeg.length; ++index) {
    if (jpeg.data[index] == 0xffU && jpeg.data[index + 1U] == 0xc2U) {
      return jpeg.data[index + 11U] == 0x22U;
    }
  }
  return false;
}
#endif

static void test_format_detection(void) {
  static const unsigned char jpeg[] = {0xff, 0xd8, 0xff, 0xe0};
  static const unsigned char png[] = {0x89, 'P',  'N',  'G',
                                      0x0d, 0x0a, 0x1a, 0x0a};
  static const unsigned char gif[] = "GIF89a";
  static const unsigned char webp[] = {'R', 'I', 'F', 'F', 0,   0,
                                       0,   0,   'W', 'E', 'B', 'P'};

  assert(laghu_image_detect_format((laghu_buffer){jpeg, sizeof(jpeg)}) ==
         LAGHU_IMAGE_FORMAT_JPEG);
  assert(laghu_image_detect_format((laghu_buffer){png, sizeof(png)}) ==
         LAGHU_IMAGE_FORMAT_PNG);
  assert(laghu_image_detect_format((laghu_buffer){gif, sizeof(gif) - 1U}) ==
         LAGHU_IMAGE_FORMAT_GIF);
  assert(laghu_image_detect_format((laghu_buffer){webp, sizeof(webp)}) ==
         LAGHU_IMAGE_FORMAT_WEBP);
  assert(laghu_image_detect_format((laghu_buffer){NULL, 0U}) ==
         LAGHU_IMAGE_FORMAT_UNKNOWN);
  assert(strcmp(laghu_image_content_type(LAGHU_IMAGE_FORMAT_WEBP),
                "image/webp") == 0);
}

static void test_capability_filtering(void) {
  laghu_image_filter_mask effective;
  laghu_image_capability_mask capabilities =
      LAGHU_IMAGE_CAP_JPEG_LOAD | LAGHU_IMAGE_CAP_JPEG_SAVE;

  effective =
      laghu_image_effective_filters(LAGHU_IMAGE_FILTER_ALL, capabilities);
  assert((effective & LAGHU_IMAGE_RECOMPRESS_JPEG) != 0U);
  assert((effective & LAGHU_IMAGE_RECOMPRESS_PNG) == 0U);
  assert((effective & LAGHU_IMAGE_JPEG_TO_WEBP) == 0U);
  assert(laghu_image_effective_filters(LAGHU_IMAGE_FILTER_ALL, 0U) == 0U);
}

static void test_image_key_v4_vector(void) {
  laghu_image_backend backend = {.available = true, .capabilities = 0x1ffU};
  laghu_image_request request;
  char output[LAGHU_SHA256_HEX_SIZE];

  strcpy(backend.backend_id, "libvips-test-build");
  laghu_image_request_init(&request);
  request.original =
      (laghu_buffer){(const unsigned char *)"abc", strlen("abc")};
  request.filters = LAGHU_IMAGE_FILTER_ALL;
  request.quality = 82U;
  request.target_width = 320U;
  request.target_height = 180U;
  request.resize_filter = LAGHU_IMAGE_RESIZE_RENDERED;
  request.allow_lossy = true;
  request.accept_webp = true;
  assert(laghu_image_variant_key(
      &backend, &request,
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      output));
  assert(
      strcmp(
          output,
          "aec01b96de3b37f52795551341c455e37c3c090be75adcf2a1fbc9e2d56c6d9c") ==
      0);
}

static void test_backend_and_fail_open(void) {
  static const unsigned char malformed[] = {0xff, 0xd8, 0xff, 0x00};
  laghu_image_backend backend;
  laghu_image_request request;
  laghu_image_result result;

  assert(laghu_image_backend_probe(&backend));
  assert(backend.backend_id[0] != '\0');
  laghu_image_request_init(&request);
  request.original = (laghu_buffer){malformed, sizeof(malformed)};
  assert(laghu_image_optimize(&backend, &request, &result));
  assert(result.selected.data == request.original.data);
  assert(result.selected.length == request.original.length);
#if LAGHU_HAVE_VIPS
  assert(result.input_rejected);
#else
  assert(result.backend_unavailable);
#endif
  laghu_image_result_release(&result);
}

static void test_markup_filters(void) {
  static const unsigned char html[] = "<IMG SRC=/a.png><img src=\"/a.png\">";
  static const unsigned char css[] =
      ".a { background-image: url(/a.png); background-repeat: no-repeat; }";
  const laghu_image_resource resource = {
      .source_url = "/a.png",
      .source_hash =
          "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      .optimized_url = "/laghu/a.webp",
      .responsive_1x_url = "/laghu/a-1x.webp",
      .responsive_2x_url = "/laghu/a-2x.webp",
      .inline_data_uri = "data:image/webp;base64,AA==",
      .preview_data_uri = "data:image/webp;base64,AQ==",
      .sprite_url = "/laghu/sprite.webp",
      .width = 320U,
      .height = 180U,
      .sprite_x = 12U,
      .sprite_y = 8U,
      .responsive_1x_width = 320U,
      .responsive_2x_width = 640U,
      .declared_width = 320U,
      .inline_payload_length = 1U,
  };
  const laghu_image_markup_options options = {
      .resources = &resource,
      .resource_count = 1U,
      .insert_dimensions = true,
      .responsive = true,
      .responsive_zoom = true,
      .lazyload = true,
      .inline_images = true,
      .inline_previews = true,
      .deduplicate_inline = true,
      .sprites = true,
      .csp_allows_data_images = true,
      .inline_limit = LAGHU_IMAGE_DEFAULT_INLINE_LIMIT,
  };
  laghu_image_markup_result result;

  assert(laghu_image_rewrite_html((laghu_buffer){html, sizeof(html) - 1U},
                                  &options, &result));
  assert(strstr((char *)result.data, "width=\"320\"") != NULL);
  assert(strstr((char *)result.data, "height=\"180\"") != NULL);
  assert(strstr((char *)result.data, "srcset=") != NULL);
  assert(strstr((char *)result.data, "loading=\"lazy\"") != NULL);
  assert(strstr((char *)result.data, "data-laghu-preview=") != NULL);
  assert((result.applied_filters & LAGHU_IMAGE_INSERT_DIMENSIONS) != 0U);
  assert((result.applied_filters & LAGHU_IMAGE_RESPONSIVE_ZOOM) != 0U);
  assert((result.applied_filters & LAGHU_IMAGE_DEDUP_INLINE) != 0U);
  assert(result.dependency_key[0] != '\0');
  laghu_image_markup_result_release(&result);

  assert(laghu_image_rewrite_css((laghu_buffer){css, sizeof(css) - 1U},
                                 &options, &result));
  assert(strstr((char *)result.data, "/laghu/sprite.webp") != NULL);
  assert(strstr((char *)result.data, "background-position:-12px -8px") != NULL);
  assert((result.applied_filters & LAGHU_IMAGE_SPRITE) != 0U);
  assert(result.dependency_key[0] != '\0');
  laghu_image_markup_result_release(&result);

  assert(laghu_image_rewrite_html((laghu_buffer){NULL, 0U}, &options, &result));
  assert(result.length == 0U && result.dependency_key[0] != '\0');
  laghu_image_markup_result_release(&result);
}

static void test_html_discovery(void) {
  static const unsigned char html[] =
      "<!-- <img src='/ignored.png'> --><IMG SRC=\"hero.png\" WIDTH=320 "
      "height='180' fetchpriority=HIGH srcset='hero.png 1x' sizes=320px>"
      "<img src=/api/private.png>"
      "<img src=https://example.test/assets/logo.png loading=lazy>"
      "<img src=//cdn.example.test/a.png><img src=data:image/png,x>";
  laghu_image_discovery_result result;
  char url[sizeof("/.laghu/image/") + LAGHU_SHA256_HEX_SIZE];
  static const char hash[] =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

  assert(laghu_image_discover_html((laghu_buffer){html, sizeof(html) - 1U},
                                   "/pages/index.html", "https://example.test",
                                   &result));
  assert(result.resource_count == 2U && !result.truncated);
  assert(strcmp(result.resources[0].source_url, "/pages/hero.png") == 0);
  assert(result.resources[0].declared_width == 320U);
  assert(result.resources[0].declared_height == 180U);
  assert(result.resources[0].fetchpriority_high);
  assert(result.resources[0].has_srcset && result.resources[0].has_sizes);
  assert(strcmp(result.resources[1].source_url, "/assets/logo.png") == 0);
  assert(result.resources[1].has_loading);
  assert(laghu_image_variant_url(hash, url));
  assert(strcmp(url,
                "/.laghu/image/0123456789abcdef0123456789abcdef"
                "0123456789abcdef0123456789abcdef") == 0);
  assert(!laghu_image_variant_url("ABC", url));
}

static void test_geometry_planning(void) {
  laghu_image_geometry_input input = {
      .natural_width = 1000U,
      .natural_height = 500U,
      .declared_width = 320U,
      .declared_height = 160U,
      .dpr_hundredths = 300U,
  };
  laghu_image_geometry_plan plan;
  assert(laghu_image_plan_geometry(&input, &plan));
  assert(plan.count == 2U && plan.width[0] == 320U && plan.height[0] == 160U &&
         plan.width[1] == 640U && plan.height[1] == 320U);
  input.declared_width = 2000U;
  assert(laghu_image_plan_geometry(&input, &plan));
  assert(plan.count == 1U && plan.width[0] == 1000U);
  input.use_rendered_dimensions = true;
  input.learned_width = 240U;
  assert(laghu_image_plan_geometry(&input, &plan));
  assert(plan.width[0] == 240U && plan.width[1] == 480U);
  input.use_mobile_dimensions = true;
  input.learned_width = 0U;
  input.viewport_width = 180U;
  assert(laghu_image_plan_geometry(&input, &plan));
  assert(plan.width[0] == 180U && plan.width[1] == 360U);
}

#if LAGHU_HAVE_VIPS
static void test_byte_filters(void) {
  laghu_image_backend backend;
  laghu_image_result result;
  test_encoded_image jpeg;
  test_encoded_image png;
  test_encoded_image alpha_png;
  test_encoded_image opaque_alpha_png;
  test_encoded_image gif;
  test_encoded_image animated_gif;
  test_encoded_image webp;
  VipsImage *decoded = NULL;

  assert(vips_init("laghu-image-test") == 0);
  assert(laghu_image_backend_probe(&backend));
  assert(backend.available);
  jpeg = test_static_image(LAGHU_IMAGE_FORMAT_JPEG, false);
  png = test_static_image(LAGHU_IMAGE_FORMAT_PNG, false);
  alpha_png = test_static_image(LAGHU_IMAGE_FORMAT_PNG, true);
  opaque_alpha_png = test_opaque_alpha_png();
  gif = test_static_image(LAGHU_IMAGE_FORMAT_GIF, false);
  gif.data = g_realloc(gif.data, gif.length + 65536U);
  assert(gif.data != NULL);
  memset((unsigned char *)gif.data + gif.length, 0, 65536U);
  gif.length += 65536U;
  animated_gif = test_animated_gif();
  animated_gif.data =
      g_realloc(animated_gif.data, animated_gif.length + 65536U);
  assert(animated_gif.data != NULL);
  memset((unsigned char *)animated_gif.data + animated_gif.length, 0, 65536U);
  animated_gif.length += 65536U;
  webp = test_static_image(LAGHU_IMAGE_FORMAT_WEBP, false);

  assert(vips_gifload_buffer(animated_gif.data, animated_gif.length, &decoded,
                             "n", 2, NULL) == 0);
  assert(vips_image_get_n_pages(decoded) == 2);
  assert(vips_image_hasalpha(decoded));
  g_object_unref(decoded);
  decoded = NULL;

  assert(vips_jpegload_buffer(jpeg.data, jpeg.length, &decoded, NULL) == 0);
  assert(vips_image_get_typeof(decoded, "exif-data") != 0U);
  assert(vips_image_get_typeof(decoded, VIPS_META_ICC_NAME) != 0U);
  g_object_unref(decoded);
  decoded = NULL;

  result = test_optimize(
      &backend, &jpeg,
      LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_RECOMPRESS_IMAGES |
          LAGHU_IMAGE_RECOMPRESS_JPEG | LAGHU_IMAGE_JPEG_PROGRESSIVE |
          LAGHU_IMAGE_JPEG_SAMPLING | LAGHU_IMAGE_STRIP_METADATA |
          LAGHU_IMAGE_STRIP_COLOR_PROFILE,
      true, false, 0U, 0U);
  assert(result.used_candidate);
  assert(result.output_format == LAGHU_IMAGE_FORMAT_JPEG);
  assert((result.applied_filters & LAGHU_IMAGE_JPEG_PROGRESSIVE) != 0U);
  assert((result.applied_filters & LAGHU_IMAGE_JPEG_SAMPLING) != 0U);
  assert(test_jpeg_progressive_420(result.selected));
  assert(vips_jpegload_buffer((void *)result.selected.data,
                              result.selected.length, &decoded, NULL) == 0);
  assert(vips_image_get_typeof(decoded, "exif-data") == 0U);
  assert(vips_image_get_typeof(decoded, "icc-profile-data") == 0U);
  g_object_unref(decoded);
  decoded = NULL;
  laghu_image_result_release(&result);

  result = test_optimize(&backend, &jpeg, LAGHU_IMAGE_RECOMPRESS_IMAGES, true,
                         false, 0U, 0U);
  assert(!result.used_candidate);
  laghu_image_result_release(&result);

  result = test_optimize(
      &backend, &jpeg, LAGHU_IMAGE_JPEG_TO_WEBP | LAGHU_IMAGE_IN_PLACE_BROWSER,
      true, true, 0U, 0U);
  assert(result.used_candidate &&
         result.output_format == LAGHU_IMAGE_FORMAT_WEBP);
  assert((result.applied_filters & LAGHU_IMAGE_IN_PLACE_BROWSER) != 0U);
  laghu_image_result_release(&result);

  result =
      test_optimize(&backend, &png,
                    LAGHU_IMAGE_RECOMPRESS_IMAGES | LAGHU_IMAGE_RECOMPRESS_PNG,
                    false, false, 0U, 0U);
  assert(result.used_candidate &&
         result.output_format == LAGHU_IMAGE_FORMAT_PNG);
  laghu_image_result_release(&result);

  result = test_optimize(&backend, &png, LAGHU_IMAGE_PNG_TO_JPEG, true, false,
                         0U, 0U);
  assert(result.used_candidate &&
         result.output_format == LAGHU_IMAGE_FORMAT_JPEG);
  laghu_image_result_release(&result);

  result = test_optimize(&backend, &alpha_png, LAGHU_IMAGE_PNG_TO_JPEG, true,
                         false, 0U, 0U);
  assert(!result.used_candidate);
  laghu_image_result_release(&result);

  result = test_optimize(&backend, &opaque_alpha_png, LAGHU_IMAGE_PNG_TO_JPEG,
                         true, false, 0U, 0U);
  assert(result.used_candidate &&
         result.output_format == LAGHU_IMAGE_FORMAT_JPEG);
  laghu_image_result_release(&result);

  result = test_optimize(&backend, &gif, LAGHU_IMAGE_GIF_TO_PNG, false, false,
                         0U, 0U);
  assert(result.used_candidate &&
         result.output_format == LAGHU_IMAGE_FORMAT_PNG);
  laghu_image_result_release(&result);

  result = test_optimize(&backend, &alpha_png, LAGHU_IMAGE_TO_WEBP_LOSSLESS,
                         false, true, 0U, 0U);
  assert(result.used_candidate &&
         result.output_format == LAGHU_IMAGE_FORMAT_WEBP);
  laghu_image_result_release(&result);

  result = test_optimize(
      &backend, &animated_gif,
      LAGHU_IMAGE_TO_WEBP_ANIMATED | LAGHU_IMAGE_STRIP_METADATA |
          LAGHU_IMAGE_STRIP_COLOR_PROFILE | LAGHU_IMAGE_IN_PLACE_BROWSER,
      true, true, 0U, 0U);
  assert(result.used_candidate && result.frames == 2U &&
         result.output_format == LAGHU_IMAGE_FORMAT_WEBP);
  assert(vips_webpload_buffer((void *)result.selected.data,
                              result.selected.length, &decoded, "n", 2,
                              NULL) == 0);
  assert(vips_image_hasalpha(decoded));
  g_object_unref(decoded);
  decoded = NULL;
  laghu_image_result_release(&result);

  result = test_optimize(&backend, &webp, LAGHU_IMAGE_RECOMPRESS_WEBP, false,
                         true, 0U, 0U);
  assert(result.used_candidate &&
         result.output_format == LAGHU_IMAGE_FORMAT_WEBP);
  laghu_image_result_release(&result);

  result = test_optimize(&backend, &webp, LAGHU_IMAGE_RECOMPRESS_IMAGES, true,
                         true, 0U, 0U);
  assert(result.used_candidate &&
         (result.applied_filters & LAGHU_IMAGE_RECOMPRESS_IMAGES) != 0U);
  laghu_image_result_release(&result);

  g_free(webp.data);
  g_free(animated_gif.data);
  g_free(gif.data);
  g_free(alpha_png.data);
  g_free(opaque_alpha_png.data);
  g_free(png.data);
  g_free(jpeg.data);
}

static void test_geometry_inline_and_sprites(void) {
  laghu_image_backend backend;
  test_encoded_image png;
  laghu_image_result result;
  laghu_image_request request;
  laghu_image_markup_result uri;
  laghu_image_sprite_item items[2];
  laghu_image_sprite_result sprite;
  laghu_image_filter_mask resize_filters[] = {LAGHU_IMAGE_RESIZE_ATTRIBUTE,
                                              LAGHU_IMAGE_RESIZE_RENDERED,
                                              LAGHU_IMAGE_RESIZE_MOBILE};
  size_t index;

  assert(laghu_image_backend_probe(&backend));
  png = test_static_image(LAGHU_IMAGE_FORMAT_PNG, false);
  for (index = 0U; index < sizeof(resize_filters) / sizeof(resize_filters[0]);
       ++index) {
    result =
        test_optimize(&backend, &png,
                      LAGHU_IMAGE_RECOMPRESS_PNG | resize_filters[index] |
                          LAGHU_IMAGE_RESPONSIVE | LAGHU_IMAGE_RESPONSIVE_ZOOM,
                      false, false, resize_filters[index], 128U);
    assert(result.used_candidate && result.width == 128U);
    assert((result.applied_filters & resize_filters[index]) != 0U);
    assert((result.applied_filters & LAGHU_IMAGE_RESPONSIVE_ZOOM) != 0U);
    laghu_image_result_release(&result);
  }

  assert(laghu_image_data_uri(
      LAGHU_IMAGE_FORMAT_PNG,
      (laghu_buffer){(const unsigned char *)png.data, png.length}, png.length,
      &uri));
  assert(strncmp((char *)uri.data, "data:image/png;base64,", 22U) == 0);
  laghu_image_markup_result_release(&uri);

  laghu_image_request_init(&request);
  request.original =
      (laghu_buffer){(const unsigned char *)png.data, png.length};
  request.filters = LAGHU_IMAGE_RECOMPRESS_PNG;
  assert(laghu_image_preview_data_uri(&backend, &request, 24U, &uri));
  assert((uri.applied_filters & LAGHU_IMAGE_INLINE_PREVIEW) != 0U);
  laghu_image_markup_result_release(&uri);

  memset(items, 0, sizeof(items));
  items[0].original = request.original;
  items[1].original = request.original;
  assert(laghu_image_build_sprite(&backend, items, 2U, LAGHU_IMAGE_FORMAT_PNG,
                                  &sprite));
  assert(sprite.width == 1024U && sprite.height == 384U);
  assert(items[1].x == 512U);
  laghu_image_sprite_result_release(&sprite);
  g_free(png.data);
}

static void test_limits_capabilities_and_keys(void) {
  laghu_image_backend backend;
  laghu_image_backend partial;
  test_encoded_image png;
  test_encoded_image jpeg;
  test_encoded_image animated;
  test_encoded_image too_many_frames;
  laghu_image_request request;
  laghu_image_result result;
  char policy_key[LAGHU_SHA256_HEX_SIZE];
  char first_key[LAGHU_SHA256_HEX_SIZE];
  char second_key[LAGHU_SHA256_HEX_SIZE];

  assert(laghu_image_backend_probe(&backend));
  png = test_static_image(LAGHU_IMAGE_FORMAT_PNG, false);
  jpeg = test_static_image(LAGHU_IMAGE_FORMAT_JPEG, false);
  animated = test_animated_gif();
  too_many_frames = test_many_frame_gif(LAGHU_IMAGE_MAX_FRAMES + 1U);
  assert(laghu_sha256_hex((laghu_buffer){(const unsigned char *)"policy", 6U},
                          policy_key));

  laghu_image_request_init(&request);
  request.original =
      (laghu_buffer){(const unsigned char *)png.data, png.length};
  assert(laghu_image_variant_key(&backend, &request, policy_key, first_key));
  assert(laghu_image_variant_key(&backend, &request, policy_key, second_key));
  assert(strcmp(first_key, second_key) == 0);
  request.quality = 81U;
  assert(laghu_image_variant_key(&backend, &request, policy_key, second_key));
  assert(strcmp(first_key, second_key) != 0);
  request.resize_filter =
      LAGHU_IMAGE_RESIZE_ATTRIBUTE | LAGHU_IMAGE_RESIZE_RENDERED;
  assert(!laghu_image_variant_key(&backend, &request, policy_key, second_key));

  laghu_image_request_init(&request);
  request.original =
      (laghu_buffer){(const unsigned char *)png.data, png.length};
  request.max_input_bytes = (unsigned int)png.length - 1U;
  assert(laghu_image_optimize(&backend, &request, &result));
  assert(result.input_rejected &&
         result.selected.data == request.original.data);
  laghu_image_result_release(&result);

  laghu_image_request_init(&request);
  request.original = (laghu_buffer){(const unsigned char *)too_many_frames.data,
                                    too_many_frames.length};
  assert(laghu_image_optimize(&backend, &request, &result));
  assert(result.input_rejected && !result.used_candidate);
  laghu_image_result_release(&result);
  laghu_image_request_init(&request);
  request.original =
      (laghu_buffer){(const unsigned char *)png.data, png.length};
  request.max_input_bytes = LAGHU_IMAGE_MAX_INPUT_BYTES;
  request.max_dimension = 100U;
  assert(laghu_image_optimize(&backend, &request, &result));
  assert(result.input_rejected);
  laghu_image_result_release(&result);
  request.max_dimension = LAGHU_IMAGE_MAX_DIMENSION;
  request.max_pixels = 100U;
  assert(laghu_image_optimize(&backend, &request, &result));
  assert(result.input_rejected);
  laghu_image_result_release(&result);

  laghu_image_request_init(&request);
  request.original =
      (laghu_buffer){(const unsigned char *)animated.data, animated.length};
  request.max_frames = 1U;
  assert(laghu_image_optimize(&backend, &request, &result));
  assert(result.input_rejected);
  laghu_image_result_release(&result);

  laghu_image_request_init(&request);
  request.original =
      (laghu_buffer){(const unsigned char *)jpeg.data, jpeg.length};
  request.filters = LAGHU_IMAGE_RECOMPRESS_JPEG | LAGHU_IMAGE_JPEG_TO_WEBP;
  request.allow_lossy = false;
  request.accept_webp = true;
  assert(laghu_image_optimize(&backend, &request, &result));
  assert(!result.used_candidate &&
         result.selected.data == request.original.data);
  laghu_image_result_release(&result);

  partial = backend;
  partial.capabilities = LAGHU_IMAGE_CAP_PNG_LOAD;
  strcpy(partial.backend_id, "partial-png-loader");
  laghu_image_request_init(&request);
  request.original =
      (laghu_buffer){(const unsigned char *)png.data, png.length};
  request.filters = LAGHU_IMAGE_RECOMPRESS_PNG;
  assert(laghu_image_optimize(&partial, &request, &result));
  assert(!result.used_candidate && !result.backend_unavailable);
  laghu_image_result_release(&result);

  g_free(animated.data);
  g_free(too_many_frames.data);
  g_free(jpeg.data);
  g_free(png.data);
}
#endif

static void test_css_parser(void) {
  static const unsigned char css[] =
      "/*! license */ .hero { background-image: url('../img/hero.png'); "
      "background-repeat: no-repeat; color: red; } /* remove */\n"
      ".token { --space: 1  2; width: calc(100% - 2px); }"
      "/*# sourceMappingURL=site.css.map */";
  static const unsigned char malformed[] =
      ".a{background-image:url('/img/a.png');";
  static const unsigned char html[] =
      "<div style=\" color: red; background-image: url('../img/hero.png'); \""
      "></div>";
  const laghu_image_resource resource = {
      .source_url = "/img/hero.png",
      .source_hash =
          "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      .optimized_url =
          "/.laghu/image/"
          "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
  };
  const laghu_image_markup_options options = {
      .resources = &resource,
      .resource_count = 1U,
  };
  laghu_css_parse_result parsed;
  laghu_image_markup_result rewritten;

  assert(laghu_css_discover((laghu_buffer){css, sizeof(css) - 1U},
                            "/css/site.css", "https://example.test", &parsed));
  assert(parsed.valid && parsed.bounded && parsed.dependency_count == 1U);
  assert(strcmp(parsed.dependencies[0].source_url, "/img/hero.png") == 0);
  assert(parsed.dependencies[0].sprite_eligible);
  assert(laghu_css_minify_and_rewrite((laghu_buffer){css, sizeof(css) - 1U},
                                      "/css/site.css", "https://example.test",
                                      &options, false, &rewritten));
  assert(strstr((char *)rewritten.data, "/*! license */") != NULL);
  assert(strstr((char *)rewritten.data, "remove") == NULL);
  assert(strstr((char *)rewritten.data, "sourceMappingURL") != NULL);
  assert(strstr((char *)rewritten.data, "calc(100% - 2px)") != NULL);
  assert(strstr((char *)rewritten.data, "--space: 1  2;") != NULL);
  laghu_image_markup_result_release(&rewritten);

  assert(laghu_css_discover((laghu_buffer){malformed, sizeof(malformed) - 1U},
                            "/site.css", "https://example.test", &parsed));
  assert(!parsed.valid && parsed.dependency_count == 1U);
  assert(laghu_css_fallback_rewrite_urls(
      (laghu_buffer){malformed, sizeof(malformed) - 1U}, "/site.css",
      "https://example.test", &options, &rewritten));
  laghu_image_markup_result_release(&rewritten);

  assert(laghu_css_rewrite_style_attributes(
      (laghu_buffer){html, sizeof(html) - 1U}, "/pages/index.html",
      "https://example.test", &options, &rewritten));
  assert(strstr((char *)rewritten.data, "style=\"color:red;") != NULL);
  laghu_image_markup_result_release(&rewritten);
  assert(laghu_css_discover_style_attributes(
      (laghu_buffer){html, sizeof(html) - 1U}, "/pages/index.html",
      "https://example.test", &parsed));
  assert(parsed.valid && parsed.dependency_count == 1U);
}

int main(void) {
  test_format_detection();
  test_capability_filtering();
  test_image_key_v4_vector();
  test_backend_and_fail_open();
  test_markup_filters();
  test_html_discovery();
  test_geometry_planning();
  test_css_parser();
#if LAGHU_HAVE_VIPS
  test_byte_filters();
  test_geometry_inline_and_sprites();
  test_limits_capabilities_and_keys();
#endif
  puts("laghu_image_test: all tests passed");
  return 0;
}
