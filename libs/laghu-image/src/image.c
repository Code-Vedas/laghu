// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/image.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if LAGHU_HAVE_VIPS
#include <vips/vips.h>

#if VIPS_MAJOR_VERSION < 8 || (VIPS_MAJOR_VERSION == 8 && VIPS_MINOR_VERSION < 15)
#error "Laghu requires libvips 8.15 or newer"
#endif

#include "probe_fixtures.h"
#endif

#define LAGHU_IMAGE_ENCODER_OPTIONS                                 \
  "jpeg-optimize=1;png-compression=9;png-filter=all;webp-effort=4;" \
  "metadata-policy=v1;autorot=1;resize=lanczos3"

static bool laghu_image_filter_needs(laghu_image_filter_mask filter, laghu_image_capability_mask available, laghu_image_capability_mask required) {
  return (filter & LAGHU_IMAGE_FILTER_ALL) != 0U && (available & required) == required;
}

void laghu_image_request_init(laghu_image_request *request) {
  if (request == NULL) {
    return;
  }

  memset(request, 0, sizeof(*request));
  request->filters = LAGHU_IMAGE_FILTER_ALL;
  request->quality = 82U;
  request->max_input_bytes = LAGHU_IMAGE_MAX_INPUT_BYTES;
  request->max_dimension = LAGHU_IMAGE_MAX_DIMENSION;
  request->max_pixels = LAGHU_IMAGE_MAX_PIXELS;
  request->max_frames = LAGHU_IMAGE_MAX_FRAMES;
}

laghu_image_viewport_bucket laghu_image_viewport_bucket_for_width(unsigned int width) {
  if (width == 0U) return LAGHU_IMAGE_VIEWPORT_DESKTOP;
  if (width <= 767U) return LAGHU_IMAGE_VIEWPORT_MOBILE;
  if (width <= 1199U) return LAGHU_IMAGE_VIEWPORT_TABLET;
  return LAGHU_IMAGE_VIEWPORT_DESKTOP;
}

unsigned int laghu_image_quality_cap(unsigned int configured_quality, bool save_data) {
  unsigned int quality = configured_quality == 0U ? 82U : configured_quality;
  if (save_data && quality > 12U) quality -= 12U;
  return quality < 35U ? 35U : quality;
}

unsigned int laghu_image_adaptive_quality(laghu_image_content_class content, unsigned int configured_quality, bool save_data) {
  static const unsigned int presets[] = {82U, 86U, 84U, 90U};
  unsigned int quality = configured_quality == 0U ? 82U : configured_quality;
  if (content <= LAGHU_IMAGE_CONTENT_FLAT_COLOR && presets[content] < quality) quality = presets[content];
  return laghu_image_quality_cap(quality, save_data);
}

bool laghu_image_classify(laghu_buffer input, laghu_image_content_class *output) {
  if (output == NULL || input.data == NULL || input.length == 0U || input.length > LAGHU_IMAGE_MAX_INPUT_BYTES) return false;
#if LAGHU_HAVE_VIPS
  VipsImage *thumbnail = NULL;
  double deviation = 0.0;
  if (vips_thumbnail_buffer((void *)(uintptr_t)input.data, input.length, &thumbnail, 64, "height", 64, "size", VIPS_SIZE_DOWN, NULL) != 0 ||
      vips_deviate(thumbnail, &deviation, NULL) != 0) {
    if (thumbnail != NULL) g_object_unref(thumbnail);
    vips_error_clear();
    return false;
  }
  g_object_unref(thumbnail);
  /* The bounded thumbnail avoids source-size-dependent work. Low variance is
   * flat-color; high variance with few source pixels is screenshot-like. */
  if (deviation < 8.0) *output = LAGHU_IMAGE_CONTENT_FLAT_COLOR;
  else if (input.length < 65536U && deviation > 45.0) *output = LAGHU_IMAGE_CONTENT_SCREENSHOT;
  else if (deviation < 24.0) *output = LAGHU_IMAGE_CONTENT_ILLUSTRATION;
  else *output = LAGHU_IMAGE_CONTENT_PHOTO;
  return true;
#else
  (void)input;
  return false;
#endif
}

laghu_image_format laghu_image_detect_format(laghu_buffer input) {
  const unsigned char *bytes = input.data;

  if ((bytes == NULL && input.length != 0U) || input.length < 2U) {
    return LAGHU_IMAGE_FORMAT_UNKNOWN;
  }
  if (bytes[0] == 0xffU && bytes[1] == 0x0aU) {
    return LAGHU_IMAGE_FORMAT_JXL;
  }
  if (input.length >= 3U && bytes[0] == 0xffU && bytes[1] == 0xd8U && bytes[2] == 0xffU) {
    return LAGHU_IMAGE_FORMAT_JPEG;
  }
  if (input.length >= 8U && bytes[0] == 0x89U && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G' && bytes[4] == 0x0dU && bytes[5] == 0x0aU &&
      bytes[6] == 0x1aU && bytes[7] == 0x0aU) {
    return LAGHU_IMAGE_FORMAT_PNG;
  }
  if (input.length >= 6U && (memcmp(bytes, "GIF87a", 6U) == 0 || memcmp(bytes, "GIF89a", 6U) == 0)) {
    return LAGHU_IMAGE_FORMAT_GIF;
  }
  if (input.length >= 12U && memcmp(bytes, "RIFF", 4U) == 0 && memcmp(bytes + 8U, "WEBP", 4U) == 0) {
    return LAGHU_IMAGE_FORMAT_WEBP;
  }
  if (input.length >= 12U && memcmp(bytes + 4U, "ftyp", 4U) == 0 && (memcmp(bytes + 8U, "avif", 4U) == 0 || memcmp(bytes + 8U, "avis", 4U) == 0)) {
    return LAGHU_IMAGE_FORMAT_AVIF;
  }
  if (input.length >= 12U && memcmp(bytes, "\0\0\0\fJXL \r\n\x87\n", 12U) == 0) {
    return LAGHU_IMAGE_FORMAT_JXL;
  }
  return LAGHU_IMAGE_FORMAT_UNKNOWN;
}

const char *laghu_image_format_name(laghu_image_format format) {
  switch (format) {
    case LAGHU_IMAGE_FORMAT_JPEG:
      return "jpeg";
    case LAGHU_IMAGE_FORMAT_PNG:
      return "png";
    case LAGHU_IMAGE_FORMAT_GIF:
      return "gif";
    case LAGHU_IMAGE_FORMAT_WEBP:
      return "webp";
    case LAGHU_IMAGE_FORMAT_AVIF:
      return "avif";
    case LAGHU_IMAGE_FORMAT_JXL:
      return "jxl";
    case LAGHU_IMAGE_FORMAT_UNKNOWN:
    default:
      return "unknown";
  }
}

