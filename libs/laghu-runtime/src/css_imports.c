// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/runtime.h"

typedef struct {
  unsigned char *data;
  size_t length;
  size_t capacity;
} laghu_import_builder;

typedef struct {
  const char *cache_path;
  const char *page_origin;
  const char *policy_key;
  uint32_t capability_mask;
  uint64_t now;
  unsigned int ttl_seconds;
  unsigned int inline_limit;
  unsigned int outline_threshold;
  char stack[LAGHU_CSS_IMPORT_MAX_DEPTH][LAGHU_RUNTIME_PATH_SIZE];
  unsigned int stack_count;
  char unique[LAGHU_CSS_MAX_IMPORTS][LAGHU_RUNTIME_PATH_SIZE];
  unsigned int unique_count;
  size_t original_external_bytes;
  laghu_import_builder dependency_material;
  bool pending;
  bool invalid;
} laghu_import_context;

static bool laghu_import_append(laghu_import_builder *builder, const void *data,
                                size_t length) {
  size_t needed;
  size_t capacity;
  unsigned char *grown;
  if (length > LAGHU_CSS_MAX_INPUT_BYTES - builder->length) {
    return false;
  }
  needed = builder->length + length + 1U;
  if (needed > builder->capacity) {
    capacity = builder->capacity == 0U ? 1024U : builder->capacity;
    while (capacity < needed) {
      capacity = capacity > LAGHU_CSS_MAX_INPUT_BYTES / 2U
                     ? LAGHU_CSS_MAX_INPUT_BYTES + 1U
                     : capacity * 2U;
    }
    grown = realloc(builder->data, capacity);
    if (grown == NULL) {
      return false;
    }
    builder->data = grown;
    builder->capacity = capacity;
  }
  if (length != 0U) {
    memcpy(builder->data + builder->length, data, length);
  }
  builder->length += length;
  builder->data[builder->length] = '\0';
  return true;
}

static bool laghu_import_stack_contains(const laghu_import_context *context,
                                        const char *url) {
  unsigned int index;
  for (index = 0U; index < context->stack_count; ++index) {
    if (strcmp(context->stack[index], url) == 0) {
      return true;
    }
  }
  return false;
}

static bool laghu_import_read(const char *cache_path, const char *key,
                              unsigned char **data, size_t *length) {
  laghu_runtime_cache_entry entry;
  if (!laghu_runtime_cache_lookup_variant(cache_path, key, &entry) ||
      entry.length == 0U || entry.length > LAGHU_CSS_MAX_INPUT_BYTES) {
    return false;
  }
  *data = malloc(entry.length + 1U);
  if (*data == NULL || !laghu_runtime_cache_read(&entry, *data, entry.length)) {
    free(*data);
    *data = NULL;
    return false;
  }
  (*data)[entry.length] = '\0';
  *length = entry.length;
  return true;
}

static bool laghu_import_record_unique(laghu_import_context *context,
                                       const laghu_stylesheet_record *record) {
  unsigned int index;
  for (index = 0U; index < context->unique_count; ++index) {
    if (strcmp(context->unique[index], record->normalized_url) == 0) {
      return true;
    }
  }
  if (context->unique_count == LAGHU_CSS_MAX_IMPORTS ||
      strlen(record->normalized_url) >= LAGHU_RUNTIME_PATH_SIZE ||
      record->derived_length > SIZE_MAX - context->original_external_bytes) {
    return false;
  }
  strcpy(context->unique[context->unique_count++], record->normalized_url);
  context->original_external_bytes += record->derived_length;
  return true;
}

static bool laghu_import_record_dependency(
    laghu_import_context *context, const laghu_stylesheet_record *record,
    const char *media) {
  char line[LAGHU_RUNTIME_PATH_SIZE + LAGHU_RUNTIME_KEY_SIZE * 3U +
            LAGHU_CSS_IMPORT_MEDIA_SIZE + 16U];
  int length = snprintf(line, sizeof(line), "%s\n%s\n%s\n%s\n%s\n",
                        record->normalized_url, record->source_hash,
                        record->derived_key, record->dependency_key, media);
  return length > 0 && (size_t)length < sizeof(line) &&
         laghu_import_append(&context->dependency_material, line,
                             (size_t)length);
}

