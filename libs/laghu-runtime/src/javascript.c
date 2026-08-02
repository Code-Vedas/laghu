// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/runtime.h"

#define LAGHU_JAVASCRIPT_DEFAULT_TARGET \
  "defaults and supports es6-module and not dead"
#define LAGHU_JAVASCRIPT_CATALOG_MAGIC UINT64_C(0x4c414748554a5343)
#define LAGHU_JAVASCRIPT_CATALOG_SIZE 1908U
#define LAGHU_JAVASCRIPT_FLAG_MODULE UINT32_C(1)
#define LAGHU_JAVASCRIPT_FLAG_URL_INDEPENDENT UINT32_C(2)
#define LAGHU_JAVASCRIPT_FLAG_CONCAT_SAFE UINT32_C(4)
#define LAGHU_JAVASCRIPT_FLAG_TOP_LEVEL_DECLARATION_FREE UINT32_C(8)
#define LAGHU_JAVASCRIPT_FLAG_DEFER_SAFE UINT32_C(16)
#define LAGHU_JAVASCRIPT_COMBINE_MAX 16U
#define LAGHU_JAVASCRIPT_COMBINE_GROUP_SIZE 65U

typedef struct {
  char variant[LAGHU_RUNTIME_KEY_SIZE];
  uint64_t source_length;
  uint64_t derived_length;
  uint32_t flags;
} laghu_javascript_catalog_record;

typedef struct {
  unsigned char *data;
  size_t length;
  size_t capacity;
} laghu_javascript_builder;

typedef struct {
  size_t start;
  size_t end;
  char url[LAGHU_RUNTIME_PATH_SIZE];
  char group[LAGHU_JAVASCRIPT_COMBINE_GROUP_SIZE];
  char type[24U];
  char nonce[129U];
  laghu_javascript_catalog_record record;
  unsigned char *body;
  size_t body_length;
  char map_variant[LAGHU_RUNTIME_KEY_SIZE];
} laghu_javascript_combine_script;

typedef struct {
  unsigned char *data;
  size_t length;
  size_t original_external_bytes;
  size_t combined_external_bytes;
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];
  bool rewritten;
} laghu_javascript_combine_result;

static bool laghu_copy(char *target, size_t size, const char *source) {
  size_t length = source == NULL ? 0U : strlen(source);
  if (source == NULL || length >= size) return false;
  memcpy(target, source, length + 1U);
  return true;
}

bool laghu_javascript_target_normalize(
    const char *target, char output[LAGHU_JAVASCRIPT_TARGET_SIZE]) {
  size_t start = 0U, end, written = 0U;
  bool space = false;
  if (output == NULL) return false;
  output[0] = '\0';
  if (target == NULL) target = LAGHU_JAVASCRIPT_DEFAULT_TARGET;
  end = strlen(target);
  while (start < end && isspace((unsigned char)target[start])) ++start;
  while (end > start && isspace((unsigned char)target[end - 1U])) --end;
  if (start == end || end - start >= LAGHU_JAVASCRIPT_TARGET_SIZE) return false;
  for (; start < end; ++start) {
    unsigned char value = (unsigned char)target[start];
    if (iscntrl(value) || value == '$' ||
        (value == '.' && start + 1U < end && target[start + 1U] == '.'))
      return false;
    if (isspace(value)) {
      space = written != 0U;
    } else {
      if (space) output[written++] = ' ';
      output[written++] = (char)tolower(value);
      space = false;
    }
  }
  output[written] = '\0';
  return written != 0U && strstr(output, "extends ") == NULL;
}

static bool laghu_javascript_key(laghu_buffer source, const char *path,
                                 const char *policy, const char *target,
                                 bool module, bool include_source_map,
                                 char output[LAGHU_RUNTIME_KEY_SIZE],
                                 char source_hash[LAGHU_RUNTIME_KEY_SIZE]) {
  char canonical[LAGHU_RUNTIME_PATH_SIZE + LAGHU_RUNTIME_KEY_SIZE +
                 LAGHU_JAVASCRIPT_TARGET_SIZE + 96U];
  int length;
  if (!laghu_sha256_hex(source, source_hash)) return false;
  length = snprintf(
      canonical, sizeof(canonical), "laghu-js-v%u\n%s\n%s\n%s\n%s\n%c\n%c",
      LAGHU_JAVASCRIPT_DERIVATION_VERSION, source_hash, path, policy, target,
      module ? 'm' : 'c', include_source_map ? 's' : '-');
  return length > 0 && (size_t)length < sizeof(canonical) &&
         laghu_sha256_hex(
             (laghu_buffer){(const unsigned char *)canonical, (size_t)length},
             output);
}

bool laghu_runtime_rewrite_javascript(
    laghu_runtime_queue *queue, const char *cache_path, laghu_buffer source,
    const char *normalized_path, const char *policy_key, const char *target,
    bool module, bool include_source_map,
    laghu_runtime_javascript_result *result) {
  laghu_runtime_cache_entry entry;
  laghu_runtime_job job;
  char normalized_target[LAGHU_JAVASCRIPT_TARGET_SIZE];
  char key[LAGHU_RUNTIME_KEY_SIZE], source_hash[LAGHU_RUNTIME_KEY_SIZE];
  if (result == NULL) return false;
  memset(result, 0, sizeof(*result));
  if (queue == NULL || cache_path == NULL || source.data == NULL ||
      source.length == 0U || source.length > LAGHU_JAVASCRIPT_MAX_BYTES ||
      normalized_path == NULL || policy_key == NULL ||
      !laghu_javascript_target_normalize(target, normalized_target) ||
      !laghu_javascript_key(source, normalized_path, policy_key,
                            normalized_target, module, include_source_map, key,
                            source_hash))
    return false;
  if (laghu_runtime_cache_lookup(cache_path, key, source_hash, &entry) &&
      entry.length > 0U && entry.length < source.length &&
      entry.length <= LAGHU_JAVASCRIPT_MAX_BYTES) {
    result->data = malloc(entry.length);
    if (result->data != NULL &&
        laghu_runtime_cache_read(&entry, result->data, entry.length)) {
      result->length = entry.length;
      result->rewritten = true;
      memcpy(result->dependency_key, entry.payload_hash,
             sizeof(result->dependency_key));
      return true;
    }
    free(result->data);
    memset(result, 0, sizeof(*result));
  }
  memset(&job, 0, sizeof(job));
  job.kind = LAGHU_RUNTIME_JOB_JAVASCRIPT;
  job.filters = (module ? UINT64_C(1) : UINT64_C(0)) |
                (include_source_map ? UINT64_C(2) : UINT64_C(0));
  job.payload = source;
  if (!laghu_copy(job.index_key, sizeof(job.index_key), key) ||
      !laghu_copy(job.request_path, sizeof(job.request_path),
                  normalized_path) ||
      !laghu_copy(job.validator, sizeof(job.validator), source_hash) ||
      !laghu_copy(job.content_type, sizeof(job.content_type),
                  "application/javascript") ||
      !laghu_copy(job.policy_key, sizeof(job.policy_key), policy_key) ||
      !laghu_copy(job.javascript_target, sizeof(job.javascript_target),
                  normalized_target))
    return false;
  result->published = laghu_runtime_queue_try_publish(queue, &job);
  return true;
}

