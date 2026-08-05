// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/cache.h"
#include "laghu/catalog.h"
#include "laghu/css.h"
#include "laghu/queue.h"
#include "laghu/types.h"

typedef struct {
  char optimized[sizeof("/.laghu/image/") + LAGHU_SHA256_HEX_SIZE];
  char sprite[sizeof("/.laghu/image/") + LAGHU_SHA256_HEX_SIZE];
  char source_hash[LAGHU_RUNTIME_KEY_SIZE];
  char variant_key[LAGHU_RUNTIME_KEY_SIZE];
  size_t original_length;
  size_t variant_length;
} laghu_runtime_css_storage;

static bool laghu_runtime_sprite_key(const char *policy_key,
                                     const laghu_runtime_queue *queue,
                                     laghu_runtime_css_storage *storage,
                                     const laghu_css_parse_result *discovery,
                                     const laghu_image_resource *resources,
                                     size_t *ordered, size_t count,
                                     char output[LAGHU_RUNTIME_KEY_SIZE]) {
  char material[16384U];
  size_t length;
  size_t index;
  int written =
      snprintf(material, sizeof(material),
               "laghu-sprite-v1\n%s\n%s\n%u\nhorizontal\npng-lossless",
               policy_key, queue != NULL ? queue->backend_id : "",
               queue != NULL ? queue->capabilities : 0U);
  if (written <= 0 || (size_t)written >= sizeof(material)) {
    return false;
  }
  length = (size_t)written;
  for (index = 0U; index < count; ++index) {
    size_t item = ordered[index];
    written = snprintf(
        material + length, sizeof(material) - length, "\n%s\n%s\n%u:%u",
        discovery->dependencies[item].source_url, storage[item].variant_key,
        resources[item].width, resources[item].height);
    if (written <= 0 || (size_t)written >= sizeof(material) - length) {
      return false;
    }
    length += (size_t)written;
  }
  return laghu_sha256_hex(
      (laghu_buffer){(const unsigned char *)material, length}, output);
}

static laghu_catalog_variant *laghu_runtime_css_variant(
    laghu_catalog_record *catalog) {
  unsigned int index;
  for (index = 0U; index < catalog->variant_count; ++index) {
    laghu_catalog_variant *variant = &catalog->variants[index];
    if (variant->ready && variant->variant_key[0] != '\0' &&
        catalog->original_content_type[0] != '\0' &&
        strcmp(variant->content_type, catalog->original_content_type) == 0) {
      return variant;
    }
  }
  return NULL;
}

static bool laghu_runtime_css_key(
    laghu_buffer css, const char *stylesheet_path, const char *policy_key,
    uint32_t capability_mask, const laghu_image_resource *resources,
    size_t resource_count, bool minify, bool allow_sprites,
    const char *import_dependency_key, char output[LAGHU_RUNTIME_KEY_SIZE]) {
  char source_hash[LAGHU_RUNTIME_KEY_SIZE];
  size_t capacity =
      512U + strlen(stylesheet_path != NULL ? stylesheet_path : "") +
      resource_count * (LAGHU_IMAGE_URL_SIZE + LAGHU_RUNTIME_KEY_SIZE * 2U);
  char *material;
  size_t length;
  size_t index;
  int written;
  if (!laghu_sha256_hex(css, source_hash) || stylesheet_path == NULL ||
      policy_key == NULL) {
    return false;
  }
  material = malloc(capacity);
  if (material == NULL) {
    return false;
  }
  written = snprintf(
      material, capacity, "laghu-css-v%u\n%s\n%s\n%s\n%u\n%d:%d\n%s",
      LAGHU_CSS_DERIVATION_VERSION, source_hash, stylesheet_path, policy_key,
      capability_mask, minify ? 1 : 0, allow_sprites ? 1 : 0,
      import_dependency_key != NULL ? import_dependency_key : "");
  if (written <= 0 || (size_t)written >= capacity) {
    free(material);
    return false;
  }
  length = (size_t)written;
  for (index = 0U; index < resource_count; ++index) {
    const laghu_image_resource *resource = &resources[index];
    written = snprintf(
        material + length, capacity - length, "\n%s\n%s\n%s",
        resource->source_url != NULL ? resource->source_url : "",
        resource->source_hash != NULL ? resource->source_hash : "",
        resource->optimized_url != NULL ? resource->optimized_url : "");
    if (written <= 0 || (size_t)written >= capacity - length) {
      free(material);
      return false;
    }
    length += (size_t)written;
  }
  if (!laghu_sha256_hex((laghu_buffer){(const unsigned char *)material, length},
                        output)) {
    free(material);
    return false;
  }
  free(material);
  return true;
}

