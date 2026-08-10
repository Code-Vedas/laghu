// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/lcp.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/csp.h"
#include "laghu/html.h"
#include "laghu/instrumentation.h"
#include "laghu/markup.h"
#include "laghu/rum.h"
#include "laghu/types.h"

#define LAGHU_LCP_KIND_IMAGE 1U
#define LAGHU_LCP_KIND_VIDEO 2U

typedef struct {
  size_t tag_start;
  size_t tag_end;
  char url[LAGHU_RUNTIME_PATH_SIZE];
  char srcset[768U];
  char sizes[256U];
  char resource_urls[LAGHU_LCP_MAX_RESOURCES][LAGHU_RUNTIME_PATH_SIZE];
  char resource_srcsets[LAGHU_LCP_MAX_RESOURCES][768U];
  char resource_sizes[LAGHU_LCP_MAX_RESOURCES][256U];
  unsigned int resource_count;
  unsigned int kind;
  unsigned int width;
  unsigned int height;
  bool excluded;
  bool picture;
  bool lazy;
  bool priority_low;
  bool priority_high;
} laghu_lcp_candidate;

typedef struct {
  laghu_lcp_candidate candidates[LAGHU_LCP_MAX_CANDIDATES];
  laghu_lcp_candidate picture_resources;
  unsigned int count;
  bool truncated;
} laghu_lcp_inventory;

static bool laghu_lcp_equal(const unsigned char *value, size_t length,
                            const char *expected);

static uint32_t laghu_lcp_tag_hash(const unsigned char *name, size_t length) {
  uint32_t hash = 2166136261U;
  size_t index;
  for (index = 0U; index < length; ++index) {
    hash ^= (uint32_t)tolower(name[index]);
    hash *= 16777619U;
  }
  return hash;
}

static bool laghu_lcp_void_tag(const unsigned char *name, size_t length) {
  return laghu_lcp_equal(name, length, "img") ||
         laghu_lcp_equal(name, length, "source") ||
         laghu_lcp_equal(name, length, "meta") ||
         laghu_lcp_equal(name, length, "link") ||
         laghu_lcp_equal(name, length, "input") ||
         laghu_lcp_equal(name, length, "br") ||
         laghu_lcp_equal(name, length, "hr");
}

static bool laghu_lcp_normalize(const char *page_path, const char *page_origin,
                                const unsigned char *value, size_t length,
                                char output[LAGHU_RUNTIME_PATH_SIZE]);

static bool laghu_lcp_add_resource(laghu_lcp_candidate *candidate,
                                   const char *url, const unsigned char *srcset,
                                   size_t srcset_length,
                                   const unsigned char *sizes,
                                   size_t sizes_length) {
  unsigned int index;
  if (url == NULL || url[0] == '\0') return false;
  for (index = 0U; index < candidate->resource_count; ++index)
    if (strcmp(candidate->resource_urls[index], url) == 0) return true;
  if (candidate->resource_count == LAGHU_LCP_MAX_RESOURCES ||
      srcset_length >= sizeof(candidate->resource_srcsets[0]) ||
      sizes_length >= sizeof(candidate->resource_sizes[0]))
    return false;
  index = candidate->resource_count++;
  strcpy(candidate->resource_urls[index], url);
  if (srcset != NULL && srcset_length != 0U) {
    memcpy(candidate->resource_srcsets[index], srcset, srcset_length);
    candidate->resource_srcsets[index][srcset_length] = '\0';
  }
  if (sizes != NULL && sizes_length != 0U) {
    memcpy(candidate->resource_sizes[index], sizes, sizes_length);
    candidate->resource_sizes[index][sizes_length] = '\0';
  }
  return true;
}

static bool laghu_lcp_add_srcset(laghu_lcp_candidate *candidate,
                                 const char *page_path, const char *page_origin,
                                 const unsigned char *srcset,
                                 size_t srcset_length,
                                 const unsigned char *sizes,
                                 size_t sizes_length) {
  laghu_buffer input = {srcset, srcset_length};
  laghu_srcset_candidate item;
  size_t cursor = 0U;
  while (laghu_srcset_next(input, &cursor, &item)) {
    char normalized[LAGHU_RUNTIME_PATH_SIZE];
    if (!laghu_lcp_normalize(page_path, page_origin, item.url.data,
                             item.url.length, normalized) ||
        !laghu_lcp_add_resource(candidate, normalized, srcset, srcset_length,
                                sizes, sizes_length))
      return false;
  }
  return true;
}

