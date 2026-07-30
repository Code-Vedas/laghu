// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/runtime.h"

#define LAGHU_CSS_MARKUP_MAX_ITEMS 64U

typedef struct {
  unsigned char *data;
  size_t length;
  size_t capacity;
} laghu_css_markup_builder;

static bool laghu_css_markup_append(laghu_css_markup_builder *builder,
                                    const void *data, size_t length) {
  size_t needed;
  unsigned char *grown;
  if (length > SIZE_MAX - builder->length - 1U) {
    return false;
  }
  needed = builder->length + length + 1U;
  if (needed > builder->capacity) {
    size_t capacity = builder->capacity == 0U ? 1024U : builder->capacity;
    while (capacity < needed) {
      capacity = capacity > SIZE_MAX / 2U ? needed : capacity * 2U;
    }
    grown = realloc(builder->data, capacity);
    if (grown == NULL) {
      return false;
    }
    builder->data = grown;
    builder->capacity = capacity;
  }
  memcpy(builder->data + builder->length, data, length);
  builder->length += length;
  builder->data[builder->length] = '\0';
  return true;
}

static bool laghu_css_markup_equal(const unsigned char *value, size_t length,
                                   const char *expected) {
  size_t index;
  if (strlen(expected) != length) {
    return false;
  }
  for (index = 0U; index < length; ++index) {
    if (tolower(value[index]) != tolower((unsigned char)expected[index])) {
      return false;
    }
  }
  return true;
}

static const unsigned char *laghu_css_markup_find(const unsigned char *data,
                                                  size_t length,
                                                  const char *needle) {
  size_t index;
  size_t needle_length = strlen(needle);
  for (index = 0U; index + needle_length <= length; ++index) {
    if (laghu_css_markup_equal(data + index, needle_length, needle)) {
      return data + index;
    }
  }
  return NULL;
}

static bool laghu_css_markup_attribute(const unsigned char *tag, size_t length,
                                       const char *name,
                                       const unsigned char **value,
                                       size_t *value_length) {
  size_t cursor = 1U;
  while (cursor < length && !isspace(tag[cursor]) && tag[cursor] != '>') {
    ++cursor;
  }
  while (cursor < length) {
    size_t attribute_start;
    size_t attribute_end;
    size_t start;
    unsigned char quote = 0U;
    while (cursor < length && (isspace(tag[cursor]) || tag[cursor] == '/')) {
      ++cursor;
    }
    attribute_start = cursor;
    while (cursor < length &&
           (isalnum(tag[cursor]) || tag[cursor] == '-' || tag[cursor] == '_')) {
      ++cursor;
    }
    attribute_end = cursor;
    if (attribute_end == attribute_start) {
      break;
    }
    while (cursor < length && isspace(tag[cursor])) {
      ++cursor;
    }
    if (cursor >= length || tag[cursor] != '=') {
      if (laghu_css_markup_equal(tag + attribute_start,
                                 attribute_end - attribute_start, name)) {
        *value = NULL;
        *value_length = 0U;
        return true;
      }
      continue;
    }
    ++cursor;
    while (cursor < length && isspace(tag[cursor])) {
      ++cursor;
    }
    if (cursor < length && (tag[cursor] == '\'' || tag[cursor] == '"')) {
      quote = tag[cursor++];
    }
    start = cursor;
    while (cursor < length &&
           ((quote != 0U && tag[cursor] != quote) ||
            (quote == 0U && !isspace(tag[cursor]) && tag[cursor] != '>'))) {
      ++cursor;
    }
    if (laghu_css_markup_equal(tag + attribute_start,
                               attribute_end - attribute_start, name)) {
      *value = tag + start;
      *value_length = cursor - start;
      return true;
    }
    if (quote != 0U && cursor < length) {
      ++cursor;
    }
  }
  return false;
}

static bool laghu_css_markup_has(const unsigned char *tag, size_t length,
                                 const char *name) {
  const unsigned char *value;
  size_t value_length;
  return laghu_css_markup_attribute(tag, length, name, &value, &value_length);
}

static bool laghu_css_markup_safe_attribute(const unsigned char *value,
                                            size_t length) {
  size_t index;
  for (index = 0U; index < length; ++index) {
    if (value[index] == '"' || value[index] == '<' || value[index] == '>') {
      return false;
    }
  }
  return true;
}