const char *laghu_image_content_type(laghu_image_format format) {
  switch (format) {
    case LAGHU_IMAGE_FORMAT_JPEG:
      return "image/jpeg";
    case LAGHU_IMAGE_FORMAT_PNG:
      return "image/png";
    case LAGHU_IMAGE_FORMAT_GIF:
      return "image/gif";
    case LAGHU_IMAGE_FORMAT_WEBP:
      return "image/webp";
    case LAGHU_IMAGE_FORMAT_AVIF:
      return "image/avif";
    case LAGHU_IMAGE_FORMAT_JXL:
      return "image/jxl";
    case LAGHU_IMAGE_FORMAT_UNKNOWN:
    default:
      return "application/octet-stream";
  }
}

laghu_image_filter_mask laghu_image_effective_filters(laghu_image_filter_mask requested, laghu_image_capability_mask capabilities) {
  laghu_image_filter_mask effective = requested & LAGHU_IMAGE_FILTER_ALL;

  if (!laghu_image_filter_needs(requested, capabilities, LAGHU_IMAGE_CAP_JPEG_LOAD | LAGHU_IMAGE_CAP_JPEG_SAVE)) {
    effective &= ~(LAGHU_IMAGE_RECOMPRESS_JPEG | LAGHU_IMAGE_JPEG_PROGRESSIVE | LAGHU_IMAGE_JPEG_SAMPLING);
  }
  if (!laghu_image_filter_needs(requested, capabilities, LAGHU_IMAGE_CAP_PNG_LOAD | LAGHU_IMAGE_CAP_PNG_SAVE)) {
    effective &= ~LAGHU_IMAGE_RECOMPRESS_PNG;
  }
  if (!laghu_image_filter_needs(requested, capabilities, LAGHU_IMAGE_CAP_WEBP_LOAD | LAGHU_IMAGE_CAP_WEBP_SAVE)) {
    effective &= ~LAGHU_IMAGE_RECOMPRESS_WEBP;
  }
  if ((capabilities & (LAGHU_IMAGE_CAP_JPEG_LOAD | LAGHU_IMAGE_CAP_WEBP_SAVE)) != (LAGHU_IMAGE_CAP_JPEG_LOAD | LAGHU_IMAGE_CAP_WEBP_SAVE)) {
    effective &= ~LAGHU_IMAGE_JPEG_TO_WEBP;
  }
  if ((capabilities & (LAGHU_IMAGE_CAP_PNG_LOAD | LAGHU_IMAGE_CAP_JPEG_SAVE)) != (LAGHU_IMAGE_CAP_PNG_LOAD | LAGHU_IMAGE_CAP_JPEG_SAVE)) {
    effective &= ~LAGHU_IMAGE_PNG_TO_JPEG;
  }
  if ((capabilities & (LAGHU_IMAGE_CAP_GIF_LOAD | LAGHU_IMAGE_CAP_PNG_SAVE)) != (LAGHU_IMAGE_CAP_GIF_LOAD | LAGHU_IMAGE_CAP_PNG_SAVE)) {
    effective &= ~LAGHU_IMAGE_GIF_TO_PNG;
  }
  if ((capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) == 0U) {
    effective &= ~(LAGHU_IMAGE_TO_WEBP_LOSSLESS | LAGHU_IMAGE_TO_WEBP_ANIMATED);
  }
  if ((capabilities & LAGHU_IMAGE_CAP_ANIMATION) == 0U) {
    effective &= ~LAGHU_IMAGE_TO_WEBP_ANIMATED;
  }
  if ((capabilities & (LAGHU_IMAGE_CAP_JPEG_LOAD | LAGHU_IMAGE_CAP_PNG_LOAD | LAGHU_IMAGE_CAP_GIF_LOAD | LAGHU_IMAGE_CAP_WEBP_LOAD)) == 0U) {
    effective = 0U;
  }
  return effective;
}

bool laghu_image_variant_key(const laghu_image_backend *backend, const laghu_image_request *request, const char policy_key[LAGHU_SHA256_HEX_SIZE],
                             char output[LAGHU_SHA256_HEX_SIZE]) {
  char source_hash[LAGHU_SHA256_HEX_SIZE];
  char canonical[768];
  int length;

  if (backend == NULL || request == NULL || policy_key == NULL || output == NULL || backend->backend_id[0] == '\0' || request->quality == 0U ||
      request->quality > 100U ||
      (request->resize_filter & ~(LAGHU_IMAGE_RESIZE_ATTRIBUTE | LAGHU_IMAGE_RESIZE_RENDERED | LAGHU_IMAGE_RESIZE_MOBILE)) != 0U ||
      (request->resize_filter != 0U && (request->resize_filter & (request->resize_filter - 1U)) != 0U) ||
      !laghu_sha256_hex(request->original, source_hash)) {
    if (output != NULL) {
      output[0] = '\0';
    }
    return false;
  }
  length =
      snprintf(canonical, sizeof(canonical),
               "laghu-image\n%s\n%s\n%s\n%s\n%08x\n%016llx\n%u\n%u\n%u\n%"
               "016llx\n%d\n%d\n%d\n%d",
               source_hash, policy_key, LAGHU_IMAGE_ENCODER_OPTIONS, backend->backend_id, backend->capabilities,
               (unsigned long long)(request->filters & LAGHU_IMAGE_FILTER_ALL), request->quality, request->target_width, request->target_height,
               (unsigned long long)request->resize_filter, request->allow_lossy ? 1 : 0, request->accept_webp ? 1 : 0, request->accept_avif ? 1 : 0,
               request->accept_jxl ? 1 : 0);
  if (length <= 0 || (size_t)length >= sizeof(canonical)) {
    output[0] = '\0';
    return false;
  }
  return laghu_sha256_hex((laghu_buffer){(const unsigned char *)canonical, (size_t)length}, output);
}

#if LAGHU_HAVE_VIPS

static bool laghu_vips_has_operation(const char *name) { return vips_type_find("VipsOperation", name) != 0U; }