static bool laghu_lcp_equal(const unsigned char *value, size_t length,
                            const char *expected) {
  return laghu_base_ascii_equal((laghu_buffer){value, length}, expected);
}

static bool laghu_lcp_attr(const unsigned char *tag, size_t length,
                           const char *name, const unsigned char **value,
                           size_t *value_length, size_t *attribute_start,
                           size_t *attribute_end) {
  laghu_html_tag parsed = {.source = {tag, length}};
  laghu_html_attribute attribute;
  if (!laghu_html_tag_attribute(&parsed, name, &attribute) ||
      !attribute.has_value)
    return false;
  if (value != NULL) *value = attribute.value.data;
  if (value_length != NULL) *value_length = attribute.value.length;
  if (attribute_start != NULL) *attribute_start = attribute.start;
  if (attribute_end != NULL) *attribute_end = attribute.end;
  return true;
}

static bool laghu_lcp_normalize(const char *page_path, const char *page_origin,
                                const unsigned char *value, size_t length,
                                char output[LAGHU_RUNTIME_PATH_SIZE]) {
  return laghu_base_url_resolve_same_origin(
      page_path, page_origin, (laghu_buffer){value, length},
      LAGHU_URL_REJECT_TRAVERSAL | LAGHU_URL_REJECT_UNSAFE_BYTES, output,
      LAGHU_RUNTIME_PATH_SIZE);
}

static unsigned int laghu_lcp_uint(const unsigned char *value, size_t length) {
  uint64_t parsed = 0U;
  size_t index;
  if (value == NULL || length == 0U) return 0U;
  for (index = 0U; index < length; ++index) {
    if (!isdigit(value[index]) || parsed > UINT_MAX / 10U) return 0U;
    parsed = parsed * 10U + (unsigned int)(value[index] - '0');
    if (parsed > UINT_MAX) return 0U;
  }
  return (unsigned int)parsed;
}

static bool laghu_lcp_flag(const unsigned char *tag, size_t length,
                           const char *name) {
  laghu_html_tag parsed = {.source = {tag, length}};
  return laghu_html_tag_has_attribute(&parsed, name);
}

static bool laghu_lcp_hash(const char *origin, const char *url,
                           unsigned char output[LAGHU_SHA256_DIGEST_SIZE]) {
  char absolute[LAGHU_RUNTIME_PATH_SIZE * 2U];
  char hex[LAGHU_RUNTIME_KEY_SIZE];
  size_t index;
  int length;
  if (origin == NULL || url == NULL) return false;
  length = snprintf(absolute, sizeof(absolute), "%s%s", origin, url);
  if (length <= 0 || (size_t)length >= sizeof(absolute) ||
      !laghu_sha256_hex(
          (laghu_buffer){(const unsigned char *)absolute, (size_t)length}, hex))
    return false;
  for (index = 0U; index < LAGHU_SHA256_DIGEST_SIZE; ++index) {
    unsigned int high, low;
    unsigned char a = (unsigned char)hex[index * 2U];
    unsigned char b = (unsigned char)hex[index * 2U + 1U];
    high = (unsigned int)laghu_base_hex_value(a);
    low = (unsigned int)laghu_base_hex_value(b);
    output[index] = (unsigned char)((high << 4U) | low);
  }
  return true;
}

