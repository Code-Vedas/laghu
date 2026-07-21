// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdlib.h>
#include <string.h>

#include "laghu/runtime.h"

typedef struct {
  char optimized[sizeof("/.laghu/image/") + LAGHU_SHA256_HEX_SIZE];
  char one_x[sizeof("/.laghu/image/") + LAGHU_SHA256_HEX_SIZE];
  char two_x[sizeof("/.laghu/image/") + LAGHU_SHA256_HEX_SIZE];
  char source_hash[LAGHU_RUNTIME_KEY_SIZE];
  char preview_data_uri[4096U];
  char *inline_uri;
} laghu_runtime_resource_storage;

static bool laghu_runtime_variant_seen(
    const laghu_runtime_resource_storage *storage, size_t count,
    const char *variant_url, bool two_x) {
  size_t index;
  for (index = 0U; index < count; ++index) {
    const char *existing = two_x ? storage[index].two_x : storage[index].one_x;
    if (existing[0] != '\0' && strcmp(existing, variant_url) == 0) {
      return true;
    }
  }
  return false;
}

static laghu_catalog_variant *laghu_runtime_variant(
    laghu_catalog_record *record, unsigned int width) {
  unsigned int index;
  for (index = 0U; index < record->variant_count; ++index) {
    if (record->variants[index].width == width) {
      return &record->variants[index];
    }
  }
  return NULL;
}

static bool laghu_runtime_inline(const char *cache_path,
                                 const laghu_catalog_variant *variant,
                                 size_t limit, char **output) {
  laghu_runtime_cache_entry entry;
  laghu_image_markup_result encoded;
  unsigned char *body;
  laghu_image_format format;
  if (!variant->ready || variant->variant_length > limit ||
      !laghu_runtime_cache_lookup_variant(cache_path, variant->variant_key,
                                          &entry)) {
    return true;
  }
  body = malloc(entry.length);
  if (body == NULL || !laghu_runtime_cache_read(&entry, body, entry.length)) {
    free(body);
    return false;
  }
  format = laghu_image_detect_format((laghu_buffer){body, entry.length});
  if (laghu_image_data_uri(format, (laghu_buffer){body, entry.length}, limit,
                           &encoded)) {
    *output = (char *)encoded.data;
  }
  free(body);
  return true;
}