static void laghu_vips_probe_codec(laghu_image_format format, const char *load_operation, const char *save_operation, const unsigned char *fixture,
                                   size_t fixture_length, bool *can_load, bool *can_save) {
  VipsImage *source = NULL;
  VipsImage *decoded = NULL;
  void *encoded = NULL;
  size_t encoded_length = 0U;
  bool load_registered = laghu_vips_has_operation(load_operation);
  bool save_registered = laghu_vips_has_operation(save_operation);
  int save_status = -1;
  int load_status = -1;

  *can_load = false;
  *can_save = false;
  switch (format) {
    case LAGHU_IMAGE_FORMAT_JPEG:
      if (load_registered) {
        load_status = vips_jpegload_buffer((void *)(uintptr_t)fixture, fixture_length, &decoded, NULL);
      }
      break;
    case LAGHU_IMAGE_FORMAT_PNG:
      if (load_registered) {
        load_status = vips_pngload_buffer((void *)(uintptr_t)fixture, fixture_length, &decoded, NULL);
      }
      break;
    case LAGHU_IMAGE_FORMAT_GIF:
      if (load_registered) {
        load_status = vips_gifload_buffer((void *)(uintptr_t)fixture, fixture_length, &decoded, NULL);
      }
      break;
    case LAGHU_IMAGE_FORMAT_WEBP:
      if (load_registered) {
        load_status = vips_webpload_buffer((void *)(uintptr_t)fixture, fixture_length, &decoded, NULL);
      }
      break;
    case LAGHU_IMAGE_FORMAT_UNKNOWN:
    default:
      break;
  }
  *can_load = load_status == 0;
  if (decoded != NULL) {
    g_object_unref(decoded);
    decoded = NULL;
  }
  if (save_registered && vips_black(&source, 2, 2, "bands", 3, NULL) == 0) {
    switch (format) {
      case LAGHU_IMAGE_FORMAT_JPEG:
        save_status = vips_jpegsave_buffer(source, &encoded, &encoded_length, NULL);
        break;
      case LAGHU_IMAGE_FORMAT_PNG:
        save_status = vips_pngsave_buffer(source, &encoded, &encoded_length, NULL);
        break;
      case LAGHU_IMAGE_FORMAT_GIF:
        save_status = vips_gifsave_buffer(source, &encoded, &encoded_length, NULL);
        break;
      case LAGHU_IMAGE_FORMAT_WEBP:
        save_status = vips_webpsave_buffer(source, &encoded, &encoded_length, NULL);
        break;
      case LAGHU_IMAGE_FORMAT_UNKNOWN:
      default:
        break;
    }
  }
  *can_save = save_status == 0;
  if (source != NULL) {
    g_object_unref(source);
  }
  g_free(encoded);
  vips_error_clear();
}

static bool laghu_vips_probe_avif(void) {
  VipsImage *source = NULL;
  VipsImage *decoded = NULL;
  void *encoded = NULL;
  size_t encoded_length = 0U;
  bool available = false;
  if (!laghu_vips_has_operation("heifsave_buffer") || !laghu_vips_has_operation("heifload_buffer") ||
      vips_black(&source, 2, 2, "bands", 3, NULL) != 0 || vips_image_write_to_buffer(source, ".avif", &encoded, &encoded_length, NULL) != 0 ||
      (decoded = vips_image_new_from_buffer(encoded, encoded_length, "", NULL)) == NULL) {
    vips_error_clear();
    goto done;
  }
  available = true;
done:
  if (decoded != NULL) g_object_unref(decoded);
  if (source != NULL) g_object_unref(source);
  g_free(encoded);
  return available;
}

static bool laghu_vips_probe_jxl(void) {
  VipsImage *source = NULL;
  VipsImage *decoded = NULL;
  void *encoded = NULL;
  size_t encoded_length = 0U;
  bool available = false;
  if (!laghu_vips_has_operation("jxlsave_buffer") || !laghu_vips_has_operation("jxlload_buffer") ||
      vips_black(&source, 2, 2, "bands", 3, NULL) != 0 || vips_image_write_to_buffer(source, ".jxl", &encoded, &encoded_length, NULL) != 0 ||
      (decoded = vips_image_new_from_buffer(encoded, encoded_length, "", NULL)) == NULL) {
    vips_error_clear();
    goto done;
  }
  available = true;
done:
  if (decoded != NULL) g_object_unref(decoded);
  if (source != NULL) g_object_unref(source);
  g_free(encoded);
  return available;
}

static bool laghu_vips_probe_animation(void) {
  VipsImage *gif = NULL;
  VipsImage *webp = NULL;
  void *webp_bytes = NULL;
  size_t webp_length = 0U;
  int *gif_delays = NULL;
  int *webp_delays = NULL;
  int gif_delay_count = 0;
  int webp_delay_count = 0;
  int gif_loop = 0;
  int webp_loop = 0;
  bool success = false;

  if (vips_gifload_buffer((void *)(uintptr_t)laghu_probe_animated_gif, sizeof(laghu_probe_animated_gif) - 1U, &gif, "n", 2, NULL) != 0 ||
      vips_webpsave_buffer(gif, &webp_bytes, &webp_length, "lossless", TRUE, NULL) != 0 ||
      vips_webpload_buffer(webp_bytes, webp_length, &webp, "n", 2, NULL) != 0) {
    goto cleanup;
  }
  success = vips_image_get_n_pages(gif) == 2 && vips_image_get_n_pages(webp) == 2 && vips_image_hasalpha(gif) && vips_image_hasalpha(webp) &&
            vips_image_get_array_int(gif, "delay", &gif_delays, &gif_delay_count) == 0 &&
            vips_image_get_array_int(webp, "delay", &webp_delays, &webp_delay_count) == 0 && gif_delay_count == webp_delay_count &&
            gif_delay_count == 2 && memcmp(gif_delays, webp_delays, (size_t)gif_delay_count * sizeof(*gif_delays)) == 0 &&
            vips_image_get_int(gif, "loop", &gif_loop) == 0 && vips_image_get_int(webp, "loop", &webp_loop) == 0 && gif_loop == webp_loop;

cleanup:
  if (webp != NULL) {
    g_object_unref(webp);
  }
  if (gif != NULL) {
    g_object_unref(gif);
  }
  g_free(webp_bytes);
  vips_error_clear();
  return success;
}