void laghu_runtime_javascript_result_release(
    laghu_runtime_javascript_result *result) {
  if (result == NULL) return;
  free(result->data);
  memset(result, 0, sizeof(*result));
}

static unsigned char laghu_lower(unsigned char value) {
  return value >= 'A' && value <= 'Z' ? (unsigned char)(value + 32U) : value;
}

static const unsigned char *laghu_find_ascii(const unsigned char *start,
                                             const unsigned char *end,
                                             const char *needle) {
  size_t length = strlen(needle), index;
  for (; (size_t)(end - start) >= length; ++start) {
    for (index = 0U; index < length; ++index)
      if (laghu_lower(start[index]) != (unsigned char)needle[index]) break;
    if (index == length) return start;
  }
  return NULL;
}

static bool laghu_tag_attribute(const unsigned char *start,
                                const unsigned char *end, const char *name,
                                const unsigned char **value,
                                size_t *value_length) {
  size_t length = strlen(name);
  while (start < end) {
    const unsigned char *key, *key_end, *item;
    while (start < end && isspace(*start)) ++start;
    key = start;
    while (start < end && (isalnum(*start) || *start == '-' || *start == '_'))
      ++start;
    key_end = start;
    while (start < end && isspace(*start)) ++start;
    item = NULL;
    *value_length = 0U;
    if (start < end && *start == '=') {
      unsigned char quote = 0U;
      ++start;
      while (start < end && isspace(*start)) ++start;
      if (start < end && (*start == '\'' || *start == '"')) quote = *start++;
      item = start;
      if (quote != 0U)
        while (start < end && *start != quote) ++start;
      else
        while (start < end && !isspace(*start) && *start != '>') ++start;
      *value_length = (size_t)(start - item);
      if (start < end && quote != 0U) ++start;
    }
    if ((size_t)(key_end - key) == length) {
      size_t index;
      for (index = 0U; index < length; ++index)
        if (laghu_lower(key[index]) != (unsigned char)name[index]) break;
      if (index == length) {
        *value = item;
        return true;
      }
    }
    if (key == key_end && start < end) ++start;
  }
  return false;
}

static bool laghu_tag_attributes_allowed(const unsigned char *start,
                                         const unsigned char *end,
                                         bool combine) {
  while (start < end) {
    const unsigned char *key;
    size_t length;
    bool allowed;
    while (start < end && isspace(*start)) ++start;
    key = start;
    while (start < end && (isalnum(*start) || *start == '-' || *start == '_'))
      ++start;
    length = (size_t)(start - key);
    if (length == 0U) return false;
    allowed =
        (length == 3U && laghu_find_ascii(key, start, "src") == key) ||
        (length == 4U && laghu_find_ascii(key, start, "type") == key) ||
        (length == 5U && laghu_find_ascii(key, start, "nonce") == key) ||
        (!combine && length == 2U &&
         laghu_find_ascii(key, start, "id") == key) ||
        (length >= 5U && laghu_find_ascii(key, key + 5U, "data-") == key &&
         (!combine ||
          (length == 18U &&
           laghu_find_ascii(key, start, "data-laghu-combine") == key)));
    if (!allowed) return false;
    while (start < end && isspace(*start)) ++start;
    if (start == end || *start++ != '=') return false;
    while (start < end && isspace(*start)) ++start;
    if (start == end) return false;
    if (*start == '\'' || *start == '"') {
      unsigned char quote = *start++;
      while (start < end && *start != quote) ++start;
      if (start == end) return false;
      ++start;
    } else {
      while (start < end && !isspace(*start) && *start != '>') ++start;
    }
  }
  return true;
}

static bool laghu_tag_attribute_span(const unsigned char *tag,
                                     const unsigned char *tag_end,
                                     const unsigned char *value,
                                     size_t value_length,
                                     const unsigned char **span_start,
                                     const unsigned char **span_end) {
  const unsigned char *cursor = value;
  const unsigned char *end = value + value_length;
  if (tag == NULL || tag_end == NULL || value == NULL || value < tag ||
      end > tag_end)
    return false;
  if (cursor > tag && (cursor[-1] == '\'' || cursor[-1] == '"')) {
    unsigned char quote = cursor[-1];
    --cursor;
    if (end < tag_end && *end == quote) ++end;
  }
  while (cursor > tag && isspace(cursor[-1])) --cursor;
  if (cursor == tag || cursor[-1] != '=') return false;
  --cursor;
  while (cursor > tag && isspace(cursor[-1])) --cursor;
  while (cursor > tag &&
         (isalnum(cursor[-1]) || cursor[-1] == '-' || cursor[-1] == '_'))
    --cursor;
  while (cursor > tag && isspace(cursor[-1])) --cursor;
  *span_start = cursor;
  *span_end = end;
  return true;
}

static bool laghu_javascript_csp_accepts_nonce(const char *csp,
                                               const unsigned char *tag,
                                               const unsigned char *tag_end) {
  const unsigned char *nonce = NULL;
  size_t nonce_length = 0U;
  if (csp == NULL || tag == NULL || tag_end == NULL) return false;
  if (laghu_tag_attribute(tag, tag_end, "nonce", &nonce, &nonce_length) &&
      nonce != NULL && nonce_length > 0U && nonce_length <= 128U) {
    char token[160U];
    int length = snprintf(token, sizeof(token), "'nonce-%.*s'",
                          (int)nonce_length, (const char *)nonce);
    return length > 0 && (size_t)length < sizeof(token) &&
           strstr(csp, token) != NULL;
  }
  return false;
}

static bool laghu_javascript_csp_allows_inline(const char *csp,
                                               const unsigned char *tag,
                                               const unsigned char *tag_end) {
  if (csp == NULL || *csp == '\0') return true;
  return strstr(csp, "'unsafe-inline'") != NULL ||
         laghu_javascript_csp_accepts_nonce(csp, tag, tag_end);
}

static bool laghu_javascript_csp_allows_external(const char *csp,
                                                 const unsigned char *tag,
                                                 const unsigned char *tag_end) {
  return laghu_runtime_csp_allows_self_scripts(csp, NULL) ||
         laghu_javascript_csp_accepts_nonce(csp, tag, tag_end);
}