static bool laghu_lcp_scan(laghu_buffer html, const char *page_path,
                           const char *page_origin,
                           laghu_lcp_inventory *inventory) {
  size_t cursor = 0U;
  unsigned int excluded_depth = 0U, picture_depth = 0U, body_depth = 0U,
               tokens = 0U;
  uint32_t element_stack[128U];
  bool hidden_stack[128U];
  unsigned int element_depth = 0U, hidden_depth = 0U;
  laghu_lcp_candidate *picture_resources;
  if (inventory == NULL || html.data == NULL ||
      html.length > LAGHU_IMAGE_MAX_INPUT_BYTES)
    return false;
  memset(inventory, 0, sizeof(*inventory));
  picture_resources = &inventory->picture_resources;
  while (cursor < html.length && tokens++ < LAGHU_HTML_MAX_TOKENS) {
    const unsigned char *open =
        memchr(html.data + cursor, '<', html.length - cursor);
    const unsigned char *end, *name, *value;
    size_t name_length = 0U, value_length = 0U;
    bool closing;
    if (open == NULL) break;
    end = memchr(open, '>', html.length - (size_t)(open - html.data));
    if (end == NULL) return false;
    name = open + 1U;
    closing = name < end && *name == '/';
    if (closing) ++name;
    while (name + name_length < end &&
           (isalnum(name[name_length]) || name[name_length] == '-'))
      ++name_length;
    if (closing) {
      uint32_t closing_hash = laghu_lcp_tag_hash(name, name_length);
      while (element_depth > 0U) {
        --element_depth;
        if (hidden_stack[element_depth] && hidden_depth > 0U) --hidden_depth;
        if (element_stack[element_depth] == closing_hash) break;
      }
      if ((laghu_lcp_equal(name, name_length, "header") ||
           laghu_lcp_equal(name, name_length, "nav") ||
           laghu_lcp_equal(name, name_length, "aside") ||
           laghu_lcp_equal(name, name_length, "template")) &&
          excluded_depth > 0U)
        --excluded_depth;
      if (laghu_lcp_equal(name, name_length, "picture") && picture_depth > 0U)
        --picture_depth;
      if (laghu_lcp_equal(name, name_length, "body") && body_depth > 0U)
        --body_depth;
      cursor = (size_t)(end - html.data + 1U);
      continue;
    }
    if (laghu_lcp_equal(name, name_length, "header") ||
        laghu_lcp_equal(name, name_length, "nav") ||
        laghu_lcp_equal(name, name_length, "aside") ||
        laghu_lcp_equal(name, name_length, "template"))
      ++excluded_depth;
    if (laghu_lcp_equal(name, name_length, "picture")) {
      ++picture_depth;
      memset(picture_resources, 0, sizeof(*picture_resources));
    }
    if (laghu_lcp_equal(name, name_length, "body")) ++body_depth;
    if (picture_depth != 0U && laghu_lcp_equal(name, name_length, "source")) {
      const unsigned char *srcset_value = NULL, *sizes_value = NULL;
      size_t srcset_length = 0U, sizes_length = 0U;
      (void)laghu_lcp_attr(open, (size_t)(end - open + 1U), "sizes",
                           &sizes_value, &sizes_length, NULL, NULL);
      if (!laghu_lcp_attr(open, (size_t)(end - open + 1U), "srcset",
                          &srcset_value, &srcset_length, NULL, NULL) ||
          !laghu_lcp_add_srcset(picture_resources, page_path, page_origin,
                                srcset_value, srcset_length, sizes_value,
                                sizes_length))
        picture_resources->excluded = true;
    }
    if (laghu_lcp_equal(name, name_length, "img") ||
        laghu_lcp_equal(name, name_length, "video")) {
      laghu_lcp_candidate *candidate;
      const char *source_name =
          laghu_lcp_equal(name, name_length, "img") ? "src" : "poster";
      if (inventory->count == LAGHU_LCP_MAX_CANDIDATES) {
        inventory->truncated = true;
        return true;
      }
      candidate = &inventory->candidates[inventory->count++];
      if (picture_depth != 0U) *candidate = *picture_resources;
      candidate->kind = laghu_lcp_equal(name, name_length, "img")
                            ? LAGHU_LCP_KIND_IMAGE
                            : LAGHU_LCP_KIND_VIDEO;
      candidate->tag_start = (size_t)(open - html.data);
      candidate->tag_end = (size_t)(end - html.data + 1U);
      candidate->excluded =
          candidate->excluded || body_depth == 0U || excluded_depth != 0U ||
          hidden_depth != 0U ||
          laghu_lcp_flag(open, (size_t)(end - open + 1U), "hidden") ||
          laghu_lcp_flag(open, (size_t)(end - open + 1U), "inert");
      candidate->picture = picture_depth != 0U;
      if (!laghu_lcp_attr(open, (size_t)(end - open + 1U), source_name, &value,
                          &value_length, NULL, NULL) ||
          !laghu_lcp_normalize(page_path, page_origin, value, value_length,
                               candidate->url))
        candidate->excluded = true;
      else if (!laghu_lcp_add_resource(candidate, candidate->url, NULL, 0U,
                                       NULL, 0U))
        candidate->excluded = true;
      if (laghu_lcp_attr(open, (size_t)(end - open + 1U), "width", &value,
                         &value_length, NULL, NULL))
        candidate->width = laghu_lcp_uint(value, value_length);
      if (laghu_lcp_attr(open, (size_t)(end - open + 1U), "height", &value,
                         &value_length, NULL, NULL))
        candidate->height = laghu_lcp_uint(value, value_length);
      if (laghu_lcp_attr(open, (size_t)(end - open + 1U), "loading", &value,
                         &value_length, NULL, NULL))
        candidate->lazy = laghu_lcp_equal(value, value_length, "lazy");
      if (laghu_lcp_attr(open, (size_t)(end - open + 1U), "fetchpriority",
                         &value, &value_length, NULL, NULL)) {
        candidate->priority_low = laghu_lcp_equal(value, value_length, "low");
        candidate->priority_high = laghu_lcp_equal(value, value_length, "high");
      }
      if (candidate->kind == LAGHU_LCP_KIND_IMAGE &&
          laghu_lcp_attr(open, (size_t)(end - open + 1U), "sizes", &value,
                         &value_length, NULL, NULL) &&
          value_length < sizeof(candidate->sizes)) {
        memcpy(candidate->sizes, value, value_length);
        candidate->sizes[value_length] = '\0';
      }
      if (candidate->kind == LAGHU_LCP_KIND_IMAGE &&
          laghu_lcp_attr(open, (size_t)(end - open + 1U), "srcset", &value,
                         &value_length, NULL, NULL) &&
          value_length < sizeof(candidate->srcset)) {
        memcpy(candidate->srcset, value, value_length);
        candidate->srcset[value_length] = '\0';
        if (!laghu_lcp_add_srcset(candidate, page_path, page_origin, value,
                                  value_length,
                                  (const unsigned char *)candidate->sizes,
                                  strlen(candidate->sizes)))
          candidate->excluded = true;
      }
    }
    if (!laghu_lcp_void_tag(name, name_length) && end > open + 1U &&
        end[-1] != '/') {
      bool hidden = laghu_lcp_flag(open, (size_t)(end - open + 1U), "hidden") ||
                    laghu_lcp_flag(open, (size_t)(end - open + 1U), "inert");
      if (element_depth == sizeof(element_stack) / sizeof(element_stack[0]))
        return false;
      element_stack[element_depth] = laghu_lcp_tag_hash(name, name_length);
      hidden_stack[element_depth++] = hidden;
      if (hidden) ++hidden_depth;
    }
    cursor = (size_t)(end - html.data + 1U);
  }
  return tokens <= LAGHU_HTML_MAX_TOKENS;
}