bool laghu_image_backend_probe(laghu_image_backend *backend) {
  bool jpeg_load;
  bool jpeg_save;
  bool png_load;
  bool png_save;
  bool gif_load;
  bool gif_save;
  bool webp_load;
  bool webp_save;
  bool avif;
  bool jxl;

  if (backend == NULL) {
    return false;
  }
  memset(backend, 0, sizeof(*backend));
  if (vips_init("laghu-libvips") != 0) {
    vips_error_clear();
    return true;
  }

  laghu_vips_probe_codec(LAGHU_IMAGE_FORMAT_JPEG, "jpegload_buffer", "jpegsave_buffer", laghu_probe_jpeg, sizeof(laghu_probe_jpeg) - 1U, &jpeg_load,
                         &jpeg_save);
  laghu_vips_probe_codec(LAGHU_IMAGE_FORMAT_PNG, "pngload_buffer", "pngsave_buffer", laghu_probe_png, sizeof(laghu_probe_png) - 1U, &png_load,
                         &png_save);
  laghu_vips_probe_codec(LAGHU_IMAGE_FORMAT_GIF, "gifload_buffer", "gifsave_buffer", laghu_probe_gif, sizeof(laghu_probe_gif) - 1U, &gif_load,
                         &gif_save);
  laghu_vips_probe_codec(LAGHU_IMAGE_FORMAT_WEBP, "webpload_buffer", "webpsave_buffer", laghu_probe_webp, sizeof(laghu_probe_webp) - 1U, &webp_load,
                         &webp_save);
  avif = laghu_vips_probe_avif();
  jxl = laghu_vips_probe_jxl();

  if (jpeg_load) {
    backend->capabilities |= LAGHU_IMAGE_CAP_JPEG_LOAD;
  }
  if (jpeg_save) {
    backend->capabilities |= LAGHU_IMAGE_CAP_JPEG_SAVE;
  }
  if (png_load) {
    backend->capabilities |= LAGHU_IMAGE_CAP_PNG_LOAD;
  }
  if (png_save) {
    backend->capabilities |= LAGHU_IMAGE_CAP_PNG_SAVE;
  }
  if (gif_load) {
    backend->capabilities |= LAGHU_IMAGE_CAP_GIF_LOAD;
  }
  if (gif_save) {
    backend->capabilities |= LAGHU_IMAGE_CAP_GIF_SAVE;
  }
  if (webp_load) {
    backend->capabilities |= LAGHU_IMAGE_CAP_WEBP_LOAD;
  }
  if (webp_save) {
    backend->capabilities |= LAGHU_IMAGE_CAP_WEBP_SAVE;
  }
  if (avif) {
    backend->capabilities |= LAGHU_IMAGE_CAP_AVIF_LOAD | LAGHU_IMAGE_CAP_AVIF_SAVE;
  }
  if (jxl) {
    backend->capabilities |= LAGHU_IMAGE_CAP_JXL_SAVE;
  }
  if ((backend->capabilities & (LAGHU_IMAGE_CAP_GIF_LOAD | LAGHU_IMAGE_CAP_WEBP_SAVE)) == (LAGHU_IMAGE_CAP_GIF_LOAD | LAGHU_IMAGE_CAP_WEBP_SAVE) &&
      webp_load && laghu_vips_probe_animation()) {
    backend->capabilities |= LAGHU_IMAGE_CAP_ANIMATION;
  }
  backend->available = backend->capabilities != 0U;
  (void)snprintf(backend->backend_id, sizeof(backend->backend_id), "laghu-libvips-" LAGHU_VERSION "-%d.%d.%d-cap-%08x", vips_version(0),
                 vips_version(1), vips_version(2), backend->capabilities);
  return true;
}

static int laghu_image_load(laghu_image_format format, laghu_buffer input, unsigned int max_frames, VipsImage **image) {
  void *bytes = (void *)(uintptr_t)input.data;
  VipsImage *first_page = NULL;
  int frames;
  int status;

  switch (format) {
    case LAGHU_IMAGE_FORMAT_JPEG:
      return vips_jpegload_buffer(bytes, input.length, image, "access", VIPS_ACCESS_RANDOM, NULL);
    case LAGHU_IMAGE_FORMAT_PNG:
      return vips_pngload_buffer(bytes, input.length, image, "access", VIPS_ACCESS_RANDOM, NULL);
    case LAGHU_IMAGE_FORMAT_GIF:
      status = vips_gifload_buffer(bytes, input.length, &first_page, "n", 1, "access", VIPS_ACCESS_RANDOM, NULL);
      break;
    case LAGHU_IMAGE_FORMAT_WEBP:
      status = vips_webpload_buffer(bytes, input.length, &first_page, "n", 1, "access", VIPS_ACCESS_RANDOM, NULL);
      break;
    case LAGHU_IMAGE_FORMAT_AVIF:
      *image = vips_image_new_from_buffer(bytes, input.length, "", "access", VIPS_ACCESS_RANDOM, NULL);
      return *image == NULL ? -1 : 0;
    case LAGHU_IMAGE_FORMAT_JXL:
      *image = vips_image_new_from_buffer(bytes, input.length, "", "access", VIPS_ACCESS_RANDOM, NULL);
      return *image == NULL ? -1 : 0;
    case LAGHU_IMAGE_FORMAT_UNKNOWN:
    default:
      return -1;
  }
  if (status != 0) {
    return status;
  }
  frames = vips_image_get_n_pages(first_page);
  if (frames <= 0) {
    frames = 1;
  }
  if ((unsigned int)frames > max_frames) {
    g_object_unref(first_page);
    return -1;
  }
  if (frames == 1) {
    *image = first_page;
    return 0;
  }
  g_object_unref(first_page);
  if (format == LAGHU_IMAGE_FORMAT_GIF) {
    return vips_gifload_buffer(bytes, input.length, image, "n", frames, "access", VIPS_ACCESS_RANDOM, NULL);
  }
  return vips_webpload_buffer(bytes, input.length, image, "n", frames, "access", VIPS_ACCESS_RANDOM, NULL);
}

static unsigned int laghu_image_frames(VipsImage *image) {
  int pages = vips_image_get_n_pages(image);
  return pages > 0 ? (unsigned int)pages : 1U;
}

static unsigned int laghu_image_page_height(VipsImage *image) {
  int page_height = vips_image_get_page_height(image);
  if (page_height <= 0) {
    page_height = vips_image_get_height(image);
  }
  return page_height > 0 ? (unsigned int)page_height : 0U;
}

static bool laghu_image_dimensions_valid(VipsImage *image, const laghu_image_request *request, unsigned int *width, unsigned int *height,
                                         unsigned int *frames) {
  uint64_t pixels;

  *width = (unsigned int)vips_image_get_width(image);
  *height = laghu_image_page_height(image);
  *frames = laghu_image_frames(image);
  pixels = (uint64_t)*width * (uint64_t)*height * (uint64_t)*frames;
  return *width > 0U && *height > 0U && *width <= request->max_dimension && *height <= request->max_dimension && *frames <= request->max_frames &&
         pixels <= request->max_pixels;
}

static bool laghu_image_is_opaque(VipsImage *image, laghu_image_format input_format) {
  VipsImage *alpha = NULL;
  double minimum = 0.0;
  double maximum = 0.0;
  double opaque_value;
  int bands = vips_image_get_bands(image);
  bool has_alpha = vips_image_hasalpha(image) || bands == 2 || (bands == 4 && input_format != LAGHU_IMAGE_FORMAT_JPEG);

  if (!has_alpha) {
    return true;
  }
  switch (vips_image_get_format(image)) {
    case VIPS_FORMAT_UCHAR:
      opaque_value = 255.0;
      break;
    case VIPS_FORMAT_USHORT:
      opaque_value = 65535.0;
      break;
    case VIPS_FORMAT_FLOAT:
    case VIPS_FORMAT_DOUBLE:
      opaque_value = 1.0;
      break;
    default:
      return false;
  }
  if (vips_extract_band(image, &alpha, vips_image_get_bands(image) - 1, NULL) != 0 || vips_min(alpha, &minimum, NULL) != 0 ||
      vips_max(alpha, &maximum, NULL) != 0) {
    if (alpha != NULL) {
      g_object_unref(alpha);
    }
    vips_error_clear();
    return false;
  }
  g_object_unref(alpha);
  return minimum == opaque_value && maximum == opaque_value;
}