static bool laghu_javascript_external_lookup(
    const char *cache_path, const char *url, const char *policy_key,
    const char *target, bool module, uint64_t now, unsigned int ttl_seconds,
    laghu_javascript_catalog_record *record) {
  unsigned char catalog[LAGHU_JAVASCRIPT_CATALOG_SIZE];
  char canonical[LAGHU_RUNTIME_PATH_SIZE + LAGHU_RUNTIME_KEY_SIZE +
                 LAGHU_JAVASCRIPT_TARGET_SIZE + 64U];
  char key[LAGHU_RUNTIME_KEY_SIZE], checksum[LAGHU_RUNTIME_KEY_SIZE];
  char path[LAGHU_RUNTIME_PATH_SIZE];
  FILE *file;
  uint64_t magic, updated, source_length, variant_length;
  uint32_t version, stored_module, flags;
  int length;
  bool read_success;
  laghu_runtime_cache_entry entry;
  length =
      snprintf(canonical, sizeof(canonical), "laghu-js-url-v1\n%s\n%s\n%s\n%s",
               url, policy_key, target, module ? "module" : "classic");
  if (length <= 0 || (size_t)length >= sizeof(canonical) ||
      !laghu_sha256_hex(
          (laghu_buffer){(const unsigned char *)canonical, (size_t)length},
          key) ||
      (length = snprintf(path, sizeof(path), "%s/javascript-%s.meta",
                         cache_path, key)) <= 0 ||
      (size_t)length >= sizeof(path))
    return false;
  file = fopen(path, "rb");
  if (file == NULL) return false;
  read_success = fread(catalog, 1U, sizeof(catalog), file) == sizeof(catalog) &&
                 fgetc(file) == EOF;
  if (fclose(file) != 0) read_success = false;
  if (!read_success) return false;
  memcpy(&magic, catalog, sizeof(magic));
  memcpy(&version, catalog + 8U, sizeof(version));
  memcpy(&stored_module, catalog + 12U, sizeof(stored_module));
  memcpy(&updated, catalog + 16U, sizeof(updated));
  memcpy(&source_length, catalog + 24U, sizeof(source_length));
  memcpy(&variant_length, catalog + 32U, sizeof(variant_length));
  memcpy(&flags, catalog + 1836U, sizeof(flags));
  if (!laghu_sha256_hex((laghu_buffer){catalog, 1840U}, checksum) ||
      magic != LAGHU_JAVASCRIPT_CATALOG_MAGIC || version != 3U ||
      stored_module != (module ? 1U : 0U) || updated > now ||
      now - updated > ttl_seconds || source_length == 0U ||
      variant_length == 0U || variant_length >= source_length ||
      memchr(catalog + 40U, '\0', 1024U) == NULL ||
      memchr(catalog + 1064U, '\0', 65U) == NULL ||
      memchr(catalog + 1129U, '\0', 512U) == NULL ||
      memchr(catalog + 1840U, '\0', 65U) == NULL ||
      strcmp((const char *)catalog + 40U, url) != 0 ||
      strcmp((const char *)catalog + 1064U, policy_key) != 0 ||
      strcmp((const char *)catalog + 1129U, target) != 0 ||
      strcmp((const char *)catalog + 1840U, checksum) != 0 ||
      (flags &
       ~(LAGHU_JAVASCRIPT_FLAG_MODULE | LAGHU_JAVASCRIPT_FLAG_URL_INDEPENDENT |
         LAGHU_JAVASCRIPT_FLAG_CONCAT_SAFE |
         LAGHU_JAVASCRIPT_FLAG_TOP_LEVEL_DECLARATION_FREE |
         LAGHU_JAVASCRIPT_FLAG_DEFER_SAFE)) != 0U ||
      (((flags & LAGHU_JAVASCRIPT_FLAG_MODULE) != 0U) != module) ||
      (module && (flags & LAGHU_JAVASCRIPT_FLAG_CONCAT_SAFE) != 0U) ||
      memchr(catalog + 1706U, '\0', LAGHU_RUNTIME_KEY_SIZE) == NULL ||
      !laghu_runtime_cache_lookup_variant(
          cache_path, (const char *)catalog + 1706U, &entry) ||
      entry.length != variant_length)
    return false;
  memset(record, 0, sizeof(*record));
  memcpy(record->variant, catalog + 1706U, LAGHU_RUNTIME_KEY_SIZE);
  record->source_length = source_length;
  record->derived_length = variant_length;
  record->flags = flags;
  return true;
}

static bool laghu_javascript_rum_ready(
    laghu_rum_engine *rum, const char *template_key, const char *page_origin,
    const char *url, unsigned int bucket, uint64_t now,
    char evidence_key[LAGHU_RUNTIME_KEY_SIZE]) {
  laghu_rum_instrumentation_record record;
  laghu_rum_value value;
  char absolute[LAGHU_RUNTIME_PATH_SIZE * 2U];
  char script_key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int index;
  int length;
  if (rum == NULL || template_key == NULL || page_origin == NULL ||
      url == NULL || bucket > 1U ||
      !laghu_rum_engine_read(rum, LAGHU_RUM_RECORD_INSTRUMENTATION,
                             template_key, now, &record, sizeof(record),
                             &value) ||
      value.length != sizeof(record) ||
      record.version != LAGHU_INSTRUMENTATION_VERSION ||
      record.observations[bucket] < 100U)
    return false;
  length = snprintf(absolute, sizeof(absolute), "%s%s", page_origin, url);
  if (length <= 0 || (size_t)length >= sizeof(absolute) ||
      !laghu_sha256_hex(
          (laghu_buffer){(const unsigned char *)absolute, (size_t)length},
          script_key))
    return false;
  for (index = 0U; index < record.script_count; ++index)
    if (strcmp(record.script_keys[index], script_key) == 0) {
      char material[LAGHU_RUNTIME_KEY_SIZE * 2U + 160U];
      int material_length;
      if (!laghu_javascript_defer_recommended(&record, bucket, index))
        return false;
      if (evidence_key == NULL) return true;
      material_length = snprintf(
          material, sizeof(material),
          "rum-js-defer-evidence-v1\n%s\n%u\n%llu\n%llu\n%s\n%llu",
          template_key, bucket, (unsigned long long)record.updated_at,
          (unsigned long long)record.observations[bucket], script_key,
          (unsigned long long)record.script_observations[bucket][index]);
      return material_length > 0 &&
             (size_t)material_length < sizeof(material) &&
             laghu_sha256_hex((laghu_buffer){(const unsigned char *)material,
                                             (size_t)material_length},
                              evidence_key);
    }
  return false;
}

static bool laghu_javascript_append(laghu_javascript_builder *builder,
                                    const void *data, size_t length) {
  size_t capacity;
  unsigned char *next;
  if (length == 0U) return true;
  if (builder == NULL || data == NULL || length > SIZE_MAX - builder->length)
    return false;
  if (builder->length + length > builder->capacity) {
    capacity = builder->capacity == 0U ? 1024U : builder->capacity;
    while (capacity < builder->length + length) {
      if (capacity > SIZE_MAX / 2U) return false;
      capacity *= 2U;
    }
    next = realloc(builder->data, capacity);
    if (next == NULL) return false;
    builder->data = next;
    builder->capacity = capacity;
  }
  memcpy(builder->data + builder->length, data, length);
  builder->length += length;
  return true;
}