bool laghu_runtime_rewrite_css(laghu_runtime_queue *queue,
                               const char *cache_path, laghu_buffer css,
                               const char *stylesheet_path,
                               const char *page_origin, const char *policy_key,
                               uint32_t capability_mask, uint64_t now,
                               unsigned int ttl_seconds, bool minify,
                               bool allow_sprites, unsigned int inline_limit,
                               unsigned int outline_threshold,
                               laghu_runtime_css_result *result) {
  laghu_css_parse_result *discovery = NULL;
  laghu_image_resource *resources = NULL;
  laghu_runtime_css_storage *storage = NULL;
  laghu_image_markup_options options = {0};
  laghu_image_markup_result rewritten;
  laghu_runtime_cache_entry entry;
  laghu_runtime_cache_entry source_entry;
  laghu_stylesheet_record stylesheet = {0};
  laghu_runtime_css_import_result imports = {0};
  laghu_buffer transform_css = css;
  char derivation_key[LAGHU_RUNTIME_KEY_SIZE];
  char source_hash[LAGHU_RUNTIME_KEY_SIZE];
  size_t index;
  bool pending = false;
  size_t byte_savings = 0U;
  bool success = false;
  if (result == NULL || cache_path == NULL || stylesheet_path == NULL ||
      policy_key == NULL || ttl_seconds == 0U || inline_limit > 65536U ||
      outline_threshold < 1024U || outline_threshold > 1048576U ||
      css.length > LAGHU_CSS_MAX_INPUT_BYTES) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  discovery = calloc(1U, sizeof(*discovery));
  if (discovery == NULL ||
      !laghu_css_discover(css, stylesheet_path, page_origin, discovery) ||
      !discovery->bounded || !laghu_sha256_hex(css, source_hash)) {
    goto finished;
  }
  resources = calloc(
      discovery->dependency_count == 0U ? 1U : discovery->dependency_count,
      sizeof(*resources));
  storage = calloc(
      discovery->dependency_count == 0U ? 1U : discovery->dependency_count,
      sizeof(*storage));
  if (resources == NULL || storage == NULL) {
    goto finished;
  }
  if (!laghu_runtime_cache_publish(cache_path, source_hash, source_hash,
                                   source_hash, "text/css",
                                   "laghu-css-source-v1", css, &source_entry)) {
    goto finished;
  }
  stylesheet.version = LAGHU_STYLESHEET_CATALOG_VERSION;
  (void)snprintf(stylesheet.normalized_url, sizeof(stylesheet.normalized_url),
                 "%s", stylesheet_path);
  memcpy(stylesheet.source_hash, source_hash, sizeof(stylesheet.source_hash));
  memcpy(stylesheet.source_key, source_hash, sizeof(stylesheet.source_key));
  memcpy(stylesheet.policy_key, policy_key, sizeof(stylesheet.policy_key));
  stylesheet.capability_mask = capability_mask;
  stylesheet.parser_version = LAGHU_CSS_DERIVATION_VERSION;
  stylesheet.inline_limit = inline_limit;
  stylesheet.outline_threshold = outline_threshold;
  stylesheet.source_length = css.length;
  stylesheet.updated_at = now;
  for (index = 0U; index < discovery->dependency_count; ++index) {
    laghu_catalog_record catalog;
    laghu_catalog_variant *variant;
    bool terminal = false;
    unsigned int variant_index;
    resources[index].source_url = discovery->dependencies[index].source_url;
    if (!laghu_catalog_lookup_url(
            cache_path, discovery->dependencies[index].source_url, policy_key,
            capability_mask, now, ttl_seconds, &catalog)) {
      pending = true;
      continue;
    }
    memcpy(storage[index].source_hash, catalog.source_hash,
           sizeof(storage[index].source_hash));
    resources[index].source_hash = storage[index].source_hash;
    variant = laghu_runtime_css_variant(&catalog);
    for (variant_index = 0U; variant_index < catalog.variant_count;
         ++variant_index) {
      terminal |= catalog.variants[variant_index].terminally_excluded;
    }
    if (variant == NULL) {
      if (terminal) {
        resources[index].terminally_excluded = true;
      } else {
        pending = true;
      }
      continue;
    }
    if (!laghu_image_variant_url(variant->variant_key,
                                 storage[index].optimized)) {
      goto finished;
    }
    resources[index].optimized_url = storage[index].optimized;
    resources[index].width = variant->width;
    resources[index].height = variant->height;
    memcpy(storage[index].variant_key, variant->variant_key,
           sizeof(storage[index].variant_key));
    storage[index].original_length = variant->original_length;
    storage[index].variant_length = variant->variant_length;
    if (variant->original_length > variant->variant_length) {
      byte_savings += variant->original_length - variant->variant_length;
    }
  }
  if (pending) {
    result->dependencies_pending = true;
    (void)laghu_stylesheet_publish(cache_path, &stylesheet);
    success = true;
    goto finished;
  }
  if (allow_sprites) {
    size_t ordered[LAGHU_RUNTIME_MAX_SPRITE_INPUTS];
    size_t sprite_count = 0U;
    size_t outer;
    char sprite_key[LAGHU_RUNTIME_KEY_SIZE];
    laghu_runtime_cache_entry sprite_entry;
    for (index = 0U; index < discovery->dependency_count &&
                     sprite_count < LAGHU_RUNTIME_MAX_SPRITE_INPUTS;
         ++index) {
      if (discovery->dependencies[index].sprite_eligible &&
          storage[index].variant_key[0] != '\0') {
        ordered[sprite_count++] = index;
      }
    }
    for (outer = 0U; outer < sprite_count; ++outer) {
      size_t inner;
      for (inner = outer + 1U; inner < sprite_count; ++inner) {
        if (strcmp(discovery->dependencies[ordered[outer]].source_url,
                   discovery->dependencies[ordered[inner]].source_url) > 0) {
          size_t swap = ordered[outer];
          ordered[outer] = ordered[inner];
          ordered[inner] = swap;
        }
      }
    }
    if (sprite_count >= 2U &&
        laghu_runtime_sprite_key(policy_key, queue, storage, discovery,
                                 resources, ordered, sprite_count,
                                 sprite_key)) {
      if (laghu_runtime_cache_lookup_variant(cache_path, sprite_key,
                                             &sprite_entry)) {
        unsigned int offset = 0U;
        char *sprite_url = storage[0].sprite;
        if (!laghu_image_variant_url(sprite_key, sprite_url)) {
          goto finished;
        }
        for (outer = 0U; outer < sprite_count; ++outer) {
          size_t item = ordered[outer];
          resources[item].sprite_url = sprite_url;
          resources[item].sprite_x = offset;
          resources[item].sprite_y = 0U;
          offset += resources[item].width;
        }
        if (byte_savings > sprite_entry.length) {
          byte_savings -= sprite_entry.length;
        }
      } else if (queue != NULL) {
        laghu_runtime_job job = {0};
        job.kind = LAGHU_RUNTIME_JOB_SPRITE;
        memcpy(job.index_key, sprite_key, sizeof(job.index_key));
        memcpy(job.policy_key, policy_key, sizeof(job.policy_key));
        memcpy(job.validator, source_hash, sizeof(source_hash));
        job.sprite_count = (unsigned int)sprite_count;
        for (outer = 0U; outer < sprite_count; ++outer) {
          size_t item = ordered[outer];
          memcpy(job.sprite_variant_keys[outer], storage[item].variant_key,
                 LAGHU_RUNTIME_KEY_SIZE);
          job.sprite_width[outer] = resources[item].width;
          job.sprite_height[outer] = resources[item].height;
        }
        (void)laghu_runtime_queue_try_publish(queue, &job);
        result->dependencies_pending = true;
        (void)laghu_stylesheet_publish(cache_path, &stylesheet);
        success = true;
        goto finished;
      }
    }
  }
  if (allow_sprites && discovery->has_imports) {
    if (!laghu_runtime_flatten_css_imports(
            cache_path, css, stylesheet_path, page_origin, policy_key,
            capability_mask, now, ttl_seconds, inline_limit, outline_threshold,
            &imports)) {
      goto finished;
    }
    if (imports.dependencies_pending) {
      result->dependencies_pending = true;
      (void)laghu_stylesheet_publish(cache_path, &stylesheet);
      success = true;
      goto finished;
    }
    if (imports.invalid || !imports.flattened) {
      stylesheet.terminally_excluded = true;
      (void)laghu_stylesheet_publish(cache_path, &stylesheet);
      success = true;
      goto finished;
    }
    transform_css = (laghu_buffer){imports.data, imports.length};
    if (imports.original_external_bytes > SIZE_MAX - byte_savings) {
      goto finished;
    }
    byte_savings += imports.original_external_bytes;
  }
  if (!laghu_runtime_css_key(
          css, stylesheet_path, policy_key, capability_mask, resources,
          discovery->dependency_count, minify, allow_sprites,
          imports.flattened ? imports.dependency_key : NULL, derivation_key)) {
    goto finished;
  }
  memcpy(result->dependency_key, derivation_key,
         sizeof(result->dependency_key));
  if (laghu_runtime_cache_lookup_variant(cache_path, derivation_key, &entry)) {
    memcpy(stylesheet.derived_key, derivation_key,
           sizeof(stylesheet.derived_key));
    memcpy(stylesheet.dependency_key, derivation_key,
           sizeof(stylesheet.dependency_key));
    stylesheet.derived_length = entry.length;
    stylesheet.ready = true;
    (void)laghu_stylesheet_publish(cache_path, &stylesheet);
    result->data = malloc(entry.length + 1U);
    if (result->data == NULL ||
        !laghu_runtime_cache_read(&entry, result->data, entry.length)) {
      goto finished;
    }
    result->data[entry.length] = '\0';
    result->length = entry.length;
    result->rewritten = true;
    success = true;
    goto finished;
  }
  options.resources = resources;
  options.resource_count = discovery->dependency_count;
  options.sprites = allow_sprites;
  if (discovery->valid && minify) {
    success =
        laghu_css_minify_and_rewrite(transform_css, stylesheet_path,
                                     page_origin, &options, false, &rewritten);
  } else {
    success = laghu_css_fallback_rewrite_urls(css, stylesheet_path, page_origin,
                                              &options, &rewritten);
    result->used_fallback = success;
  }
  if (!success) {
    goto finished;
  }
  if (imports.flattened) {
    rewritten.applied_filters |= LAGHU_CSS_FLATTEN_IMPORTS_APPLIED;
  }
  if ((rewritten.length >= css.length &&
       rewritten.length - css.length >= byte_savings) ||
      rewritten.applied_filters == 0U) {
    stylesheet.terminally_excluded = true;
    (void)laghu_stylesheet_publish(cache_path, &stylesheet);
    laghu_image_markup_result_release(&rewritten);
    success = true;
    goto finished;
  }
  if (!laghu_runtime_cache_publish(
          cache_path, derivation_key, derivation_key, source_hash, "text/css",
          "laghu-css-v1", (laghu_buffer){rewritten.data, rewritten.length},
          &entry)) {
    laghu_image_markup_result_release(&rewritten);
    success = false;
    goto finished;
  }
  laghu_image_markup_result_release(&rewritten);
  memcpy(stylesheet.derived_key, derivation_key,
         sizeof(stylesheet.derived_key));
  memcpy(stylesheet.dependency_key, derivation_key,
         sizeof(stylesheet.dependency_key));
  stylesheet.derived_length = entry.length;
  stylesheet.ready = true;
  (void)laghu_stylesheet_publish(cache_path, &stylesheet);
  result->published = true;
  success = true;

finished:
  laghu_runtime_css_import_result_release(&imports);
  free(storage);
  free(resources);
  free(discovery);
  if (!success) {
    laghu_runtime_css_result_release(result);
  }
  return success;
}

void laghu_runtime_css_result_release(laghu_runtime_css_result *result) {
  if (result != NULL) {
    free(result->data);
    memset(result, 0, sizeof(*result));
  }
}