static int laghu_image_prepare(VipsImage *input, const laghu_image_request *request, unsigned int frames, VipsImage **prepared,
                               laghu_image_filter_mask *applied) {
  VipsImage *oriented = NULL;
  VipsImage *resized = NULL;
  unsigned int width = (unsigned int)vips_image_get_width(input);
  unsigned int height = laghu_image_page_height(input);
  unsigned int target_width = request->target_width;
  unsigned int target_height = request->target_height;
  double scale;

  if (vips_autorot(input, &oriented, NULL) != 0) {
    return -1;
  }
  if (frames == 1U && (target_width > 0U || target_height > 0U) &&
      (request->resize_filter & (LAGHU_IMAGE_RESIZE_ATTRIBUTE | LAGHU_IMAGE_RESIZE_RENDERED | LAGHU_IMAGE_RESIZE_MOBILE)) != 0U &&
      (request->filters & request->resize_filter) != 0U) {
    if (target_width == 0U) {
      target_width = (unsigned int)((uint64_t)width * target_height / height);
    }
    if (target_height == 0U) {
      target_height = (unsigned int)((uint64_t)height * target_width / width);
    }
    if (target_width < width || target_height < height) {
      double width_scale = (double)target_width / (double)width;
      double height_scale = (double)target_height / (double)height;
      scale = width_scale < height_scale ? width_scale : height_scale;
      if (vips_resize(oriented, &resized, scale, "kernel", VIPS_KERNEL_LANCZOS3, NULL) != 0) {
        g_object_unref(oriented);
        return -1;
      }
      g_object_unref(oriented);
      oriented = resized;
      *applied |= request->resize_filter;
      *applied |= request->filters & (LAGHU_IMAGE_RESPONSIVE | LAGHU_IMAGE_RESPONSIVE_ZOOM);
    }
  }
  *prepared = oriented;
  return 0;
}

static int laghu_image_save(VipsImage *image, laghu_image_format format, const laghu_image_request *request, bool lossless, bool progressive,
                            bool sampling, void **output, size_t *output_length) {
  int keep = VIPS_FOREIGN_KEEP_ALL;

  if ((request->filters & LAGHU_IMAGE_STRIP_METADATA) != 0U) {
    keep = VIPS_FOREIGN_KEEP_NONE;
  } else if ((request->filters & LAGHU_IMAGE_STRIP_COLOR_PROFILE) != 0U) {
    keep &= ~VIPS_FOREIGN_KEEP_ICC;
  }

  switch (format) {
    case LAGHU_IMAGE_FORMAT_JPEG:
      return vips_jpegsave_buffer(image, output, output_length, "Q", (int)request->quality, "optimize_coding", TRUE, "interlace", progressive,
                                  "subsample_mode", sampling ? VIPS_FOREIGN_SUBSAMPLE_ON : VIPS_FOREIGN_SUBSAMPLE_AUTO, "keep", keep, NULL);
    case LAGHU_IMAGE_FORMAT_PNG:
      return vips_pngsave_buffer(image, output, output_length, "compression", 9, "filter", VIPS_FOREIGN_PNG_FILTER_ALL, "keep", keep, NULL);
    case LAGHU_IMAGE_FORMAT_WEBP:
      return vips_webpsave_buffer(image, output, output_length, "Q", (int)request->quality, "lossless", lossless, "effort", 4, "keep", keep, NULL);
    case LAGHU_IMAGE_FORMAT_AVIF:
      return vips_image_write_to_buffer(image, ".avif", output, output_length, "Q", (int)request->quality, "keep", keep, NULL);
    case LAGHU_IMAGE_FORMAT_JXL:
      return vips_image_write_to_buffer(image, ".jxl", output, output_length, "Q", (int)request->quality, "keep", keep, NULL);
    case LAGHU_IMAGE_FORMAT_GIF:
    case LAGHU_IMAGE_FORMAT_UNKNOWN:
    default:
      return -1;
  }
}

static double laghu_image_alpha_max(VipsBandFormat format) {
  switch (format) {
    case VIPS_FORMAT_UCHAR:
      return 255.0;
    case VIPS_FORMAT_USHORT:
      return 65535.0;
    case VIPS_FORMAT_FLOAT:
    case VIPS_FORMAT_DOUBLE:
      return 1.0;
    default:
      return 0.0;
  }
}

static bool laghu_image_alpha_equal(VipsImage *expected, VipsImage *decoded) {
  VipsImage *expected_alpha = NULL;
  VipsImage *decoded_alpha = NULL;
  VipsImage *expected_normalized = NULL;
  VipsImage *decoded_normalized = NULL;
  void *expected_pixels = NULL;
  void *decoded_pixels = NULL;
  size_t expected_size = 0U;
  size_t decoded_size = 0U;
  double expected_max = laghu_image_alpha_max(vips_image_get_format(expected));
  double decoded_max = laghu_image_alpha_max(vips_image_get_format(decoded));
  bool equal = false;

  if (expected_max == 0.0 || decoded_max == 0.0 || vips_extract_band(expected, &expected_alpha, vips_image_get_bands(expected) - 1, NULL) != 0 ||
      vips_extract_band(decoded, &decoded_alpha, vips_image_get_bands(decoded) - 1, NULL) != 0 ||
      vips_linear1(expected_alpha, &expected_normalized, 1.0 / expected_max, 0.0, NULL) != 0 ||
      vips_linear1(decoded_alpha, &decoded_normalized, 1.0 / decoded_max, 0.0, NULL) != 0) {
    goto cleanup;
  }
  expected_pixels = vips_image_write_to_memory(expected_normalized, &expected_size);
  decoded_pixels = vips_image_write_to_memory(decoded_normalized, &decoded_size);
  equal = expected_pixels != NULL && decoded_pixels != NULL && expected_size == decoded_size &&
          memcmp(expected_pixels, decoded_pixels, expected_size) == 0;

cleanup:
  g_free(decoded_pixels);
  g_free(expected_pixels);
  if (decoded_normalized != NULL) {
    g_object_unref(decoded_normalized);
  }
  if (expected_normalized != NULL) {
    g_object_unref(expected_normalized);
  }
  if (decoded_alpha != NULL) {
    g_object_unref(decoded_alpha);
  }
  if (expected_alpha != NULL) {
    g_object_unref(expected_alpha);
  }
  if (!equal) {
    vips_error_clear();
  }
  return equal;
}