static bool laghu_import_validate(laghu_import_context *context,
                                  laghu_buffer css, const char *path,
                                  unsigned int depth) {
  laghu_css_parse_result *parsed = calloc(1U, sizeof(*parsed));
  size_t index;
  bool success = false;
  if (parsed == NULL || depth >= LAGHU_CSS_IMPORT_MAX_DEPTH ||
      context->stack_count >= LAGHU_CSS_IMPORT_MAX_DEPTH ||
      strlen(path) >= LAGHU_RUNTIME_PATH_SIZE ||
      !laghu_css_discover(css, path, context->page_origin, parsed) ||
      !parsed->valid || !parsed->bounded || parsed->import_graph_forbidden ||
      (parsed->has_imports && !parsed->imports_supported)) {
    context->invalid = true;
    goto finished;
  }
  strcpy(context->stack[context->stack_count++], path);
  for (index = 0U; index < parsed->import_count; ++index) {
    laghu_stylesheet_record record = {0};
    unsigned char *nested = NULL;
    size_t nested_length = 0U;
    const char *key;
    if (laghu_import_stack_contains(context,
                                    parsed->imports[index].source_url)) {
      context->invalid = true;
      goto pop;
    }
    if (!laghu_stylesheet_lookup(
            context->cache_path, parsed->imports[index].source_url,
            context->policy_key, context->capability_mask,
            context->inline_limit, context->outline_threshold, context->now,
            context->ttl_seconds, &record)) {
      context->pending = true;
      continue;
    }
    if (record.terminally_excluded) {
      context->invalid = true;
      goto pop;
    }
    key = record.ready ? record.derived_key : record.source_key;
    if (key[0] == '\0' ||
        !laghu_import_read(context->cache_path, key, &nested, &nested_length)) {
      context->pending = true;
      continue;
    }
    if (!laghu_import_validate(context, (laghu_buffer){nested, nested_length},
                               record.normalized_url, depth + 1U)) {
      free(nested);
      goto pop;
    }
    free(nested);
    if (!record.ready) {
      context->pending = true;
    }
  }
  success = !context->invalid;
pop:
  --context->stack_count;
finished:
  free(parsed);
  return success;
}

static bool laghu_import_flatten(laghu_import_context *context,
                                 laghu_buffer css, const char *path,
                                 unsigned int depth,
                                 laghu_import_builder *output) {
  laghu_css_parse_result *parsed = calloc(1U, sizeof(*parsed));
  size_t cursor = 0U;
  size_t index;
  bool success = false;
  if (parsed == NULL || depth >= LAGHU_CSS_IMPORT_MAX_DEPTH ||
      !laghu_css_discover(css, path, context->page_origin, parsed) ||
      !parsed->valid || !parsed->bounded || parsed->import_graph_forbidden ||
      (parsed->has_imports && !parsed->imports_supported)) {
    context->invalid = true;
    goto finished;
  }
  for (index = 0U; index < parsed->import_count; ++index) {
    const laghu_css_import *import = &parsed->imports[index];
    laghu_stylesheet_record record = {0};
    unsigned char *nested = NULL;
    size_t nested_length = 0U;
    laghu_image_markup_result rebased = {0};
    if (!laghu_import_append(output, css.data + cursor,
                             import->start - cursor)) {
      goto finished;
    }
    if (!laghu_stylesheet_lookup(context->cache_path, import->source_url,
                                 context->policy_key, context->capability_mask,
                                 context->inline_limit,
                                 context->outline_threshold, context->now,
                                 context->ttl_seconds, &record)) {
      context->pending = true;
      goto finished;
    }
    if (record.terminally_excluded) {
      context->invalid = true;
      goto finished;
    }
    if (!record.ready ||
        !laghu_import_read(context->cache_path, record.derived_key, &nested,
                           &nested_length)) {
      free(nested);
      context->pending = true;
      goto finished;
    }
    if (!laghu_import_record_unique(context, &record) ||
        !laghu_import_record_dependency(context, &record, import->media)) {
      free(nested);
      context->invalid = true;
      goto finished;
    }
    if (!laghu_css_rebase_urls((laghu_buffer){nested, nested_length},
                               record.normalized_url, context->page_origin,
                               &rebased)) {
      free(nested);
      context->invalid = true;
      goto finished;
    }
    free(nested);
    nested = rebased.data;
    nested_length = rebased.length;
    rebased.data = NULL;
    if (import->media[0] != '\0' &&
        (!laghu_import_append(output, "@media ", 7U) ||
         !laghu_import_append(output, import->media, strlen(import->media)) ||
         !laghu_import_append(output, "{", 1U))) {
      free(nested);
      goto finished;
    }
    if (!laghu_import_flatten(context, (laghu_buffer){nested, nested_length},
                              record.normalized_url, depth + 1U, output)) {
      free(nested);
      goto finished;
    }
    free(nested);
    if (import->media[0] != '\0' && !laghu_import_append(output, "}", 1U)) {
      goto finished;
    }
    if (!laghu_import_append(output, "\n", 1U)) {
      goto finished;
    }
    cursor = import->end;
  }
  success = laghu_import_append(output, css.data + cursor, css.length - cursor);
finished:
  free(parsed);
  return success;
}