static bool laghu_css_markup_normalize(const unsigned char *url, size_t length,
                                       const char *page_path,
                                       const char *page_origin,
                                       char output[LAGHU_RUNTIME_PATH_SIZE]) {
  const char *slash;
  size_t prefix = 0U;
  size_t origin_length = page_origin == NULL ? 0U : strlen(page_origin);
  if (length == 0U || length >= LAGHU_RUNTIME_PATH_SIZE ||
      (length >= 2U && url[0] == '/' && url[1] == '/')) {
    return false;
  }
  if (origin_length != 0U && length > origin_length &&
      memcmp(url, page_origin, origin_length) == 0 &&
      url[origin_length] == '/') {
    url += origin_length;
    length -= origin_length;
  } else if (memchr(url, ':', length) != NULL) {
    return false;
  }
  if (url[0] != '/') {
    slash = strrchr(page_path != NULL ? page_path : "/", '/');
    prefix = slash == NULL ? 1U : (size_t)(slash - page_path + 1U);
    if (prefix + length >= LAGHU_RUNTIME_PATH_SIZE) {
      return false;
    }
    memcpy(output, page_path, prefix);
  }
  memcpy(output + prefix, url, length);
  output[prefix + length] = '\0';
  return strstr(output, "..") == NULL &&
         !(strncmp(output, "/api", 4U) == 0 &&
           (output[4] == '\0' || output[4] == '/')) &&
         !(strncmp(output, "/graphql", 8U) == 0 &&
           (output[8] == '\0' || output[8] == '/'));
}