static bool laghu_javascript_copy_attribute(const unsigned char *value,
                                            size_t length, char *output,
                                            size_t output_size) {
  if (value == NULL || length == 0U || length >= output_size) return false;
  memcpy(output, value, length);
  output[length] = '\0';
  return true;
}

static bool laghu_javascript_group_valid(const char *group) {
  size_t index, length = strlen(group);
  if (length == 0U || length >= LAGHU_JAVASCRIPT_COMBINE_GROUP_SIZE)
    return false;
  for (index = 0U; index < length; ++index)
    if (!(isalnum((unsigned char)group[index]) || group[index] == '-' ||
          group[index] == '_' || group[index] == '.'))
      return false;
  return true;
}

static bool laghu_javascript_parse_combine_script(
    laghu_buffer html, size_t offset, laghu_javascript_combine_script *script) {
  const unsigned char *start, *open_end, *close, *close_end;
  const unsigned char *value = NULL;
  size_t length = 0U;
  if (script == NULL || offset >= html.length) return false;
  memset(script, 0, sizeof(*script));
  start = html.data + offset;
  if ((size_t)(html.data + html.length - start) < 7U ||
      laghu_find_ascii(start, start + 7U, "<script") != start)
    return false;
  open_end = memchr(start, '>', (size_t)(html.data + html.length - start));
  if (open_end == NULL) return false;
  if (!laghu_tag_attributes_allowed(start + 7U, open_end, true)) return false;
  close = laghu_find_ascii(open_end + 1U, html.data + html.length, "</script");
  if (close == NULL) return false;
  close_end = memchr(close, '>', (size_t)(html.data + html.length - close));
  if (close_end == NULL) return false;
  while (++open_end < close)
    if (!isspace(*open_end)) return false;
  open_end = memchr(start, '>', (size_t)(html.data + html.length - start));
  if (!laghu_tag_attribute(start + 7U, open_end, "src", &value, &length) ||
      !laghu_javascript_copy_attribute(value, length, script->url,
                                       sizeof(script->url)) ||
      script->url[0] != '/' || script->url[1] == '/')
    return false;
  if (!laghu_tag_attribute(start + 7U, open_end, "data-laghu-combine", &value,
                           &length) ||
      !laghu_javascript_copy_attribute(value, length, script->group,
                                       sizeof(script->group)) ||
      !laghu_javascript_group_valid(script->group))
    return false;
  if (laghu_tag_attribute(start + 7U, open_end, "type", &value, &length)) {
    if (!laghu_javascript_copy_attribute(value, length, script->type,
                                         sizeof(script->type)) ||
        (strcmp(script->type, "text/javascript") != 0 &&
         strcmp(script->type, "application/javascript") != 0))
      return false;
  }
  if (laghu_tag_attribute(start + 7U, open_end, "nonce", &value, &length) &&
      !laghu_javascript_copy_attribute(value, length, script->nonce,
                                       sizeof(script->nonce)))
    return false;
  if (laghu_tag_attribute(start + 7U, open_end, "integrity", &value, &length) ||
      laghu_tag_attribute(start + 7U, open_end, "async", &value, &length) ||
      laghu_tag_attribute(start + 7U, open_end, "defer", &value, &length) ||
      laghu_tag_attribute(start + 7U, open_end, "nomodule", &value, &length) ||
      laghu_tag_attribute(start + 7U, open_end, "onload", &value, &length) ||
      laghu_tag_attribute(start + 7U, open_end, "onerror", &value, &length) ||
      laghu_tag_attribute(start + 7U, open_end, "id", &value, &length))
    return false;
  script->start = offset;
  script->end = (size_t)(close_end + 1U - html.data);
  return true;
}

static bool laghu_javascript_combine_key(
    const laghu_javascript_combine_script *scripts, size_t count,
    const char *policy_key, const char *target,
    char output[LAGHU_RUNTIME_KEY_SIZE]) {
  laghu_javascript_builder material = {0};
  size_t index;
  static const char prefix[] = "laghu-js-combine-v1\n";
  bool success =
      laghu_javascript_append(&material, prefix, sizeof(prefix) - 1U);
  for (index = 0U; success && index < count; ++index) {
    success = laghu_javascript_append(&material, scripts[index].record.variant,
                                      strlen(scripts[index].record.variant)) &&
              laghu_javascript_append(&material, "\n", 1U);
  }
  success =
      success &&
      laghu_javascript_append(&material, scripts[0].group,
                              strlen(scripts[0].group)) &&
      laghu_javascript_append(&material, "\n", 1U) &&
      laghu_javascript_append(&material, scripts[0].type,
                              strlen(scripts[0].type)) &&
      laghu_javascript_append(&material, "\n", 1U) &&
      laghu_javascript_append(&material, scripts[0].nonce,
                              strlen(scripts[0].nonce)) &&
      laghu_javascript_append(&material, "\n", 1U) &&
      laghu_javascript_append(&material, policy_key, strlen(policy_key)) &&
      laghu_javascript_append(&material, "\n", 1U) &&
      laghu_javascript_append(&material, target, strlen(target)) &&
      laghu_sha256_hex((laghu_buffer){material.data, material.length}, output);
  free(material.data);
  return success;
}