bool laghu_runtime_flatten_css_imports(
    const char *cache_path, laghu_buffer css, const char *stylesheet_path,
    const char *page_origin, const char *policy_key, uint32_t capability_mask,
    uint64_t now, unsigned int ttl_seconds, unsigned int inline_limit,
    unsigned int outline_threshold, laghu_runtime_css_import_result *result) {
  laghu_import_context context = {0};
  laghu_import_builder output = {0};
  laghu_css_parse_result *parsed = NULL;
  bool success = false;
  if (result == NULL || cache_path == NULL || stylesheet_path == NULL ||
      policy_key == NULL || css.length > LAGHU_CSS_MAX_INPUT_BYTES) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  parsed = calloc(1U, sizeof(*parsed));
  if (parsed == NULL ||
      !laghu_css_discover(css, stylesheet_path, page_origin, parsed) ||
      !parsed->valid || !parsed->bounded) {
    goto finished;
  }
  if (!parsed->has_imports) {
    success = true;
    goto finished;
  }
  if (!parsed->imports_supported || parsed->import_graph_forbidden) {
    result->invalid = true;
    success = true;
    goto finished;
  }
  context.cache_path = cache_path;
  context.page_origin = page_origin;
  context.policy_key = policy_key;
  context.capability_mask = capability_mask;
  context.now = now;
  context.ttl_seconds = ttl_seconds;
  context.inline_limit = inline_limit;
  context.outline_threshold = outline_threshold;
  if (!laghu_import_validate(&context, css, stylesheet_path, 0U)) {
    result->invalid = context.invalid;
    success = true;
    goto finished;
  }
  if (context.pending) {
    result->dependencies_pending = true;
    success = true;
    goto finished;
  }
  context.stack_count = 0U;
  if (!laghu_import_flatten(&context, css, stylesheet_path, 0U, &output)) {
    result->dependencies_pending = context.pending;
    result->invalid = context.invalid;
    success = context.pending || context.invalid;
    goto finished;
  }
  if (!laghu_sha256_hex((laghu_buffer){context.dependency_material.data,
                                       context.dependency_material.length},
                        result->dependency_key)) {
    goto finished;
  }
  result->data = output.data;
  result->length = output.length;
  result->original_external_bytes = context.original_external_bytes;
  result->flattened = true;
  output.data = NULL;
  success = true;
finished:
  free(parsed);
  free(output.data);
  free(context.dependency_material.data);
  if (!success) {
    laghu_runtime_css_import_result_release(result);
  }
  return success;
}

void laghu_runtime_css_import_result_release(
    laghu_runtime_css_import_result *result) {
  if (result != NULL) {
    free(result->data);
    memset(result, 0, sizeof(*result));
  }
}