static bool laghu_css_markup_read(const char *cache_path, const char *key,
                                  unsigned char **data, size_t *length) {
  laghu_runtime_cache_entry entry;
  if (!laghu_runtime_cache_lookup_variant(cache_path, key, &entry)) {
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

bool laghu_runtime_rewrite_css_markup(
    const char *cache_path, laghu_buffer html, const char *page_path,
    const char *page_origin, const char *policy_key, uint32_t capability_mask,
    uint64_t now, unsigned int ttl_seconds, bool allow_inline,
    bool allow_outline, bool allow_combine, laghu_html_planner_mask html_plan,
    bool csp_allows_inline_styles, bool csp_allows_self_styles,
    unsigned int inline_limit, unsigned int outline_threshold,
    laghu_runtime_html_result *result) {
  laghu_buffer source_html = html;
  laghu_css_markup_builder builder = {0};
  size_t cursor = 0U;
  size_t original_bundle = html.length;
  size_t outlined_payload = 0U;
  size_t rewritten_bundle;
  unsigned int item_count = 0U;
  bool changed = false;
  bool structural_changed = false;
  bool lexical_changed = false;
  laghu_runtime_head_result head = {0};
  if (result == NULL || cache_path == NULL || page_path == NULL ||
      policy_key == NULL || html.length > LAGHU_IMAGE_MAX_INPUT_BYTES) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  if (html_plan != 0U &&
      !laghu_runtime_plan_html_document(html, html_plan, &head)) {
    return false;
  }
  if (head.rewritten) {
    html = (laghu_buffer){head.data, head.length};
    structural_changed = head.structural_changed;
    lexical_changed = head.lexical_changed;
  }
  while (cursor < html.length) {
    const unsigned char *open =
        memchr(html.data + cursor, '<', html.length - cursor);
    size_t start;
    size_t end;
    if (open == NULL) {
      if (!laghu_css_markup_append(&builder, html.data + cursor,
                                   html.length - cursor)) {
        goto failed;
      }
      break;
    }
    start = (size_t)(open - html.data);
    if (!laghu_css_markup_append(&builder, html.data + cursor,
                                 start - cursor)) {
      goto failed;
    }
    end = start + 1U;
    while (end < html.length && html.data[end] != '>') {
      ++end;
    }
    if (end == html.length || ++item_count > LAGHU_CSS_MARKUP_MAX_ITEMS) {
      goto unchanged;
    }
    if (allow_inline && csp_allows_inline_styles && start + 5U <= end &&
        laghu_css_markup_equal(html.data + start + 1U, 4U, "link")) {
      const unsigned char *rel = NULL;
      const unsigned char *href = NULL;
      const unsigned char *media = NULL;
      const unsigned char *type = NULL;
      size_t rel_length = 0U;
      size_t href_length = 0U;
      size_t media_length = 0U;
      size_t type_length = 0U;
      char normalized[LAGHU_RUNTIME_PATH_SIZE];
      laghu_stylesheet_record record;
      unsigned char *css = NULL;
      size_t css_length = 0U;
      bool eligible =
          laghu_css_markup_attribute(html.data + start, end - start + 1U, "rel",
                                     &rel, &rel_length) &&
          laghu_css_markup_equal(rel, rel_length, "stylesheet") &&
          laghu_css_markup_attribute(html.data + start, end - start + 1U,
                                     "href", &href, &href_length) &&
          !laghu_css_markup_has(html.data + start, end - start + 1U,
                                "integrity") &&
          !laghu_css_markup_has(html.data + start, end - start + 1U, "nonce") &&
          !laghu_css_markup_has(html.data + start, end - start + 1U,
                                "disabled") &&
          !laghu_css_markup_has(html.data + start, end - start + 1U,
                                "alternate");
      if (eligible &&
          laghu_css_markup_attribute(html.data + start, end - start + 1U,
                                     "media", &media, &media_length)) {
        eligible = laghu_css_markup_equal(media, media_length, "all");
      }
      if (eligible &&
          laghu_css_markup_attribute(html.data + start, end - start + 1U,
                                     "type", &type, &type_length)) {
        eligible = laghu_css_markup_equal(type, type_length, "text/css");
      }
      if (eligible && inline_limit != 0U &&
          laghu_css_markup_normalize(href, href_length, page_path, page_origin,
                                     normalized)) {
        if (!laghu_stylesheet_lookup(
                cache_path, normalized, policy_key, capability_mask,
                inline_limit, outline_threshold, now, ttl_seconds, &record)) {
          result->dependencies_pending = true;
          goto unchanged;
        }
        if ((record.ready || record.terminally_excluded) &&
            record.source_length <= inline_limit &&
            laghu_css_markup_read(
                cache_path,
                record.ready ? record.derived_key : record.source_key, &css,
                &css_length) &&
            strstr((const char *)css, "@import") == NULL &&
            strstr((const char *)css, "@font-face") == NULL) {
          if (!laghu_css_markup_append(&builder, "<style>", 7U) ||
              !laghu_css_markup_append(&builder, css, css_length) ||
              !laghu_css_markup_append(&builder, "</style>", 8U)) {
            free(css);
            goto failed;
          }
          original_bundle += css_length;
          free(css);
          cursor = end + 1U;
          changed = true;
          continue;
        }
        free(css);
      }
    }
    if (allow_outline && csp_allows_self_styles && start + 6U <= end &&
        laghu_css_markup_equal(html.data + start + 1U, 5U, "style") &&
        !laghu_css_markup_has(html.data + start, end - start + 1U, "scoped")) {
      const unsigned char *block_media = NULL;
      const unsigned char *type = NULL;
      size_t block_media_length = 0U;
      size_t type_length = 0U;
      bool has_block_media = laghu_css_markup_attribute(
          html.data + start, end - start + 1U, "media", &block_media,
          &block_media_length);
      bool has_type = laghu_css_markup_attribute(
          html.data + start, end - start + 1U, "type", &type, &type_length);
      const unsigned char *close = laghu_css_markup_find(
          html.data + end + 1U, html.length - end - 1U, "</style>");
      size_t css_length =
          close == NULL ? 0U : (size_t)(close - html.data) - end - 1U;
      laghu_css_parse_result *imports = calloc(1U, sizeof(*imports));
      bool eligible =
          imports != NULL && close != NULL &&
          (!has_block_media ||
           laghu_css_markup_equal(block_media, block_media_length, "all")) &&
          (!has_type || laghu_css_markup_equal(type, type_length, "text/css"));
      if (eligible &&
          !laghu_css_discover((laghu_buffer){html.data + end + 1U, css_length},
                              page_path, page_origin, imports)) {
        eligible = false;
      }
      if (eligible && imports->has_imports && imports->imports_supported &&
          !imports->import_graph_forbidden) {
        laghu_css_markup_builder links = {0};
        laghu_css_markup_builder remainder = {0};
        size_t import_index;
        size_t css_cursor = 0U;
        size_t imported_bytes = 0U;
        for (import_index = 0U; import_index < imports->import_count;
             ++import_index) {
          laghu_stylesheet_record record;
          char link[1024U];
          int link_length;
          const laghu_css_import *import = &imports->imports[import_index];
          if (!laghu_stylesheet_lookup(
                  cache_path, import->source_url, policy_key, capability_mask,
                  inline_limit, outline_threshold, now, ttl_seconds, &record)) {
            result->dependencies_pending = true;
            eligible = false;
            break;
          }
          if (!record.ready && !record.terminally_excluded) {
            result->dependencies_pending = true;
            eligible = false;
            break;
          }
          if (!record.ready || record.terminally_excluded ||
              record.derived_key[0] == '\0' ||
              record.derived_length > SIZE_MAX - imported_bytes) {
            eligible = false;
            break;
          }
          imported_bytes += record.derived_length;
          link_length = snprintf(
              link, sizeof(link),
              "<link rel=\"stylesheet\" href=\"/.laghu/css/%s\"%s%s%s>",
              record.derived_key, import->media[0] != '\0' ? " media=\"" : "",
              import->media, import->media[0] != '\0' ? "\"" : "");
          if (link_length <= 0 || (size_t)link_length >= sizeof(link) ||
              !laghu_css_markup_append(&links, link, (size_t)link_length) ||
              !laghu_css_markup_append(&remainder,
                                       html.data + end + 1U + css_cursor,
                                       import->start - css_cursor)) {
            free(links.data);
            free(remainder.data);
            free(imports);
            goto failed;
          }
          css_cursor = import->end;
        }
        if (eligible && !laghu_css_markup_append(
                            &remainder, html.data + end + 1U + css_cursor,
                            css_length - css_cursor)) {
          free(links.data);
          free(remainder.data);
          free(imports);
          goto failed;
        }
        if (eligible &&
            (!laghu_css_markup_append(&builder, links.data, links.length) ||
             (remainder.length != 0U &&
              (!laghu_css_markup_append(&builder, html.data + start,
                                        end - start + 1U) ||
               !laghu_css_markup_append(&builder, remainder.data,
                                        remainder.length) ||
               !laghu_css_markup_append(&builder, "</style>", 8U))))) {
          free(links.data);
          free(remainder.data);
          free(imports);
          goto failed;
        }
        if (eligible) {
          original_bundle += imported_bytes;
          outlined_payload += imported_bytes;
          cursor = (size_t)(close - html.data) + 8U;
          changed = true;
          free(links.data);
          free(remainder.data);
          free(imports);
          continue;
        }
        free(links.data);
        free(remainder.data);
      }
      free(imports);
      if (result->dependencies_pending) {
        goto unchanged;
      }
    }
    if (allow_outline && start + 6U <= end &&
        laghu_css_markup_equal(html.data + start + 1U, 5U, "style") &&
        !laghu_css_markup_has(html.data + start, end - start + 1U, "nonce") &&
        !laghu_css_markup_has(html.data + start, end - start + 1U, "scoped")) {
      const unsigned char *media = NULL;
      const unsigned char *type = NULL;
      size_t media_length = 0U;
      size_t type_length = 0U;
      bool has_media = laghu_css_markup_attribute(
          html.data + start, end - start + 1U, "media", &media, &media_length);
      bool has_type = laghu_css_markup_attribute(
          html.data + start, end - start + 1U, "type", &type, &type_length);
      const unsigned char *close = laghu_css_markup_find(
          html.data + end + 1U, html.length - end - 1U, "</style>");
      size_t css_length =
          close == NULL ? 0U : (size_t)(close - html.data) - end - 1U;
      if ((!has_media ||
           laghu_css_markup_safe_attribute(media, media_length)) &&
          (!has_type ||
           laghu_css_markup_equal(type, type_length, "text/css")) &&
          close != NULL && css_length >= outline_threshold &&
          laghu_css_markup_find(html.data + end + 1U, css_length, "@import") ==
              NULL &&
          laghu_css_markup_find(html.data + end + 1U, css_length,
                                "@font-face") == NULL) {
        laghu_runtime_css_result css_result;
        char source_hash[LAGHU_RUNTIME_KEY_SIZE];
        char synthetic_path[LAGHU_RUNTIME_PATH_SIZE];
        const char *slash = strrchr(page_path, '/');
        size_t directory_length =
            slash == NULL ? 1U : (size_t)(slash - page_path + 1U);
        if (!laghu_sha256_hex((laghu_buffer){html.data + end + 1U, css_length},
                              source_hash) ||
            directory_length + sizeof(".laghu-style-.css") - 1U +
                    LAGHU_SHA256_HEX_LENGTH >=
                sizeof(synthetic_path)) {
          goto unchanged;
        }
        memcpy(synthetic_path, page_path, directory_length);
        (void)snprintf(synthetic_path + directory_length,
                       sizeof(synthetic_path) - directory_length,
                       ".laghu-style-%s.css", source_hash);
        if (!laghu_runtime_rewrite_css(
                NULL, cache_path,
                (laghu_buffer){html.data + end + 1U, css_length},
                synthetic_path, page_origin, policy_key, capability_mask, now,
                ttl_seconds, true, false, inline_limit, outline_threshold,
                &css_result)) {
          goto unchanged;
        }
        if (css_result.published || css_result.dependencies_pending) {
          laghu_runtime_css_result_release(&css_result);
          result->dependencies_pending = true;
          goto unchanged;
        }
        if (css_result.rewritten && css_result.length < css_length) {
          char link[512U];
          int length = snprintf(
              link, sizeof(link),
              "<link rel=\"stylesheet\" href=\"/.laghu/css/%s\"%s%.*s%s%s>",
              css_result.dependency_key, has_media ? " media=\"" : "",
              has_media ? (int)media_length : 0,
              has_media ? (const char *)media : "", has_media ? "\"" : "",
              has_type ? " type=\"text/css\"" : "");
          if (length <= 0 || (size_t)length >= sizeof(link) ||
              !laghu_css_markup_append(&builder, link, (size_t)length)) {
            laghu_runtime_css_result_release(&css_result);
            goto failed;
          }
          outlined_payload += css_result.length;
          laghu_runtime_css_result_release(&css_result);
          cursor = (size_t)(close - html.data) + 8U;
          changed = true;
          continue;
        }
        laghu_runtime_css_result_release(&css_result);
      }
    }
    if (!laghu_css_markup_append(&builder, html.data + start,
                                 end - start + 1U)) {
      goto failed;
    }
    cursor = end + 1U;
  }
  if (allow_combine && csp_allows_self_styles) {
    laghu_runtime_css_combine_result combined;
    if (!laghu_runtime_combine_css_markup(
            cache_path, (laghu_buffer){builder.data, builder.length}, page_path,
            page_origin, policy_key, capability_mask, now, ttl_seconds,
            inline_limit, outline_threshold, &combined)) {
      goto failed;
    }
    if (combined.dependencies_pending) {
      result->dependencies_pending = true;
      laghu_runtime_css_combine_result_release(&combined);
      goto unchanged;
    }
    if (combined.rewritten) {
      free(builder.data);
      builder.data = combined.data;
      builder.length = combined.length;
      builder.capacity = combined.length + 1U;
      original_bundle += combined.original_external_bytes;
      outlined_payload += combined.combined_external_bytes;
      changed = true;
    }
  }
  rewritten_bundle = builder.length + outlined_payload;
  if ((!changed && !structural_changed && !lexical_changed) ||
      rewritten_bundle > original_bundle ||
      (!structural_changed && rewritten_bundle == original_bundle)) {
    goto unchanged;
  }
  {
    char source_hash[LAGHU_RUNTIME_KEY_SIZE];
    char output_hash[LAGHU_RUNTIME_KEY_SIZE];
    char material[LAGHU_RUNTIME_KEY_SIZE * 3U + 128U];
    int material_length;
    if (!laghu_sha256_hex(source_html, source_hash) ||
        !laghu_sha256_hex((laghu_buffer){builder.data, builder.length},
                          output_hash)) {
      goto failed;
    }
    material_length = snprintf(
        material, sizeof(material), "laghu-html-markup-v%u\n%s\n%s\n%s\n%08x",
        LAGHU_HTML_PLANNER_VERSION, source_hash, output_hash, policy_key,
        (unsigned int)html_plan);
    if (material_length <= 0 || (size_t)material_length >= sizeof(material) ||
        !laghu_sha256_hex((laghu_buffer){(const unsigned char *)material,
                                         (size_t)material_length},
                          result->dependency_key)) {
      goto failed;
    }
  }
  {
    laghu_runtime_cache_entry entry;
    if (!laghu_runtime_cache_lookup_variant(cache_path, result->dependency_key,
                                            &entry)) {
      if (!laghu_runtime_cache_publish(
              cache_path, result->dependency_key, result->dependency_key,
              result->dependency_key, "text/html", "laghu-css-markup-v3",
              (laghu_buffer){builder.data, builder.length}, &entry)) {
        goto failed;
      }
      free(builder.data);
      laghu_runtime_head_result_release(&head);
      result->dependencies_pending = true;
      return true;
    }
    result->data = malloc(entry.length + 1U);
    if (result->data == NULL ||
        !laghu_runtime_cache_read(&entry, result->data, entry.length)) {
      goto failed;
    }
    result->data[entry.length] = '\0';
    result->length = entry.length;
    result->rewritten = true;
    free(builder.data);
    laghu_runtime_head_result_release(&head);
    return true;
  }

unchanged:
  free(builder.data);
  laghu_runtime_head_result_release(&head);
  return true;
failed:
  free(builder.data);
  laghu_runtime_head_result_release(&head);
  free(result->data);
  memset(result, 0, sizeof(*result));
  return false;
}

bool laghu_runtime_rewrite_font_css(
    laghu_runtime_queue *fetch_queue, const char *cache_path,
    const laghu_font_provider_set *providers, laghu_buffer html, uint64_t now,
    bool allow_inline, bool csp_allows_inline_styles, unsigned int inline_limit,
    laghu_runtime_html_result *result) {
  laghu_css_markup_builder builder = {0};
  size_t cursor = 0U;
  size_t original_bundle = html.length;
  bool changed = false;
  if (result == NULL || cache_path == NULL || providers == NULL ||
      html.length > LAGHU_IMAGE_MAX_INPUT_BYTES) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  if (!allow_inline || !csp_allows_inline_styles || inline_limit == 0U) {
    return true;
  }
  while (cursor < html.length) {
    const unsigned char *open =
        memchr(html.data + cursor, '<', html.length - cursor);
    size_t start;
    size_t end;
    if (open == NULL) {
      if (!laghu_css_markup_append(&builder, html.data + cursor,
                                   html.length - cursor))
        goto failed;
      break;
    }
    start = (size_t)(open - html.data);
    if (!laghu_css_markup_append(&builder, html.data + cursor, start - cursor))
      goto failed;
    end = start + 1U;
    while (end < html.length && html.data[end] != '>') ++end;
    if (end == html.length) goto unchanged;
    if (start + 5U <= end &&
        laghu_css_markup_equal(html.data + start + 1U, 4U, "link")) {
      const unsigned char *rel = NULL, *href = NULL, *media = NULL;
      const unsigned char *type = NULL;
      size_t rel_length = 0U, href_length = 0U, media_length = 0U;
      size_t type_length = 0U;
      char url[LAGHU_RUNTIME_PATH_SIZE];
      const laghu_font_provider *provider = NULL;
      laghu_font_stylesheet_record record;
      bool has_media = false;
      bool eligible =
          laghu_css_markup_attribute(html.data + start, end - start + 1U, "rel",
                                     &rel, &rel_length) &&
          laghu_css_markup_equal(rel, rel_length, "stylesheet") &&
          laghu_css_markup_attribute(html.data + start, end - start + 1U,
                                     "href", &href, &href_length) &&
          href_length < sizeof(url) &&
          !laghu_css_markup_has(html.data + start, end - start + 1U,
                                "integrity") &&
          !laghu_css_markup_has(html.data + start, end - start + 1U, "nonce") &&
          !laghu_css_markup_has(html.data + start, end - start + 1U,
                                "disabled") &&
          !laghu_css_markup_has(html.data + start, end - start + 1U,
                                "alternate") &&
          !laghu_css_markup_has(html.data + start, end - start + 1U,
                                "onload") &&
          !laghu_css_markup_has(html.data + start, end - start + 1U, "onerror");
      if (eligible) {
        memcpy(url, href, href_length);
        url[href_length] = '\0';
        provider = laghu_font_provider_match(providers, url);
        eligible = provider != NULL;
      }
      if (eligible &&
          laghu_css_markup_attribute(html.data + start, end - start + 1U,
                                     "media", &media, &media_length)) {
        has_media = true;
        eligible = laghu_css_markup_safe_attribute(media, media_length);
      }
      if (eligible &&
          laghu_css_markup_attribute(html.data + start, end - start + 1U,
                                     "type", &type, &type_length))
        eligible = laghu_css_markup_equal(type, type_length, "text/css");
      if (eligible) {
        bool found = laghu_font_stylesheet_lookup(cache_path, url, provider,
                                                  now, &record);
        if (!found || (!record.ready && now >= record.retry_after)) {
          laghu_runtime_job job = {0};
          char key[LAGHU_RUNTIME_KEY_SIZE];
          if (fetch_queue != NULL &&
              laghu_font_stylesheet_key(url, provider->digest, key)) {
            job.kind = LAGHU_RUNTIME_JOB_FONT_CSS;
            memcpy(job.index_key, key, sizeof(job.index_key));
            memcpy(job.policy_key, key, sizeof(job.policy_key));
            (void)snprintf(job.request_path, sizeof(job.request_path), "%s",
                           url);
            (void)snprintf(job.content_type, sizeof(job.content_type),
                           "text/css");
            (void)snprintf(job.provider_id, sizeof(job.provider_id), "%s",
                           provider->id);
            memcpy(job.provider_digest, provider->digest,
                   sizeof(job.provider_digest));
            if (laghu_runtime_queue_try_publish(fetch_queue, &job)) {
              memset(&record, 0, sizeof(record));
              (void)snprintf(record.provider_id, sizeof(record.provider_id),
                             "%s", provider->id);
              memcpy(record.provider_digest, provider->digest,
                     sizeof(record.provider_digest));
              (void)snprintf(record.normalized_url,
                             sizeof(record.normalized_url), "%s", url);
              record.retry_after = now + 30U;
              record.ttl_seconds = provider->ttl_seconds;
              (void)laghu_font_stylesheet_publish(cache_path, &record);
            }
          }
          result->dependencies_pending = true;
          goto unchanged;
        }
        if (record.ready && record.css_length <= inline_limit) {
          laghu_runtime_cache_entry entry;
          unsigned char *css = NULL;
          if (!laghu_runtime_cache_lookup_variant(cache_path,
                                                  record.variant_key, &entry) ||
              entry.length != record.css_length ||
              (css = malloc(entry.length + 1U)) == NULL ||
              !laghu_runtime_cache_read(&entry, css, entry.length)) {
            free(css);
            result->dependencies_pending = true;
            goto unchanged;
          }
          css[entry.length] = '\0';
          if (!laghu_font_css_validate(provider,
                                       (laghu_buffer){css, entry.length}) ||
              !laghu_css_markup_append(&builder, "<style", 6U) ||
              (has_media &&
               (!laghu_css_markup_append(&builder, " media=\"", 8U) ||
                !laghu_css_markup_append(&builder, media, media_length) ||
                !laghu_css_markup_append(&builder, "\"", 1U))) ||
              !laghu_css_markup_append(&builder, ">", 1U) ||
              !laghu_css_markup_append(&builder, css, entry.length) ||
              !laghu_css_markup_append(&builder, "</style>", 8U)) {
            free(css);
            goto failed;
          }
          free(css);
          original_bundle += entry.length;
          cursor = end + 1U;
          changed = true;
          continue;
        }
      }
    }
    if (!laghu_css_markup_append(&builder, html.data + start, end - start + 1U))
      goto failed;
    cursor = end + 1U;
  }
  if (!changed || builder.length > original_bundle) goto unchanged;
  {
    char source_hash[LAGHU_RUNTIME_KEY_SIZE];
    char output_hash[LAGHU_RUNTIME_KEY_SIZE];
    char material[LAGHU_RUNTIME_KEY_SIZE * 3U + 64U];
    int length;
    if (!laghu_sha256_hex(html, source_hash) ||
        !laghu_sha256_hex((laghu_buffer){builder.data, builder.length},
                          output_hash))
      goto failed;
    length = snprintf(material, sizeof(material), "font-markup-v1\n%s\n%s\n%s",
                      source_hash, output_hash, providers->digest);
    if (length <= 0 || (size_t)length >= sizeof(material) ||
        !laghu_sha256_hex(
            (laghu_buffer){(const unsigned char *)material, (size_t)length},
            result->dependency_key))
      goto failed;
  }
  result->data = builder.data;
  result->length = builder.length;
  result->rewritten = true;
  return true;
unchanged:
  free(builder.data);
  return true;
failed:
  free(builder.data);
  memset(result, 0, sizeof(*result));
  return false;
}