static bool laghu_image_candidate_valid(laghu_buffer candidate, laghu_image_format format, VipsImage *expected, bool lossless,
                                        unsigned int expected_width, unsigned int expected_height, unsigned int expected_frames) {
  VipsImage *decoded = NULL;
  unsigned int width;
  unsigned int height;
  unsigned int frames;
  bool valid = false;

  if (laghu_image_load(format, candidate, expected_frames, &decoded) != 0) {
    vips_error_clear();
    return false;
  }
  width = (unsigned int)vips_image_get_width(decoded);
  height = laghu_image_page_height(decoded);
  frames = laghu_image_frames(decoded);
  valid = width == expected_width && height == expected_height && frames == expected_frames &&
          (vips_image_hasalpha(decoded) == vips_image_hasalpha(expected) ||
           (vips_image_hasalpha(expected) && !vips_image_hasalpha(decoded) && laghu_image_is_opaque(expected, LAGHU_IMAGE_FORMAT_PNG)));
  if (valid && vips_image_hasalpha(expected) && vips_image_hasalpha(decoded)) {
    valid = laghu_image_alpha_equal(expected, decoded);
  }
  if (valid && frames > 1U) {
    int *expected_delays = NULL;
    int *decoded_delays = NULL;
    int expected_delay_count = 0;
    int decoded_delay_count = 0;
    int expected_loop = 0;
    int decoded_loop = 0;
    bool expected_has_delay = vips_image_get_typeof(expected, "delay") != 0U;
    bool decoded_has_delay = vips_image_get_typeof(decoded, "delay") != 0U;
    bool expected_has_loop = vips_image_get_typeof(expected, "loop") != 0U;
    bool decoded_has_loop = vips_image_get_typeof(decoded, "loop") != 0U;

    valid = expected_has_delay == decoded_has_delay && expected_has_loop == decoded_has_loop;
    if (valid && expected_has_delay) {
      valid = vips_image_get_array_int(expected, "delay", &expected_delays, &expected_delay_count) == 0 &&
              vips_image_get_array_int(decoded, "delay", &decoded_delays, &decoded_delay_count) == 0 && expected_delay_count == decoded_delay_count &&
              (expected_delay_count == 0 || memcmp(expected_delays, decoded_delays, (size_t)expected_delay_count * sizeof(*expected_delays)) == 0);
    }
    if (valid && expected_has_loop) {
      valid = vips_image_get_int(expected, "loop", &expected_loop) == 0 && vips_image_get_int(decoded, "loop", &decoded_loop) == 0 &&
              expected_loop == decoded_loop;
    }
  }
  if (valid && lossless) {
    size_t expected_size = 0U;
    size_t decoded_size = 0U;
    void *expected_pixels = vips_image_write_to_memory(expected, &expected_size);
    void *decoded_pixels = vips_image_write_to_memory(decoded, &decoded_size);
    valid = expected_pixels != NULL && decoded_pixels != NULL && expected_size == decoded_size &&
            memcmp(expected_pixels, decoded_pixels, expected_size) == 0;
    g_free(expected_pixels);
    g_free(decoded_pixels);
  }
  g_object_unref(decoded);
  return valid;
}

static void laghu_image_consider(laghu_image_result *result, void *candidate, size_t candidate_length, laghu_image_format format,
                                 laghu_image_filter_mask filters, VipsImage *expected, bool lossless, unsigned int width, unsigned int height,
                                 unsigned int frames, bool preferred) {
  laghu_candidate_result finalized;
  laghu_buffer current = preferred ? result->original : (result->used_candidate ? result->selected : result->original);
  laghu_buffer proposed = {(const unsigned char *)candidate, candidate_length};

  finalized = laghu_finalize_candidate(current, proposed, true);
  if ((preferred || finalized.decision == LAGHU_CANDIDATE_ACCEPTED) &&
      laghu_image_candidate_valid(proposed, format, expected, lossless, width, height, frames)) {
    if (result->owned_candidate != NULL) {
      g_free(result->owned_candidate);
    }
    result->owned_candidate = candidate;
    result->selected = proposed;
    result->output_format = format;
    result->applied_filters = filters;
    result->used_candidate = true;
  } else {
    g_free(candidate);
  }
}