static bool laghu_javascript_combine(const char *cache_path, laghu_buffer html,
                                     const char *policy_key, const char *target,
                                     uint64_t now, unsigned int ttl_seconds,
                                     bool include_source_maps,
                                     laghu_javascript_combine_result *result) {
  laghu_javascript_builder output = {0};
  size_t cursor = 0U;
  bool changed = false;
  memset(result, 0, sizeof(*result));
  while (cursor < html.length) {
    const unsigned char *found = laghu_find_ascii(
        html.data + cursor, html.data + html.length, "<script");
    laghu_javascript_combine_script scripts[LAGHU_JAVASCRIPT_COMBINE_MAX];
    size_t count = 0U, group_end, scan, index;
    laghu_javascript_builder combined = {0};
    laghu_runtime_cache_entry entry;
    char key[LAGHU_RUNTIME_KEY_SIZE];
    char markup[512U];
    int markup_length = 0;
    bool ready = true, overflow = false, indexed_map = include_source_maps;
    if (found == NULL) {
      ready = laghu_javascript_append(&output, html.data + cursor,
                                      html.length - cursor);
      if (!ready) goto failed;
      break;
    }
    scan = (size_t)(found - html.data);
    if (!laghu_javascript_parse_combine_script(html, scan, &scripts[0])) {
      if (!laghu_javascript_append(&output, html.data + cursor,
                                   scan + 1U - cursor))
        goto failed;
      cursor = scan + 1U;
      continue;
    }
    count = 1U;
    group_end = scripts[0].end;
    while (count < LAGHU_JAVASCRIPT_COMBINE_MAX) {
      scan = group_end;
      while (scan < html.length && isspace(html.data[scan])) ++scan;
      if (!laghu_javascript_parse_combine_script(html, scan, &scripts[count]) ||
          strcmp(scripts[count].group, scripts[0].group) != 0 ||
          strcmp(scripts[count].type, scripts[0].type) != 0 ||
          strcmp(scripts[count].nonce, scripts[0].nonce) != 0)
        break;
      group_end = scripts[count].end;
      ++count;
    }
    if (count == LAGHU_JAVASCRIPT_COMBINE_MAX) {
      laghu_javascript_combine_script extra;
      scan = group_end;
      while (scan < html.length && isspace(html.data[scan])) ++scan;
      while (laghu_javascript_parse_combine_script(html, scan, &extra) &&
             strcmp(extra.group, scripts[0].group) == 0 &&
             strcmp(extra.type, scripts[0].type) == 0 &&
             strcmp(extra.nonce, scripts[0].nonce) == 0) {
        overflow = true;
        group_end = extra.end;
        scan = group_end;
        while (scan < html.length && isspace(html.data[scan])) ++scan;
      }
    }
    if (count < 2U || overflow) {
      if (!laghu_javascript_append(&output, html.data + cursor,
                                   group_end - cursor))
        goto failed;
      cursor = group_end;
      continue;
    }
    for (index = 0U; index < count; ++index) {
      if (!laghu_javascript_external_lookup(
              cache_path, scripts[index].url, policy_key, target, false, now,
              ttl_seconds, &scripts[index].record) ||
          (scripts[index].record.flags & LAGHU_JAVASCRIPT_FLAG_CONCAT_SAFE) ==
              0U ||
          (scripts[index].record.flags &
           LAGHU_JAVASCRIPT_FLAG_TOP_LEVEL_DECLARATION_FREE) == 0U ||
          !laghu_runtime_cache_lookup_variant(
              cache_path, scripts[index].record.variant, &entry)) {
        ready = false;
        break;
      }
      scripts[index].body = malloc(entry.length);
      scripts[index].body_length = entry.length;
      if (scripts[index].body == NULL ||
          !laghu_runtime_cache_read(&entry, scripts[index].body,
                                    entry.length)) {
        ready = false;
        break;
      }
      if (include_source_maps) {
        static const char marker[] = "\n//# sourcemappingurl=/.laghu/js/";
        const unsigned char *map = laghu_find_ascii(
            scripts[index].body,
            scripts[index].body + scripts[index].body_length, marker);
        size_t remaining = map == NULL
                               ? 0U
                               : (size_t)(scripts[index].body +
                                          scripts[index].body_length - map);
        if (map == NULL ||
            remaining != sizeof(marker) - 1U + LAGHU_SHA256_HEX_LENGTH + 4U ||
            memcmp(map + remaining - 4U, ".map", 4U) != 0) {
          indexed_map = false;
        } else {
          memcpy(scripts[index].map_variant, map + sizeof(marker) - 1U,
                 LAGHU_SHA256_HEX_LENGTH);
          scripts[index].map_variant[LAGHU_SHA256_HEX_LENGTH] = '\0';
          scripts[index].body_length = (size_t)(map - scripts[index].body);
        }
      }
    }
    if (ready) {
      laghu_javascript_builder indexed = {0};
      size_t line = 0U;
      if (indexed_map && !laghu_javascript_append(
                             &indexed, "{\"version\":3,\"sections\":[",
                             sizeof("{\"version\":3,\"sections\":[") - 1U))
        ready = false;
      for (index = 0U; ready && index < count; ++index) {
        char section[256U];
        int section_length = 0;
        size_t byte;
        if (index != 0U && !laghu_javascript_append(&combined, ";\n", 2U)) {
          ready = false;
          break;
        }
        if (indexed_map) {
          section_length = snprintf(
              section, sizeof(section),
              "%s{\"offset\":{\"line\":%zu,\"column\":0},\"url\":"
              "\"/.laghu/js/%s.map\"}",
              index == 0U ? "" : ",", line, scripts[index].map_variant);
          if (section_length <= 0 ||
              (size_t)section_length >= sizeof(section) ||
              !laghu_javascript_append(&indexed, section,
                                       (size_t)section_length)) {
            ready = false;
            break;
          }
        }
        if (!laghu_javascript_append(&combined, scripts[index].body,
                                     scripts[index].body_length) ||
            combined.length > LAGHU_JAVASCRIPT_MAX_BYTES) {
          ready = false;
          break;
        }
        for (byte = 0U; byte < scripts[index].body_length; ++byte)
          if (scripts[index].body[byte] == '\n') ++line;
        if (index + 1U < count) ++line;
      }
      if (ready && indexed_map) {
        laghu_runtime_cache_entry map_entry;
        char map_key[LAGHU_RUNTIME_KEY_SIZE];
        char directive[LAGHU_RUNTIME_KEY_SIZE + 48U];
        int directive_length;
        if (!laghu_javascript_append(&indexed, "]}", 2U) ||
            !laghu_sha256_hex((laghu_buffer){indexed.data, indexed.length},
                              map_key) ||
            !laghu_runtime_cache_publish(
                cache_path, map_key, map_key, map_key, "application/json",
                "laghu-js-indexed-map-v1",
                (laghu_buffer){indexed.data, indexed.length}, &map_entry) ||
            (directive_length = snprintf(
                 directive, sizeof(directive),
                 "\n//# sourceMappingURL=/.laghu/js/%s.map", map_key)) <= 0 ||
            (size_t)directive_length >= sizeof(directive) ||
            !laghu_javascript_append(&combined, directive,
                                     (size_t)directive_length))
          indexed_map = false;
      }
      free(indexed.data);
    }
    if (ready &&
        (!laghu_javascript_combine_key(scripts, count, policy_key, target,
                                       key) ||
         (!laghu_runtime_cache_lookup_variant(cache_path, key, &entry) &&
          !laghu_runtime_cache_publish(
              cache_path, key, key, key, "application/javascript",
              "laghu-js-combine-v1",
              (laghu_buffer){combined.data, combined.length}, &entry))))
      ready = false;
    if (ready) {
      markup_length = snprintf(
          markup, sizeof(markup),
          "<script src=\"/.laghu/js/%s\" "
          "data-laghu-combine=\"%s\"%s%s%s%s%s%s></script>",
          key, scripts[0].group, scripts[0].type[0] == '\0' ? "" : " type=\"",
          scripts[0].type, scripts[0].type[0] == '\0' ? "" : "\"",
          scripts[0].nonce[0] == '\0' ? "" : " nonce=\"", scripts[0].nonce,
          scripts[0].nonce[0] == '\0' ? "" : "\"");
    }
    if (ready && markup_length > 0 && (size_t)markup_length < sizeof(markup) &&
        laghu_javascript_append(&output, html.data + cursor,
                                scripts[0].start - cursor) &&
        laghu_javascript_append(&output, markup, (size_t)markup_length)) {
      for (index = 0U; index < count; ++index)
        result->original_external_bytes += scripts[index].record.derived_length;
      result->combined_external_bytes += combined.length;
      if (result->dependency_key[0] == '\0') {
        memcpy(result->dependency_key, key, sizeof(result->dependency_key));
      } else {
        char dependency_material[LAGHU_RUNTIME_KEY_SIZE * 2U + 2U];
        int dependency_length =
            snprintf(dependency_material, sizeof(dependency_material), "%s\n%s",
                     result->dependency_key, key);
        if (dependency_length <= 0 ||
            (size_t)dependency_length >= sizeof(dependency_material) ||
            !laghu_sha256_hex(
                (laghu_buffer){(const unsigned char *)dependency_material,
                               (size_t)dependency_length},
                result->dependency_key)) {
          for (index = 0U; index < count; ++index) free(scripts[index].body);
          free(combined.data);
          goto failed;
        }
      }
      changed = true;
      cursor = group_end;
    } else {
      if (!laghu_javascript_append(&output, html.data + cursor,
                                   group_end - cursor))
        goto failed;
      cursor = group_end;
    }
    for (index = 0U; index < count; ++index) free(scripts[index].body);
    free(combined.data);
  }
  if (changed) {
    result->data = output.data;
    result->length = output.length;
    result->rewritten = true;
  } else {
    free(output.data);
  }
  return true;
failed:
  free(output.data);
  return false;
}