bool laghu_lcp_inventory_record(laghu_buffer html, const char *page_path,
                                const char *page_origin,
                                laghu_rum_instrumentation_record *record,
                                char digest[LAGHU_RUNTIME_KEY_SIZE]) {
  laghu_lcp_inventory *inventory;
  unsigned char
      material[LAGHU_LCP_MAX_CANDIDATES *
               (2U + LAGHU_LCP_MAX_RESOURCES * LAGHU_SHA256_DIGEST_SIZE)];
  size_t used = 0U;
  unsigned int index;
  bool ok = false;
  if (record == NULL || digest == NULL) return false;
  inventory = calloc(1U, sizeof(*inventory));
  if (inventory == NULL) return false;
  if (!laghu_lcp_scan(html, page_path, page_origin, inventory) ||
      inventory->truncated)
    goto finished;
  record->media_count = inventory->count;
  for (index = 0U; index < inventory->count; ++index) {
    laghu_lcp_candidate *candidate = &inventory->candidates[index];
    unsigned int resource;
    record->media_kind[index] =
        candidate->excluded ? 0U : (unsigned char)candidate->kind;
    if (record->media_kind[index] != 0U &&
        !laghu_lcp_hash(page_origin, candidate->url, record->media_keys[index]))
      goto finished;
    record->media_resource_count[index] =
        record->media_kind[index] == 0U
            ? 0U
            : (unsigned char)candidate->resource_count;
    material[used++] = record->media_kind[index];
    material[used++] = record->media_resource_count[index];
    for (resource = 0U; resource < record->media_resource_count[index];
         ++resource) {
      if (!laghu_lcp_hash(page_origin, candidate->resource_urls[resource],
                          record->media_resource_keys[index][resource]))
        goto finished;
      memcpy(material + used, record->media_resource_keys[index][resource],
             LAGHU_SHA256_DIGEST_SIZE);
      used += LAGHU_SHA256_DIGEST_SIZE;
    }
  }
  ok = laghu_sha256_hex((laghu_buffer){material, used}, digest);
finished:
  free(inventory);
  return ok;
}