bool laghu_image_optimize(const laghu_image_backend *backend, const laghu_image_request *request, laghu_image_result *result) {
  laghu_image_filter_mask effective;
  laghu_image_filter_mask prepared_filters = 0U;
  laghu_image_format input_format;
  VipsImage *input = NULL;
  VipsImage *prepared = NULL;
  VipsImage *opaque_rgb = NULL;
  VipsImage *candidate_source;
  unsigned int width;
  unsigned int height;
  unsigned int frames;
  bool opaque;

  if (request == NULL || result == NULL || (request->original.data == NULL && request->original.length != 0U)) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  result->original = request->original;
  result->selected = request->original;
  result->input_format = laghu_image_detect_format(request->original);
  result->output_format = result->input_format;

  if (backend == NULL || !backend->available) {
    result->backend_unavailable = true;
    return true;
  }
  if (request->quality > 100U || request->quality == 0U || request->max_input_bytes == 0U || request->max_input_bytes > LAGHU_IMAGE_MAX_INPUT_BYTES ||
      request->max_dimension == 0U || request->max_dimension > LAGHU_IMAGE_MAX_DIMENSION || request->max_pixels == 0U ||
      request->max_pixels > LAGHU_IMAGE_MAX_PIXELS || request->max_frames == 0U || request->max_frames > LAGHU_IMAGE_MAX_FRAMES ||
      (request->resize_filter & ~(LAGHU_IMAGE_RESIZE_ATTRIBUTE | LAGHU_IMAGE_RESIZE_RENDERED | LAGHU_IMAGE_RESIZE_MOBILE)) != 0U ||
      (request->resize_filter != 0U && (request->resize_filter & (request->resize_filter - 1U)) != 0U) ||
      request->original.length > request->max_input_bytes) {
    result->input_rejected = true;
    return true;
  }
  input_format = result->input_format;
  if (input_format == LAGHU_IMAGE_FORMAT_UNKNOWN || laghu_image_load(input_format, request->original, request->max_frames, &input) != 0) {
    vips_error_clear();
    result->input_rejected = true;
    return true;
  }
  if (!laghu_image_dimensions_valid(input, request, &width, &height, &frames)) {
    result->input_rejected = true;
    g_object_unref(input);
    return true;
  }
  result->natural_width = width;
  result->natural_height = height;
  result->width = width;
  result->height = height;
  result->frames = frames;
  opaque = laghu_image_is_opaque(input, input_format);
  effective = laghu_image_effective_filters(request->filters, backend->capabilities);
  if (laghu_image_prepare(input, request, frames, &prepared, &prepared_filters) != 0) {
    vips_error_clear();
    result->input_rejected = true;
    g_object_unref(input);
    return true;
  }
  width = (unsigned int)vips_image_get_width(prepared);
  height = laghu_image_page_height(prepared);
  candidate_source = prepared;

#define LAGHU_TRY_SAVE(target_format, lossless_value, filter_bits)                                                                       \
  do {                                                                                                                                   \
    void *candidate_bytes = NULL;                                                                                                        \
    size_t candidate_length = 0U;                                                                                                        \
    if (laghu_image_save(candidate_source, target_format, request, lossless_value, (effective & LAGHU_IMAGE_JPEG_PROGRESSIVE) != 0U,     \
                         (effective & LAGHU_IMAGE_JPEG_SAMPLING) != 0U, &candidate_bytes, &candidate_length) == 0) {                     \
      laghu_image_consider(result, candidate_bytes, candidate_length, target_format, prepared_filters | (filter_bits), candidate_source, \
                           lossless_value, width, height, frames,                                                                         \
                           (target_format == LAGHU_IMAGE_FORMAT_JXL && request->accept_jxl) ||                                           \
                               (target_format == LAGHU_IMAGE_FORMAT_AVIF && request->accept_avif &&                                      \
                                (!request->accept_jxl || result->output_format != LAGHU_IMAGE_FORMAT_JXL)) ||                            \
                               (target_format == LAGHU_IMAGE_FORMAT_WEBP && request->accept_webp));                                      \
    } else {                                                                                                                             \
      vips_error_clear();                                                                                                                \
    }                                                                                                                                    \
  } while (0)

  switch (input_format) {
    case LAGHU_IMAGE_FORMAT_JPEG:
      if (request->allow_lossy && (effective & LAGHU_IMAGE_RECOMPRESS_JPEG) != 0U) {
        LAGHU_TRY_SAVE(LAGHU_IMAGE_FORMAT_JPEG, false,
                       effective & (LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_RECOMPRESS_JPEG | LAGHU_IMAGE_JPEG_PROGRESSIVE |
                                    LAGHU_IMAGE_JPEG_SAMPLING | LAGHU_IMAGE_STRIP_METADATA | LAGHU_IMAGE_STRIP_COLOR_PROFILE));
      }
      if (request->accept_webp && request->allow_lossy && (effective & LAGHU_IMAGE_JPEG_TO_WEBP) != 0U) {
        LAGHU_TRY_SAVE(LAGHU_IMAGE_FORMAT_WEBP, false,
                       effective & (LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_JPEG_TO_WEBP | LAGHU_IMAGE_STRIP_METADATA |
                                    LAGHU_IMAGE_STRIP_COLOR_PROFILE | LAGHU_IMAGE_IN_PLACE_BROWSER));
      }
      if (request->accept_jxl && request->allow_lossy && (backend->capabilities & LAGHU_IMAGE_CAP_JXL_SAVE) != 0U) {
        LAGHU_TRY_SAVE(LAGHU_IMAGE_FORMAT_JXL, false, LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_IN_PLACE_BROWSER);
      }
      if (request->accept_avif && request->allow_lossy && result->output_format != LAGHU_IMAGE_FORMAT_JXL) {
        LAGHU_TRY_SAVE(LAGHU_IMAGE_FORMAT_AVIF, false, LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_IN_PLACE_BROWSER);
      }
      break;
    case LAGHU_IMAGE_FORMAT_PNG:
      if ((effective & (LAGHU_IMAGE_RECOMPRESS_IMAGES | LAGHU_IMAGE_RECOMPRESS_PNG)) != 0U) {
        LAGHU_TRY_SAVE(LAGHU_IMAGE_FORMAT_PNG, true,
                       effective & (LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_RECOMPRESS_IMAGES | LAGHU_IMAGE_RECOMPRESS_PNG |
                                    LAGHU_IMAGE_STRIP_METADATA | LAGHU_IMAGE_STRIP_COLOR_PROFILE));
      }
      if (opaque && request->allow_lossy && (effective & LAGHU_IMAGE_PNG_TO_JPEG) != 0U) {
        bool jpeg_ready =
            !vips_image_hasalpha(prepared) || vips_extract_band(prepared, &opaque_rgb, 0, "n", vips_image_get_bands(prepared) - 1, NULL) == 0;
        if (!jpeg_ready) {
          vips_error_clear();
        } else {
          candidate_source = opaque_rgb != NULL ? opaque_rgb : prepared;
          LAGHU_TRY_SAVE(LAGHU_IMAGE_FORMAT_JPEG, false,
                         effective & (LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_PNG_TO_JPEG | LAGHU_IMAGE_STRIP_METADATA |
                                      LAGHU_IMAGE_STRIP_COLOR_PROFILE | LAGHU_IMAGE_IN_PLACE_BROWSER));
          candidate_source = prepared;
        }
      }
      if (request->accept_webp && (effective & LAGHU_IMAGE_TO_WEBP_LOSSLESS) != 0U) {
        if (opaque && vips_image_hasalpha(prepared) && opaque_rgb == NULL &&
            vips_extract_band(prepared, &opaque_rgb, 0, "n", vips_image_get_bands(prepared) - 1, NULL) != 0) {
          vips_error_clear();
        }
        if (opaque_rgb != NULL) {
          candidate_source = opaque_rgb;
        }
        LAGHU_TRY_SAVE(LAGHU_IMAGE_FORMAT_WEBP, true,
                       effective & (LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_TO_WEBP_LOSSLESS | LAGHU_IMAGE_STRIP_METADATA |
                                    LAGHU_IMAGE_STRIP_COLOR_PROFILE | LAGHU_IMAGE_IN_PLACE_BROWSER));
        candidate_source = prepared;
      }
      if (request->accept_jxl && request->allow_lossy && (backend->capabilities & LAGHU_IMAGE_CAP_JXL_SAVE) != 0U) {
        LAGHU_TRY_SAVE(LAGHU_IMAGE_FORMAT_JXL, false, LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_IN_PLACE_BROWSER);
      }
      if (request->accept_avif && request->allow_lossy && result->output_format != LAGHU_IMAGE_FORMAT_JXL) {
        LAGHU_TRY_SAVE(LAGHU_IMAGE_FORMAT_AVIF, false, LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_IN_PLACE_BROWSER);
      }
      break;
    case LAGHU_IMAGE_FORMAT_GIF:
      if (frames == 1U && (effective & LAGHU_IMAGE_GIF_TO_PNG) != 0U) {
        LAGHU_TRY_SAVE(
            LAGHU_IMAGE_FORMAT_PNG, true,
            effective & (LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_GIF_TO_PNG | LAGHU_IMAGE_STRIP_METADATA | LAGHU_IMAGE_STRIP_COLOR_PROFILE));
      }
      if (request->accept_webp && frames == 1U && (effective & LAGHU_IMAGE_TO_WEBP_LOSSLESS) != 0U) {
        LAGHU_TRY_SAVE(LAGHU_IMAGE_FORMAT_WEBP, true,
                       effective & (LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_TO_WEBP_LOSSLESS | LAGHU_IMAGE_STRIP_METADATA |
                                    LAGHU_IMAGE_STRIP_COLOR_PROFILE | LAGHU_IMAGE_IN_PLACE_BROWSER));
      }
      if (request->accept_webp && frames > 1U && (effective & LAGHU_IMAGE_TO_WEBP_ANIMATED) != 0U) {
        LAGHU_TRY_SAVE(LAGHU_IMAGE_FORMAT_WEBP, !request->allow_lossy,
                       effective & (LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_TO_WEBP_ANIMATED | LAGHU_IMAGE_STRIP_METADATA |
                                    LAGHU_IMAGE_STRIP_COLOR_PROFILE | LAGHU_IMAGE_IN_PLACE_BROWSER));
      }
      break;
    case LAGHU_IMAGE_FORMAT_WEBP:
      if ((effective & (LAGHU_IMAGE_RECOMPRESS_IMAGES | LAGHU_IMAGE_RECOMPRESS_WEBP)) != 0U) {
        bool lossless = (effective & LAGHU_IMAGE_RECOMPRESS_IMAGES) != 0U || !request->allow_lossy;
        LAGHU_TRY_SAVE(LAGHU_IMAGE_FORMAT_WEBP, lossless,
                       effective & (LAGHU_IMAGE_REWRITE_IMAGES | LAGHU_IMAGE_RECOMPRESS_IMAGES | LAGHU_IMAGE_RECOMPRESS_WEBP |
                                    LAGHU_IMAGE_STRIP_METADATA | LAGHU_IMAGE_STRIP_COLOR_PROFILE | LAGHU_IMAGE_IN_PLACE_BROWSER));
      }
      break;
    case LAGHU_IMAGE_FORMAT_AVIF:
    case LAGHU_IMAGE_FORMAT_JXL:
      break;
    case LAGHU_IMAGE_FORMAT_UNKNOWN:
    default:
      break;
  }

#undef LAGHU_TRY_SAVE

  if (result->used_candidate) {
    result->width = width;
    result->height = height;
    result->frames = frames;
  }
  if (opaque_rgb != NULL) {
    g_object_unref(opaque_rgb);
  }
  g_object_unref(prepared);
  g_object_unref(input);
  return true;
}