static bool laghu_javascript_defer_suffix_ready(
    const char *cache_path, const char *policy_key, const char *target,
    const char *page_path, const char *page_origin, const unsigned char *cursor,
    const unsigned char *end, uint64_t now, unsigned int ttl_seconds,
    laghu_rum_engine *rum, const char *template_key,
    unsigned int viewport_bucket, const laghu_javascript_defer_set *defer_set) {
  while ((cursor = laghu_find_ascii(cursor, end, "<script")) != NULL) {
    const unsigned char *open_end = memchr(cursor, '>', (size_t)(end - cursor));
    const unsigned char *close;
    const unsigned char *source = NULL, *attribute = NULL;
    size_t source_length = 0U, attribute_length = 0U;
    laghu_javascript_catalog_record record;
    char url[LAGHU_RUNTIME_PATH_SIZE];
    if (open_end == NULL) return false;
    close = laghu_find_ascii(open_end + 1U, end, "</script");
    if (close == NULL ||
        !laghu_tag_attribute(cursor + 7U, open_end, "src", &source,
                             &source_length) ||
        source == NULL || source_length == 0U || source_length >= sizeof(url) ||
        source[0] != '/' ||
        laghu_tag_attribute(cursor + 7U, open_end, "integrity", &attribute,
                            &attribute_length) ||
        laghu_tag_attribute(cursor + 7U, open_end, "async", &attribute,
                            &attribute_length) ||
        laghu_tag_attribute(cursor + 7U, open_end, "defer", &attribute,
                            &attribute_length) ||
        laghu_tag_attribute(cursor + 7U, open_end, "nomodule", &attribute,
                            &attribute_length) ||
        !laghu_tag_attributes_allowed(cursor + 7U, open_end, false))
      return false;
    if (laghu_tag_attribute(cursor + 7U, open_end, "type", &attribute,
                            &attribute_length) &&
        !((attribute_length == 15U &&
           laghu_find_ascii(attribute, attribute + attribute_length,
                            "text/javascript") == attribute) ||
          (attribute_length == 22U &&
           laghu_find_ascii(attribute, attribute + attribute_length,
                            "application/javascript") == attribute)))
      return false;
    memcpy(url, source, source_length);
    url[source_length] = '\0';
    if (strchr(url, '?') != NULL || strchr(url, '#') != NULL ||
        !laghu_javascript_external_lookup(cache_path, url, policy_key, target,
                                          false, now, ttl_seconds, &record) ||
        (record.flags & LAGHU_JAVASCRIPT_FLAG_DEFER_SAFE) == 0U ||
        !laghu_javascript_rum_ready(rum, template_key, page_origin, url,
                                    viewport_bucket, now, NULL) ||
        !laghu_javascript_defer_approved(defer_set, url, page_path))
      return false;
    cursor = close + 8U;
  }
  return true;
}