static bool laghu_lcp_ready(const laghu_rum_instrumentation_record *record,
                            unsigned int viewport, unsigned int *ordinal,
                            unsigned int *resource,
                            unsigned int *observations) {
  unsigned int schemes[2] = {viewport * 2U, viewport * 2U + 1U};
  unsigned int winner = UINT_MAX, resource_winner = UINT_MAX, scheme;
  *observations = 0U;
  for (scheme = 0U; scheme < 2U; ++scheme) {
    unsigned int bucket = schemes[scheme], candidate, best = UINT_MAX;
    unsigned int resource_index, best_resource = UINT_MAX;
    uint16_t best_count = 0U;
    uint16_t best_resource_count = 0U;
    uint16_t total = record->lcp_observations[bucket];
    if (total < LAGHU_LCP_QUORUM) return false;
    for (candidate = 0U; candidate < record->media_count; ++candidate)
      if (record->lcp_candidates[bucket][candidate] > best_count) {
        best = candidate;
        best_count = record->lcp_candidates[bucket][candidate];
      }
    if (best == UINT_MAX || (uint64_t)best_count * 100U <
                                (uint64_t)total * LAGHU_LCP_DOMINANCE_PERCENT)
      return false;
    if (winner != UINT_MAX && winner != best) return false;
    for (resource_index = 0U;
         resource_index < record->media_resource_count[best]; ++resource_index)
      if (record->lcp_resources[bucket][best][resource_index] >
          best_resource_count) {
        best_resource = resource_index;
        best_resource_count =
            record->lcp_resources[bucket][best][resource_index];
      }
    if (best_resource == UINT_MAX ||
        (uint64_t)best_resource_count * 100U <
            (uint64_t)total * LAGHU_LCP_DOMINANCE_PERCENT ||
        (resource_winner != UINT_MAX && resource_winner != best_resource))
      return false;
    winner = best;
    resource_winner = best_resource;
    *observations += total;
  }
  *ordinal = winner;
  *resource = resource_winner;
  return winner != UINT_MAX && resource_winner != UINT_MAX;
}

static bool laghu_lcp_bucket_ready(
    const laghu_rum_instrumentation_record *record, unsigned int bucket) {
  unsigned int candidate;
  uint16_t best = 0U;
  uint16_t total = record->lcp_observations[bucket];
  if (total < LAGHU_LCP_QUORUM) return false;
  for (candidate = 0U; candidate < record->media_count; ++candidate)
    if (record->lcp_candidates[bucket][candidate] > best)
      best = record->lcp_candidates[bucket][candidate];
  return (uint64_t)best * 100U >= (uint64_t)total * LAGHU_LCP_DOMINANCE_PERCENT;
}