bool laghu_image_build_sprite(const laghu_image_backend *backend, laghu_image_sprite_item *items, size_t item_count, laghu_image_format output_format,
                              laghu_image_sprite_result *result) {
  VipsImage **images;
  VipsImage *joined = NULL;
  laghu_image_request request;
  void *encoded = NULL;
  size_t encoded_length = 0U;
  size_t original_length = 0U;
  size_t index;
  unsigned int offset = 0U;
  unsigned int maximum_height = 0U;
  bool success = false;

  if (backend == NULL || !backend->available || items == NULL || item_count < 2U || item_count > (size_t)INT_MAX || result == NULL ||
      (output_format != LAGHU_IMAGE_FORMAT_PNG && output_format != LAGHU_IMAGE_FORMAT_WEBP)) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  images = calloc(item_count, sizeof(*images));
  if (images == NULL) {
    return false;
  }
  for (index = 0U; index < item_count; ++index) {
    laghu_image_format input_format = laghu_image_detect_format(items[index].original);
    unsigned int width;
    unsigned int height;
    unsigned int frames;

    laghu_image_request_init(&request);
    if (input_format == LAGHU_IMAGE_FORMAT_UNKNOWN || items[index].original.length > SIZE_MAX - original_length ||
        laghu_image_load(input_format, items[index].original, 1U, &images[index]) != 0 ||
        !laghu_image_dimensions_valid(images[index], &request, &width, &height, &frames) || frames != 1U ||
        width > LAGHU_IMAGE_MAX_DIMENSION - offset) {
      goto cleanup;
    }
    original_length += items[index].original.length;
    items[index].x = offset;
    items[index].y = 0U;
    items[index].width = width;
    items[index].height = height;
    offset += width;
    if (height > maximum_height) {
      maximum_height = height;
    }
  }
  if ((uint64_t)offset * maximum_height > LAGHU_IMAGE_MAX_PIXELS) {
    goto cleanup;
  }
  if (vips_arrayjoin(images, &joined, (int)item_count, "across", (int)item_count, NULL) != 0) {
    goto cleanup;
  }
  laghu_image_request_init(&request);
  request.quality = 100U;
  request.filters = LAGHU_IMAGE_SPRITE | LAGHU_IMAGE_STRIP_METADATA;
  if (laghu_image_save(joined, output_format, &request, true, false, false, &encoded, &encoded_length) != 0 || encoded_length >= original_length ||
      !laghu_image_candidate_valid((laghu_buffer){encoded, encoded_length}, output_format, joined, true, (unsigned int)vips_image_get_width(joined),
                                   laghu_image_page_height(joined), 1U)) {
    goto cleanup;
  }
  result->data = malloc(encoded_length);
  if (result->data == NULL) {
    goto cleanup;
  }
  memcpy(result->data, encoded, encoded_length);
  result->length = encoded_length;
  result->format = output_format;
  result->width = (unsigned int)vips_image_get_width(joined);
  result->height = laghu_image_page_height(joined);
  success = true;

cleanup:
  g_free(encoded);
  if (joined != NULL) {
    g_object_unref(joined);
  }
  for (index = 0U; index < item_count; ++index) {
    if (images[index] != NULL) {
      g_object_unref(images[index]);
    }
  }
  free(images);
  if (!success) {
    free(result->data);
    memset(result, 0, sizeof(*result));
    vips_error_clear();
  }
  return success;
}

void laghu_image_result_release(laghu_image_result *result) {
  if (result == NULL) {
    return;
  }
  if (result->owned_candidate != NULL) {
    g_free(result->owned_candidate);
  }
  memset(result, 0, sizeof(*result));
}

#else

bool laghu_image_backend_probe(laghu_image_backend *backend) {
  if (backend == NULL) {
    return false;
  }
  memset(backend, 0, sizeof(*backend));
  (void)snprintf(backend->backend_id, sizeof(backend->backend_id), "laghu-libvips-" LAGHU_VERSION "-unavailable");
  return true;
}

bool laghu_image_optimize(const laghu_image_backend *backend, const laghu_image_request *request, laghu_image_result *result) {
  (void)backend;
  if (request == NULL || result == NULL || (request->original.data == NULL && request->original.length != 0U)) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  result->original = request->original;
  result->selected = request->original;
  result->input_format = laghu_image_detect_format(request->original);
  result->output_format = result->input_format;
  result->backend_unavailable = true;
  return true;
}

bool laghu_image_build_sprite(const laghu_image_backend *backend, laghu_image_sprite_item *items, size_t item_count, laghu_image_format output_format,
                              laghu_image_sprite_result *result) {
  (void)backend;
  (void)items;
  (void)item_count;
  (void)output_format;
  if (result != NULL) {
    memset(result, 0, sizeof(*result));
  }
  return false;
}

void laghu_image_result_release(laghu_image_result *result) {
  if (result != NULL) {
    memset(result, 0, sizeof(*result));
  }
}

#endif

void laghu_image_sprite_result_release(laghu_image_sprite_result *result) {
  if (result != NULL) {
    free(result->data);
    memset(result, 0, sizeof(*result));
  }
}
