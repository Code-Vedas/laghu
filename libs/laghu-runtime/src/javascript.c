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
#define LAGHU_JAVASCRIPT_CATALOG_SIZE 1904U

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
                                 bool module,
                                 char output[LAGHU_RUNTIME_KEY_SIZE],
                                 char source_hash[LAGHU_RUNTIME_KEY_SIZE]) {
  char canonical[LAGHU_RUNTIME_PATH_SIZE + LAGHU_RUNTIME_KEY_SIZE +
                 LAGHU_JAVASCRIPT_TARGET_SIZE + 96U];
  int length;
  if (!laghu_sha256_hex(source, source_hash)) return false;
  length =
      snprintf(canonical, sizeof(canonical), "laghu-js-v%u\n%s\n%s\n%s\n%s\n%c",
               LAGHU_JAVASCRIPT_DERIVATION_VERSION, source_hash, path, policy,
               target, module ? 'm' : 'c');
  return length > 0 && (size_t)length < sizeof(canonical) &&
         laghu_sha256_hex(
             (laghu_buffer){(const unsigned char *)canonical, (size_t)length},
             output);
}

bool laghu_runtime_rewrite_javascript(
    laghu_runtime_queue *queue, const char *cache_path, laghu_buffer source,
    const char *normalized_path, const char *policy_key, const char *target,
    bool module, laghu_runtime_javascript_result *result) {
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
                            normalized_target, module, key, source_hash))
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
  job.filters = module ? UINT64_C(1) : UINT64_C(0);
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

static bool laghu_javascript_external_lookup(
    const char *cache_path, const char *url, const char *policy_key,
    const char *target, bool module, uint64_t now, unsigned int ttl_seconds,
    char variant[LAGHU_RUNTIME_KEY_SIZE]) {
  unsigned char catalog[LAGHU_JAVASCRIPT_CATALOG_SIZE];
  unsigned char checksum_bytes[1836U];
  char canonical[LAGHU_RUNTIME_PATH_SIZE + LAGHU_RUNTIME_KEY_SIZE +
                 LAGHU_JAVASCRIPT_TARGET_SIZE + 64U];
  char key[LAGHU_RUNTIME_KEY_SIZE], checksum[LAGHU_RUNTIME_KEY_SIZE];
  char path[LAGHU_RUNTIME_PATH_SIZE];
  FILE *file;
  uint64_t magic, updated, source_length, variant_length;
  uint32_t version, stored_module;
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
  memcpy(checksum_bytes, catalog, sizeof(checksum_bytes));
  if (!laghu_sha256_hex((laghu_buffer){checksum_bytes, sizeof(checksum_bytes)},
                        checksum) ||
      magic != LAGHU_JAVASCRIPT_CATALOG_MAGIC || version != 1U ||
      stored_module != (module ? 1U : 0U) || updated > now ||
      now - updated > ttl_seconds || source_length == 0U ||
      variant_length == 0U || variant_length >= source_length ||
      memchr(catalog + 40U, '\0', 1024U) == NULL ||
      memchr(catalog + 1064U, '\0', 65U) == NULL ||
      memchr(catalog + 1129U, '\0', 512U) == NULL ||
      memchr(catalog + 1836U, '\0', 65U) == NULL ||
      strcmp((const char *)catalog + 40U, url) != 0 ||
      strcmp((const char *)catalog + 1064U, policy_key) != 0 ||
      strcmp((const char *)catalog + 1129U, target) != 0 ||
      strcmp((const char *)catalog + 1836U, checksum) != 0 ||
      memchr(catalog + 1706U, '\0', LAGHU_RUNTIME_KEY_SIZE) == NULL ||
      !laghu_runtime_cache_lookup_variant(
          cache_path, (const char *)catalog + 1706U, &entry) ||
      entry.length != variant_length)
    return false;
  memcpy(variant, catalog + 1706U, LAGHU_RUNTIME_KEY_SIZE);
  return true;
}

bool laghu_runtime_rewrite_javascript_html(
    laghu_runtime_queue *queue, const char *cache_path, laghu_buffer html,
    const char *page_path, const char *policy_key, const char *target,
    const char *content_security_policy, uint64_t now, unsigned int ttl_seconds,
    laghu_runtime_html_result *result) {
  const unsigned char *cursor, *end;
  unsigned char *output = NULL;
  size_t output_length = 0U, script_count = 0U;
  size_t original_length = html.length;
  char normalized_target[LAGHU_JAVASCRIPT_TARGET_SIZE];
  char dependencies[LAGHU_JAVASCRIPT_MAX_SCRIPTS][LAGHU_RUNTIME_KEY_SIZE];
  size_t dependency_count = 0U;
  if (result == NULL) return false;
  memset(result, 0, sizeof(*result));
  if (queue == NULL || cache_path == NULL || html.data == NULL ||
      page_path == NULL || policy_key == NULL)
    return true;
  if (!laghu_javascript_target_normalize(target, normalized_target))
    return true;
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
        char variant[LAGHU_RUNTIME_KEY_SIZE];
        char route[LAGHU_RUNTIME_KEY_SIZE + 16U];
        int route_length;
        if (accepted && source_attribute != NULL && source_length > 0U &&
            source_length < sizeof(url) && source_attribute[0] == '/') {
          memcpy(url, source_attribute, source_length);
          url[source_length] = '\0';
          if (laghu_javascript_external_lookup(cache_path, url, policy_key,
                                               normalized_target, module, now,
                                               ttl_seconds, variant) &&
              (route_length = snprintf(route, sizeof(route), "/.laghu/js/%s",
                                       variant)) > 0 &&
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
            memcpy(dependencies[dependency_count++], variant,
                   LAGHU_RUNTIME_KEY_SIZE);
            continue;
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
            location, policy_key, normalized_target, module, &javascript)) {
      if (javascript.rewritten) {
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
    if (output_length >= original_length) {
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