static unsigned int laghu_lcp_heuristic(const laghu_lcp_inventory *inventory) {
  unsigned int index;
  for (index = 0U; index < inventory->count; ++index) {
    const laghu_lcp_candidate *candidate = &inventory->candidates[index];
    if (candidate->kind != LAGHU_LCP_KIND_IMAGE || candidate->excluded ||
        candidate->picture || candidate->lazy || candidate->priority_low ||
        candidate->resource_count != 1U || candidate->srcset[0] != '\0' ||
        (candidate->width != 0U && candidate->height != 0U &&
         (candidate->width < 120U || candidate->height < 80U)))
      continue;
    return index;
  }
  return UINT_MAX;
}

static bool laghu_lcp_safe_hint_value(const char *value) {
  size_t index;
  for (index = 0U; value[index] != '\0'; ++index)
    if ((unsigned char)value[index] < 0x20U || value[index] == '"' ||
        value[index] == '\\')
      return false;
  return true;
}

static bool laghu_lcp_rewrite_tag(laghu_buffer html,
                                  const laghu_lcp_candidate *candidate,
                                  bool add_priority, bool remove_lazy,
                                  unsigned char **output, size_t *length) {
  const unsigned char *tag = html.data + candidate->tag_start;
  size_t tag_length = candidate->tag_end - candidate->tag_start;
  size_t loading_start = 0U, loading_end = 0U;
  const unsigned char *loading_value;
  size_t loading_length;
  bool has_lazy =
      laghu_lcp_attr(tag, tag_length, "loading", &loading_value,
                     &loading_length, &loading_start, &loading_end) &&
      laghu_lcp_equal(loading_value, loading_length, "lazy");
  size_t removed = remove_lazy && has_lazy ? loading_end - loading_start : 0U;
  size_t added = add_priority && !candidate->priority_high
                     ? sizeof(" fetchpriority=\"high\"") - 1U
                     : 0U;
  size_t new_length;
  unsigned char *rewritten;
  size_t at = 0U;
  if (removed == 0U && added == 0U) return true;
  if (html.length < removed || html.length - removed > SIZE_MAX - added)
    return false;
  new_length = html.length - removed + added;
  rewritten = malloc(new_length + 1U);
  if (rewritten == NULL) return false;
  memcpy(rewritten, html.data, candidate->tag_start);
  at = candidate->tag_start;
  if (removed != 0U) {
    memcpy(rewritten + at, tag, loading_start);
    at += loading_start;
    memcpy(rewritten + at, tag + loading_end, tag_length - loading_end - 1U);
    at += tag_length - loading_end - 1U;
  } else {
    memcpy(rewritten + at, tag, tag_length - 1U);
    at += tag_length - 1U;
  }
  if (added != 0U) {
    memcpy(rewritten + at, " fetchpriority=\"high\"", added);
    at += added;
  }
  rewritten[at++] = '>';
  memcpy(rewritten + at, html.data + candidate->tag_end,
         html.length - candidate->tag_end);
  at += html.length - candidate->tag_end;
  rewritten[at] = '\0';
  *output = rewritten;
  *length = at;
  return true;
}