bool laghu_runtime_rewrite_javascript_html(
    laghu_runtime_queue *queue, const char *cache_path, laghu_buffer html,
    const char *page_path, const char *policy_key, const char *target,
    const char *content_security_policy, uint64_t now, unsigned int ttl_seconds,
    laghu_rum_engine *rum, const char *template_key, const char *page_origin,
    unsigned int viewport_bucket, const laghu_javascript_defer_set *defer_set,
    bool allow_defer, bool allow_defer_suggestions, bool allow_combine,
    bool allow_inline, bool allow_outline, bool include_source_maps,
    unsigned int inline_limit, unsigned int outline_threshold,
    laghu_runtime_html_result *result) {
  const unsigned char *cursor, *end;
  unsigned char *output = NULL;
  size_t output_length = 0U, script_count = 0U;
  size_t original_bundle = html.length;
  size_t rewritten_external = 0U;
  size_t defer_growth = 0U;
  char normalized_target[LAGHU_JAVASCRIPT_TARGET_SIZE];
  char dependencies[LAGHU_JAVASCRIPT_MAX_SCRIPTS][LAGHU_RUNTIME_KEY_SIZE];
  size_t dependency_count = 0U;
  laghu_javascript_combine_result combined;
  laghu_runtime_cache_entry entry;
  if (result == NULL) return false;
  memset(result, 0, sizeof(*result));
  if (queue == NULL || cache_path == NULL || html.data == NULL ||
      page_path == NULL || policy_key == NULL)
    return true;
  if (!laghu_javascript_target_normalize(target, normalized_target))
    return true;
  memset(&combined, 0, sizeof(combined));
  if (allow_combine) {
    if (!laghu_javascript_combine(cache_path, html, policy_key,
                                  normalized_target, now, ttl_seconds,
                                  include_source_maps, &combined))
      return false;
    if (combined.rewritten) {
      output = combined.data;
      output_length = combined.length;
      original_bundle += combined.original_external_bytes;
      rewritten_external += combined.combined_external_bytes;
      memcpy(dependencies[dependency_count++], combined.dependency_key,
             LAGHU_RUNTIME_KEY_SIZE);
      html = (laghu_buffer){output, output_length};
    }
  }
  cursor = html.data;
  end = html.data + html.length;
  while ((cursor = laghu_find_ascii(cursor, end, "<script")) != NULL) {
    const unsigned char *open_end = memchr(cursor, '>', (size_t)(end - cursor));
    const unsigned char *close;
    const unsigned char *attribute = NULL;
    size_t attribute_length = 0U;
    bool module = false, accepted = true;
    laghu_runtime_javascript_result javascript;
    char location[LAGHU_RUNTIME_PATH_SIZE];
    int location_length;
    if (open_end == NULL || ++script_count > LAGHU_JAVASCRIPT_MAX_SCRIPTS) {
      free(output);
      return true;
    }
    close = laghu_find_ascii(open_end + 1U, end, "</script");
    if (close == NULL) {
      free(output);
      return true;
    }
    {
      const unsigned char *source_attribute = NULL;
      size_t source_length = 0U;
      bool external = laghu_tag_attribute(cursor + 7U, open_end, "src",
                                          &source_attribute, &source_length);
      if (laghu_tag_attribute(cursor + 7U, open_end, "integrity", &attribute,
                              &attribute_length))
        accepted = false;
      if (laghu_tag_attribute(cursor + 7U, open_end, "type", &attribute,
                              &attribute_length)) {
        module = attribute != NULL && attribute_length == 6U &&
                 laghu_find_ascii(attribute, attribute + attribute_length,
                                  "module") == attribute;
        if (!module &&
            !(attribute_length == 15U &&
              laghu_find_ascii(attribute, attribute + attribute_length,
                               "text/javascript") == attribute) &&
            !(attribute_length == 22U &&
              laghu_find_ascii(attribute, attribute + attribute_length,
                               "application/javascript") == attribute))
          accepted = false;
      }
      if (content_security_policy != NULL &&
          (strstr(content_security_policy, "sha256-") != NULL ||
           strstr(content_security_policy, "sha384-") != NULL ||
           strstr(content_security_policy, "sha512-") != NULL) &&
          !laghu_tag_attribute(cursor + 7U, open_end, "nonce", &attribute,
                               &attribute_length))
        accepted = false;
      if (laghu_find_ascii(open_end + 1U, close, "sourcemappingurl") != NULL ||
          laghu_find_ascii(open_end + 1U, close, "sourceurl") != NULL)
        accepted = false;
      if (external) {
        char url[LAGHU_RUNTIME_PATH_SIZE];
        char defer_evidence_key[LAGHU_RUNTIME_KEY_SIZE];
        laghu_javascript_catalog_record record;
        char route[LAGHU_RUNTIME_KEY_SIZE + 16U];
        int route_length;
        if (accepted && source_attribute != NULL && source_length > 0U &&
            source_length < sizeof(url) && source_attribute[0] == '/') {
          memcpy(url, source_attribute, source_length);
          url[source_length] = '\0';
          if (laghu_javascript_external_lookup(cache_path, url, policy_key,
                                               normalized_target, module, now,
                                               ttl_seconds, &record)) {
            bool defer_ready =
                allow_defer && !module && strchr(url, '?') == NULL &&
                strchr(url, '#') == NULL &&
                (record.flags & LAGHU_JAVASCRIPT_FLAG_DEFER_SAFE) != 0U &&
                laghu_javascript_rum_ready(rum, template_key, page_origin, url,
                                           viewport_bucket, now,
                                           defer_evidence_key) &&
                laghu_tag_attributes_allowed(cursor + 7U, open_end, false) &&
                !laghu_tag_attribute(cursor + 7U, open_end, "async", &attribute,
                                     &attribute_length) &&
                !laghu_tag_attribute(cursor + 7U, open_end, "defer", &attribute,
                                     &attribute_length) &&
                !laghu_tag_attribute(cursor + 7U, open_end, "nomodule",
                                     &attribute, &attribute_length) &&
                laghu_javascript_defer_suffix_ready(
                    cache_path, policy_key, normalized_target, page_path,
                    page_origin, close + 8U, end, now, ttl_seconds, rum,
                    template_key, viewport_bucket, defer_set);
            original_bundle += (size_t)record.derived_length;
            if (defer_ready &&
                !laghu_javascript_defer_approved(defer_set, url, page_path) &&
                allow_defer_suggestions &&
                !result->javascript_defer_recommended) {
              result->javascript_defer_recommended = true;
              (void)laghu_copy(result->javascript_defer_path,
                               sizeof(result->javascript_defer_path), url);
              (void)laghu_copy(result->javascript_defer_template,
                               sizeof(result->javascript_defer_template),
                               template_key);
              result->javascript_defer_bucket = viewport_bucket;
              {
                laghu_rum_instrumentation_record evidence;
                laghu_rum_value evidence_value;
                if (laghu_rum_engine_read(rum, LAGHU_RUM_RECORD_INSTRUMENTATION,
                                          template_key, now, &evidence,
                                          sizeof(evidence), &evidence_value))
                  result->javascript_defer_observations =
                      evidence.observations[viewport_bucket];
              }
            }
            if (defer_ready &&
                laghu_javascript_defer_approved(defer_set, url, page_path)) {
              size_t prefix = (size_t)(open_end - html.data);
              size_t suffix = html.length - prefix;
              unsigned char *next = malloc(html.length + 6U);
              if (next == NULL) {
                free(output);
                return false;
              }
              memcpy(next, html.data, prefix);
              memcpy(next + prefix, " defer", 6U);
              memcpy(next + prefix + 6U, open_end, suffix);
              free(output);
              output = next;
              output_length = html.length + 6U;
              html = (laghu_buffer){output, output_length};
              cursor = html.data + prefix + 6U;
              end = html.data + html.length;
              defer_growth += 6U;
              {
                char material[LAGHU_RUNTIME_KEY_SIZE * 2U + 2U];
                int material_length =
                    snprintf(material, sizeof(material), "%s\n%s",
                             defer_set->digest, defer_evidence_key);
                if (material_length <= 0 ||
                    (size_t)material_length >= sizeof(material) ||
                    !laghu_sha256_hex(
                        (laghu_buffer){(const unsigned char *)material,
                                       (size_t)material_length},
                        dependencies[dependency_count++])) {
                  free(output);
                  return false;
                }
              }
              continue;
            }
            if (allow_inline && inline_limit != 0U &&
                record.derived_length <= inline_limit &&
                laghu_tag_attributes_allowed(cursor + 7U, open_end, false) &&
                !laghu_tag_attribute(cursor + 7U, open_end, "async", &attribute,
                                     &attribute_length) &&
                !laghu_tag_attribute(cursor + 7U, open_end, "defer", &attribute,
                                     &attribute_length) &&
                !laghu_tag_attribute(cursor + 7U, open_end, "nomodule",
                                     &attribute, &attribute_length) &&
                (record.flags & LAGHU_JAVASCRIPT_FLAG_URL_INDEPENDENT) != 0U &&
                laghu_javascript_csp_allows_inline(content_security_policy,
                                                   cursor + 7U, open_end) &&
                laghu_runtime_cache_lookup_variant(cache_path, record.variant,
                                                   &entry) &&
                entry.length == record.derived_length) {
              const unsigned char *span_start;
              const unsigned char *span_end;
              unsigned char *body = malloc(entry.length);
              if (body != NULL &&
                  laghu_runtime_cache_read(&entry, body, entry.length) &&
                  laghu_tag_attribute_span(cursor + 7U, open_end,
                                           source_attribute, source_length,
                                           &span_start, &span_end)) {
                size_t prefix = (size_t)(span_start - html.data);
                size_t middle = (size_t)(open_end + 1U - span_end);
                size_t suffix = html.length - (size_t)(close - html.data);
                unsigned char *next =
                    malloc(prefix + middle + entry.length + suffix);
                if (next == NULL) {
                  free(body);
                  free(output);
                  return false;
                }
                memcpy(next, html.data, prefix);
                memcpy(next + prefix, span_end, middle);
                memcpy(next + prefix + middle, body, entry.length);
                memcpy(next + prefix + middle + entry.length, close, suffix);
                free(body);
                free(output);
                output = next;
                output_length = prefix + middle + entry.length + suffix;
                html = (laghu_buffer){output, output_length};
                cursor = html.data + prefix + middle + entry.length;
                end = html.data + html.length;
                memcpy(dependencies[dependency_count++], record.variant,
                       LAGHU_RUNTIME_KEY_SIZE);
                continue;
              }
              free(body);
            }
            if (laghu_runtime_cache_lookup_variant(cache_path, record.variant,
                                                   &entry) &&
                entry.length == record.derived_length &&
                (route_length = snprintf(route, sizeof(route), "/.laghu/js/%s",
                                         record.variant)) > 0 &&
                (size_t)route_length < sizeof(route)) {
              size_t prefix = (size_t)(source_attribute - html.data);
              size_t suffix = html.length - prefix - source_length;
              unsigned char *next =
                  malloc(prefix + (size_t)route_length + suffix);
              if (next == NULL) {
                free(output);
                return false;
              }
              memcpy(next, html.data, prefix);
              memcpy(next + prefix, route, (size_t)route_length);
              memcpy(next + prefix + (size_t)route_length,
                     source_attribute + source_length, suffix);
              free(output);
              output = next;
              output_length = prefix + (size_t)route_length + suffix;
              html = (laghu_buffer){output, output_length};
              cursor = html.data + prefix + (size_t)route_length;
              end = html.data + html.length;
              rewritten_external += (size_t)record.derived_length;
              memcpy(dependencies[dependency_count++], record.variant,
                     LAGHU_RUNTIME_KEY_SIZE);
              continue;
            }
          }
        }
        cursor = close + 8U;
        continue;
      }
    }
    location_length = snprintf(location, sizeof(location), "%s#script-%zu",
                               page_path, script_count);
    if (accepted && location_length > 0 &&
        (size_t)location_length < sizeof(location) &&
        laghu_runtime_rewrite_javascript(
            queue, cache_path,
            (laghu_buffer){open_end + 1U, (size_t)(close - open_end - 1U)},
            location, policy_key, normalized_target, module,
            include_source_maps, &javascript)) {
      if (javascript.rewritten) {
        laghu_javascript_catalog_record inline_record;
        size_t source_length = (size_t)(close - open_end - 1U);
        bool hash_policy =
            content_security_policy != NULL &&
            (strstr(content_security_policy, "sha256-") != NULL ||
             strstr(content_security_policy, "sha384-") != NULL ||
             strstr(content_security_policy, "sha512-") != NULL);
        if (allow_outline && source_length >= outline_threshold &&
            !hash_policy &&
            laghu_tag_attributes_allowed(cursor + 7U, open_end, false) &&
            laghu_javascript_csp_allows_external(content_security_policy,
                                                 cursor + 7U, open_end) &&
            laghu_javascript_external_lookup(cache_path, location, policy_key,
                                             normalized_target, module, now,
                                             ttl_seconds, &inline_record) &&
            (inline_record.flags & LAGHU_JAVASCRIPT_FLAG_URL_INDEPENDENT) !=
                0U &&
            laghu_runtime_cache_lookup_variant(cache_path,
                                               inline_record.variant, &entry) &&
            entry.length == inline_record.derived_length) {
          char source_attribute[LAGHU_RUNTIME_KEY_SIZE + 32U];
          int source_attribute_length =
              snprintf(source_attribute, sizeof(source_attribute),
                       " src=\"/.laghu/js/%s\"", inline_record.variant);
          size_t prefix = (size_t)(open_end - html.data);
          size_t suffix = html.length - (size_t)(close - html.data);
          if (source_attribute_length > 0 &&
              (size_t)source_attribute_length < sizeof(source_attribute)) {
            unsigned char *next =
                malloc(prefix + (size_t)source_attribute_length + suffix);
            if (next == NULL) {
              laghu_runtime_javascript_result_release(&javascript);
              free(output);
              return false;
            }
            memcpy(next, html.data, prefix);
            memcpy(next + prefix, source_attribute,
                   (size_t)source_attribute_length);
            memcpy(next + prefix + (size_t)source_attribute_length, close,
                   suffix);
            free(output);
            output = next;
            output_length = prefix + (size_t)source_attribute_length + suffix;
            rewritten_external += javascript.length;
            memcpy(dependencies[dependency_count++], inline_record.variant,
                   LAGHU_RUNTIME_KEY_SIZE);
            html = (laghu_buffer){output, output_length};
            cursor = html.data + prefix + (size_t)source_attribute_length;
            end = html.data + html.length;
            laghu_runtime_javascript_result_release(&javascript);
            continue;
          }
        }
        size_t prefix = (size_t)(open_end + 1U - html.data);
        size_t suffix = html.length - (size_t)(close - html.data);
        unsigned char *next = malloc(prefix + javascript.length + suffix);
        if (next == NULL) {
          laghu_runtime_javascript_result_release(&javascript);
          free(output);
          return false;
        }
        memcpy(next, html.data, prefix);
        memcpy(next + prefix, javascript.data, javascript.length);
        memcpy(next + prefix + javascript.length, close, suffix);
        free(output);
        output = next;
        output_length = prefix + javascript.length + suffix;
        memcpy(dependencies[dependency_count++], javascript.dependency_key,
               LAGHU_RUNTIME_KEY_SIZE);
        /* Re-scan the rebuilt document so offsets never refer to stale data. */
        html = (laghu_buffer){output, output_length};
        cursor = html.data + prefix + javascript.length;
        end = html.data + html.length;
      } else {
        result->dependencies_pending |= javascript.published;
        cursor = close + 8U;
      }
      laghu_runtime_javascript_result_release(&javascript);
    } else {
      cursor = close + 8U;
    }
  }
  if (output != NULL) {
    if (output_length - defer_growth + rewritten_external >= original_bundle) {
      free(output);
      return true;
    }
    result->data = output;
    result->length = output_length;
    result->rewritten = true;
    if (dependency_count != 0U) {
      laghu_buffer material = {(const unsigned char *)dependencies,
                               dependency_count * LAGHU_RUNTIME_KEY_SIZE};
      (void)laghu_sha256_hex(material, result->dependency_key);
    }
  }
  return true;
}