bool laghu_runtime_rewrite_html(
    const char *cache_path, laghu_buffer html, const char *page_path,
    const char *page_origin, const char *policy_key, uint32_t capability_mask,
    uint64_t now, unsigned int ttl_seconds, laghu_image_filter_mask filters,
    bool allow_inline, bool csp_allows_data, bool beacon_enabled,
    size_t inline_limit, unsigned int viewport_width,
    unsigned int dpr_hundredths, laghu_runtime_html_result *result) {
  laghu_image_discovery_result *discovery = NULL;
  laghu_image_resource *resources = NULL;
  laghu_runtime_resource_storage *storage = NULL;
  laghu_image_markup_options options = {0};
  laghu_image_markup_result rewritten;
  size_t index;
  bool pending = false;
  bool success = false;
  if (result == NULL || cache_path == NULL || policy_key == NULL ||
      ttl_seconds == 0U) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  discovery = calloc(1U, sizeof(*discovery));
  if (discovery == NULL ||
      !laghu_image_discover_html(html, page_path, page_origin, discovery)) {
    free(discovery);
    return false;
  }
  {
    laghu_css_parse_result *styles = calloc(1U, sizeof(*styles));
    if (styles == NULL ||
        !laghu_css_discover_style_attributes(html, page_path, page_origin,
                                             styles) ||
        !styles->bounded) {
      free(styles);
      free(discovery);
      return false;
    }
    for (index = 0U; index < styles->dependency_count; ++index) {
      size_t existing;
      bool duplicate = false;
      for (existing = 0U; existing < discovery->resource_count; ++existing) {
        if (strcmp(discovery->resources[existing].source_url,
                   styles->dependencies[index].source_url) == 0) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        if (discovery->resource_count == LAGHU_IMAGE_MAX_PAGE_RESOURCES) {
          free(styles);
          free(discovery);
          return false;
        }
        memcpy(discovery->resources[discovery->resource_count++].source_url,
               styles->dependencies[index].source_url, LAGHU_IMAGE_URL_SIZE);
      }
    }
    free(styles);
  }
  resources =
      calloc(discovery->resource_count == 0U ? 1U : discovery->resource_count,
             sizeof(*resources));
  storage =
      calloc(discovery->resource_count == 0U ? 1U : discovery->resource_count,
             sizeof(*storage));
  if (resources == NULL || storage == NULL) {
    goto finished;
  }
  for (index = 0U; index < discovery->resource_count; ++index) {
    laghu_catalog_record catalog;
    laghu_image_geometry_input geometry = {0};
    laghu_image_geometry_plan plan;
    laghu_catalog_variant *one;
    laghu_catalog_variant *two = NULL;
    bool changed = false;
    resources[index].source_url = discovery->resources[index].source_url;
    resources[index].declared_width =
        discovery->resources[index].declared_width;
    resources[index].declared_height =
        discovery->resources[index].declared_height;
    if (!laghu_catalog_lookup_url(
            cache_path, discovery->resources[index].source_url, policy_key,
            capability_mask, now, ttl_seconds, &catalog)) {
      pending = true;
      continue;
    }
    memcpy(storage[index].source_hash, catalog.source_hash,
           sizeof(storage[index].source_hash));
    resources[index].source_hash = storage[index].source_hash;
    resources[index].width = catalog.natural_width;
    resources[index].height = catalog.natural_height;
    if (catalog.natural_width == 0U || catalog.natural_height == 0U) {
      pending = true;
      continue;
    }
    geometry.natural_width = catalog.natural_width;
    geometry.natural_height = catalog.natural_height;
    geometry.declared_width = discovery->resources[index].declared_width;
    geometry.declared_height = discovery->resources[index].declared_height;
    if ((filters & LAGHU_IMAGE_RESIZE_MOBILE) != 0U &&
        catalog.learned_mobile_width > 0U) {
      geometry.learned_width = catalog.learned_mobile_width;
      geometry.learned_height = catalog.learned_mobile_height;
      geometry.viewport_width = catalog.learned_viewport_width;
      geometry.dpr_hundredths = catalog.learned_dpr_hundredths;
      geometry.use_mobile_dimensions = true;
    } else if ((filters & LAGHU_IMAGE_RESIZE_MOBILE) != 0U &&
               viewport_width > 0U) {
      geometry.viewport_width = viewport_width;
      geometry.dpr_hundredths = dpr_hundredths;
      geometry.use_mobile_dimensions = true;
    } else if ((filters & LAGHU_IMAGE_RESIZE_RENDERED) != 0U &&
               catalog.learned_width > 0U) {
      geometry.learned_width = catalog.learned_width;
      geometry.learned_height = catalog.learned_height;
      geometry.dpr_hundredths = catalog.learned_dpr_hundredths;
      geometry.use_rendered_dimensions = true;
    }
    resources[index].above_fold = catalog.learned_above_fold;
    if (!laghu_image_plan_geometry(&geometry, &plan)) {
      pending = true;
      continue;
    }
    one = laghu_runtime_variant(&catalog, plan.width[0]);
    if (one == NULL && catalog.variant_count < LAGHU_CATALOG_MAX_WIDTHS) {
      one = &catalog.variants[catalog.variant_count++];
      one->width = plan.width[0];
      one->height = plan.height[0];
      changed = true;
    }
    if (plan.count == 2U) {
      two = laghu_runtime_variant(&catalog, plan.width[1]);
      if (two == NULL && catalog.variant_count < LAGHU_CATALOG_MAX_WIDTHS) {
        two = &catalog.variants[catalog.variant_count++];
        two->width = plan.width[1];
        two->height = plan.height[1];
        changed = true;
      }
    }
    if (changed) {
      catalog.updated_at = now;
      catalog.last_accessed_at = now;
      if (!laghu_catalog_publish_url(cache_path, &catalog)) {
        goto finished;
      }
    }
    if (one == NULL || (!one->ready && !one->terminally_excluded) ||
        (two != NULL && !two->ready && !two->terminally_excluded)) {
      pending = true;
      continue;
    }
    if (one->terminally_excluded || (two != NULL && two->terminally_excluded)) {
      resources[index].terminally_excluded = true;
      continue;
    }
    if (!laghu_image_variant_url(one->variant_key, storage[index].one_x)) {
      goto finished;
    }
    memcpy(storage[index].optimized, storage[index].one_x,
           sizeof(storage[index].optimized));
    resources[index].optimized_url = storage[index].optimized;
    resources[index].responsive_1x_url = storage[index].one_x;
    resources[index].responsive_1x_width = one->width;
    if (!laghu_runtime_variant_seen(storage, index, storage[index].one_x,
                                    false)) {
      options.unique_variant_savings_1x +=
          one->original_length > one->variant_length
              ? one->original_length - one->variant_length
              : 0U;
    }
    if (two != NULL &&
        laghu_image_variant_url(two->variant_key, storage[index].two_x)) {
      resources[index].responsive_2x_url = storage[index].two_x;
      resources[index].responsive_2x_width = two->width;
      if (!laghu_runtime_variant_seen(storage, index, storage[index].two_x,
                                      true)) {
        options.unique_variant_savings_2x +=
            two->original_length > two->variant_length
                ? two->original_length - two->variant_length
                : 0U;
      }
    } else {
      memcpy(storage[index].two_x, storage[index].one_x,
             sizeof(storage[index].two_x));
      resources[index].responsive_2x_url = storage[index].one_x;
      resources[index].responsive_2x_width = one->width;
      if (!laghu_runtime_variant_seen(storage, index, storage[index].two_x,
                                      true)) {
        options.unique_variant_savings_2x +=
            one->original_length > one->variant_length
                ? one->original_length - one->variant_length
                : 0U;
      }
    }
    if (catalog.preview_data_uri[0] != '\0') {
      memcpy(storage[index].preview_data_uri, catalog.preview_data_uri,
             sizeof(storage[index].preview_data_uri));
      resources[index].preview_data_uri = storage[index].preview_data_uri;
    }
    if (allow_inline && csp_allows_data &&
        laghu_runtime_inline(cache_path, one, inline_limit,
                             &storage[index].inline_uri)) {
      resources[index].inline_data_uri = storage[index].inline_uri;
      resources[index].inline_payload_length = one->variant_length;
    }
  }
  if (pending) {
    result->dependencies_pending = true;
    success = true;
    goto finished;
  }
  options.resources = resources;
  options.resource_count = discovery->resource_count;
  options.insert_dimensions = (filters & LAGHU_IMAGE_INSERT_DIMENSIONS) != 0U;
  options.responsive = (filters & LAGHU_IMAGE_RESPONSIVE) != 0U;
  options.responsive_zoom = (filters & LAGHU_IMAGE_RESPONSIVE_ZOOM) != 0U;
  options.lazyload = (filters & LAGHU_IMAGE_LAZYLOAD) != 0U;
  options.inline_images = (filters & LAGHU_IMAGE_INLINE) != 0U && allow_inline;
  options.inline_previews = (filters & LAGHU_IMAGE_INLINE_PREVIEW) != 0U;
  options.deduplicate_inline = (filters & LAGHU_IMAGE_DEDUP_INLINE) != 0U;
  options.csp_allows_data_images = csp_allows_data;
  options.inline_limit = inline_limit;
  options.enforce_bundle_gate = true;
  if (laghu_image_rewrite_html(html, &options, &rewritten)) {
    laghu_image_markup_result styled;
    if (laghu_css_rewrite_style_attributes(
            (laghu_buffer){rewritten.data, rewritten.length}, page_path,
            page_origin, &options, &styled)) {
      if (styled.applied_filters != 0U && styled.length <= rewritten.length) {
        laghu_image_markup_result_release(&rewritten);
        rewritten = styled;
      } else {
        laghu_image_markup_result_release(&styled);
      }
    }
    static const unsigned char beacon[] =
        "<script src=\"/.laghu/beacon/images.js\" defer></script>";
    result->data = rewritten.data;
    result->length = rewritten.length;
    result->rewritten = rewritten.applied_filters != 0U;
    if (beacon_enabled && result->rewritten) {
      size_t final_length = rewritten.length + sizeof(beacon) - 1U;
      size_t savings =
          options.unique_variant_savings_1x < options.unique_variant_savings_2x
              ? options.unique_variant_savings_1x
              : options.unique_variant_savings_2x;
      if (final_length <= html.length || final_length - html.length < savings) {
        unsigned char *with_beacon = malloc(final_length + 1U);
        if (with_beacon == NULL) {
          goto finished;
        }
        memcpy(with_beacon, rewritten.data, rewritten.length);
        memcpy(with_beacon + rewritten.length, beacon, sizeof(beacon) - 1U);
        with_beacon[final_length] = '\0';
        free(rewritten.data);
        result->data = with_beacon;
        result->length = final_length;
      }
    }
    memcpy(result->dependency_key, rewritten.dependency_key,
           sizeof(result->dependency_key));
    success = true;
  }

finished:
  if (storage != NULL) {
    for (index = 0U; index < discovery->resource_count; ++index) {
      free(storage[index].inline_uri);
    }
  }
  free(storage);
  free(resources);
  free(discovery);
  if (!success) {
    laghu_runtime_html_result_release(result);
  }
  return success;
}

void laghu_runtime_html_result_release(laghu_runtime_html_result *result) {
  if (result != NULL) {
    free(result->data);
    memset(result, 0, sizeof(*result));
  }
}