static bool laghu_runtime_prioritize_lcp_impl(
    laghu_rum_engine *rum, laghu_buffer evidence_html, laghu_buffer html,
    const char *page_path, const char *page_origin, const char *template_key,
    uint64_t now, unsigned int ttl_seconds, unsigned int viewport_width,
    bool resource_hints, bool lazyload, const laghu_csp_policy *csp,
    bool learned_only, laghu_lcp_result *result) {
  laghu_lcp_inventory *evidence = NULL, *current = NULL;
  laghu_rum_instrumentation_record *record = NULL;
  laghu_rum_instrumentation_record *evidence_record = NULL;
  laghu_rum_value value;
  unsigned int ordinal = UINT_MAX, resource = UINT_MAX, observations = 0U;
  bool learned = false;
  bool record_ready = false;
  if (result == NULL || page_path == NULL || page_origin == NULL ||
      html.data == NULL || evidence_html.data == NULL)
    return false;
  memset(result, 0, sizeof(*result));
  evidence = calloc(1U, sizeof(*evidence));
  current = calloc(1U, sizeof(*current));
  record = calloc(1U, sizeof(*record));
  evidence_record = calloc(1U, sizeof(*evidence_record));
  if (evidence == NULL || current == NULL || record == NULL ||
      evidence_record == NULL ||
      !laghu_lcp_scan(evidence_html, page_path, page_origin, evidence) ||
      !laghu_lcp_scan(html, page_path, page_origin, current))
    goto failed;
  if (template_key != NULL && template_key[0] != '\0' && rum != NULL &&
      laghu_rum_engine_read(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, template_key,
                            now, record, sizeof(*record), &value) &&
      value.length == sizeof(*record) &&
      record->version == LAGHU_INSTRUMENTATION_VERSION &&
      now >= record->updated_at && now - record->updated_at <= ttl_seconds) {
    unsigned int bucket;
    char digest[LAGHU_RUNTIME_KEY_SIZE];
    record_ready = laghu_lcp_inventory_record(html, page_path, page_origin,
                                              evidence_record, digest) &&
                   record->media_count == evidence_record->media_count &&
                   memcmp(record->media_kind, evidence_record->media_kind,
                          sizeof(record->media_kind)) == 0 &&
                   memcmp(record->media_keys, evidence_record->media_keys,
                          sizeof(record->media_keys)) == 0 &&
                   memcmp(record->media_resource_count,
                          evidence_record->media_resource_count,
                          sizeof(record->media_resource_count)) == 0 &&
                   memcmp(record->media_resource_keys,
                          evidence_record->media_resource_keys,
                          sizeof(record->media_resource_keys)) == 0;
    if (!record_ready) goto profile_unavailable;
    for (bucket = 0U; bucket < 4U; ++bucket) {
      result->profile_observations[bucket] = record->lcp_observations[bucket];
      result->profile_ready[bucket] = laghu_lcp_bucket_ready(record, bucket);
    }
    learned = laghu_lcp_ready(
        record, viewport_width != 0U && viewport_width < 768U ? 0U : 1U,
        &ordinal, &resource, &observations);
  }
profile_unavailable:
  if (!learned && learned_only) {
    result->decision =
        record_ready ? LAGHU_LCP_DECISION_UNRESOLVED : LAGHU_LCP_DECISION_STALE;
    free(evidence);
    free(current);
    free(record);
    free(evidence_record);
    return true;
  }
  if (!learned) {
    ordinal = laghu_lcp_heuristic(evidence);
    if (ordinal != UINT_MAX && ordinal < current->count &&
        current->candidates[ordinal].resource_count == 1U &&
        current->candidates[ordinal].srcset[0] == '\0')
      resource = 0U;
    else
      ordinal = UINT_MAX;
  }
  if (ordinal == UINT_MAX || ordinal >= evidence->count ||
      ordinal >= current->count ||
      resource >= current->candidates[ordinal].resource_count) {
    result->decision =
        record_ready || template_key == NULL || template_key[0] == '\0'
            ? LAGHU_LCP_DECISION_UNRESOLVED
            : LAGHU_LCP_DECISION_STALE;
    free(evidence);
    free(current);
    free(record);
    free(evidence_record);
    return true;
  }
  result->decision =
      learned ? LAGHU_LCP_DECISION_LEARNED : LAGHU_LCP_DECISION_HEURISTIC;
  result->observations = observations;
  if (!laghu_csp_allows_external_image(csp)) {
    result->decision = LAGHU_LCP_DECISION_CONFLICT;
    free(evidence);
    free(current);
    free(record);
    free(evidence_record);
    return true;
  }
  if (current->candidates[ordinal].priority_low) {
    result->decision = LAGHU_LCP_DECISION_CONFLICT;
    free(evidence);
    free(current);
    free(record);
    free(evidence_record);
    return true;
  }
  if (!laghu_lcp_rewrite_tag(
          html, &current->candidates[ordinal],
          resource_hints &&
              current->candidates[ordinal].kind == LAGHU_LCP_KIND_IMAGE,
          lazyload && (learned || !evidence->candidates[ordinal].lazy),
          &result->data, &result->length))
    goto failed;
  result->rewritten = result->data != NULL;
  if (!result->rewritten) result->length = html.length;
  if (resource_hints) {
    const laghu_lcp_candidate *candidate = &current->candidates[ordinal];
    int written;
    result->link_header = calloc(LAGHU_HTML_HEADER_VALUE_SIZE, 1U);
    if (result->link_header == NULL) goto failed;
    if (candidate->srcset[0] != '\0' &&
        candidate->resource_srcsets[resource][0] == '\0' &&
        laghu_lcp_safe_hint_value(candidate->srcset) &&
        laghu_lcp_safe_hint_value(candidate->sizes))
      written =
          snprintf(result->link_header, LAGHU_HTML_HEADER_VALUE_SIZE,
                   "<%s>; rel=preload; as=image; imagesrcset=\"%s\"%s%s%s",
                   candidate->resource_urls[resource], candidate->srcset,
                   candidate->sizes[0] == '\0' ? "" : "; imagesizes=\"",
                   candidate->sizes, candidate->sizes[0] == '\0' ? "" : "\"");
    else if (candidate->resource_srcsets[resource][0] != '\0' &&
             laghu_lcp_safe_hint_value(candidate->resource_srcsets[resource]) &&
             laghu_lcp_safe_hint_value(candidate->resource_sizes[resource]))
      written = snprintf(
          result->link_header, LAGHU_HTML_HEADER_VALUE_SIZE,
          "<%s>; rel=preload; as=image; imagesrcset=\"%s\"%s%s%s",
          candidate->resource_urls[resource],
          candidate->resource_srcsets[resource],
          candidate->resource_sizes[resource][0] == '\0' ? ""
                                                         : "; imagesizes=\"",
          candidate->resource_sizes[resource],
          candidate->resource_sizes[resource][0] == '\0' ? "" : "\"");
    else
      written = snprintf(result->link_header, LAGHU_HTML_HEADER_VALUE_SIZE,
                         "<%s>; rel=preload; as=image",
                         candidate->resource_urls[resource]);
    if (written <= 0 || (size_t)written >= LAGHU_HTML_HEADER_VALUE_SIZE)
      result->link_header[0] = '\0';
  }
  result->applied = result->rewritten || (result->link_header != NULL &&
                                          result->link_header[0] != '\0');
  {
    char material[LAGHU_RUNTIME_KEY_SIZE + 64U];
    int written = snprintf(material, sizeof(material), "lcp-v1\n%s\n%u\n%u",
                           template_key == NULL ? "heuristic" : template_key,
                           ordinal, (unsigned int)result->decision);
    if (written > 0 && (size_t)written < sizeof(material))
      (void)laghu_sha256_hex(
          (laghu_buffer){(const unsigned char *)material, (size_t)written},
          result->dependency_key);
  }
  free(evidence);
  free(current);
  free(record);
  free(evidence_record);
  return true;
failed:
  free(evidence);
  free(current);
  free(record);
  free(evidence_record);
  laghu_lcp_result_release(result);
  return false;
}

bool laghu_runtime_prioritize_lcp(
    laghu_rum_engine *rum, laghu_buffer evidence_html, laghu_buffer html,
    const char *page_path, const char *page_origin, const char *template_key,
    uint64_t now, unsigned int ttl_seconds, unsigned int viewport_width,
    bool resource_hints, bool lazyload, const laghu_csp_policy *csp,
    laghu_lcp_result *result) {
  return laghu_runtime_prioritize_lcp_impl(
      rum, evidence_html, html, page_path, page_origin, template_key, now,
      ttl_seconds, viewport_width, resource_hints, lazyload, csp, false,
      result);
}

bool laghu_runtime_prioritize_learned_lcp(
    laghu_rum_engine *rum, laghu_buffer evidence_html, laghu_buffer html,
    const char *page_path, const char *page_origin, const char *template_key,
    uint64_t now, unsigned int ttl_seconds, unsigned int viewport_width,
    bool resource_hints, bool lazyload, const laghu_csp_policy *csp,
    laghu_lcp_result *result) {
  return laghu_runtime_prioritize_lcp_impl(
      rum, evidence_html, html, page_path, page_origin, template_key, now,
      ttl_seconds, viewport_width, resource_hints, lazyload, csp, true, result);
}

void laghu_lcp_result_release(laghu_lcp_result *result) {
  if (result != NULL) {
    free(result->data);
    free(result->link_header);
    memset(result, 0, sizeof(*result));
  }
}
