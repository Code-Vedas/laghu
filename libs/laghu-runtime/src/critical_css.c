// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/runtime.h"

typedef struct {
  size_t start;
  size_t end;
  bool foundational;
} laghu_critical_rule;

static const char laghu_critical_script[] =
    "addEventListener('load',()=>{const s=document.currentScript||document."
    "querySelector('script[data-laghu-critical]');if(!s)return;const out=[];"
    "for(const sh of document.styleSheets){let rs;try{rs=sh.cssRules}catch(e)"
    "{continue}if(!rs)continue;for(let i=0;i<rs.length&&i<512;i++){const r=rs"
    "[i];let hit=false;const walk=x=>{if(hit)return;if(x.selectorText){try{for"
    "(const e of document.querySelectorAll(x.selectorText)){const b=e."
    "getBoundingClientRect();if(b.width>0&&b.height>0&&b.bottom>0&&b.top<"
    "innerHeight){hit=true;break}}}catch(e){}}else if(x.cssRules){for(const c "
    "of"
    "x.cssRules)walk(c)}};walk(r);if(hit)out.push(i)}}fetch('/.laghu/beacon/"
    "critical-css',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({template:s.dataset.laghuCritical,bucket:innerWidth<"
    "768?0:1,rules:out.slice(0,512)}),keepalive:true})});";

const char *laghu_runtime_critical_css_beacon_script(void) {
  return laghu_critical_script;
}

static bool laghu_critical_hash(const char *value) {
  size_t index;
  if (value == NULL || strlen(value) != LAGHU_SHA256_HEX_LENGTH) return false;
  for (index = 0U; index < LAGHU_SHA256_HEX_LENGTH; ++index)
    if (!((value[index] >= '0' && value[index] <= '9') ||
          (value[index] >= 'a' && value[index] <= 'f')))
      return false;
  return true;
}

static bool laghu_critical_read_record(laghu_rum_engine *rum, const char *key,
                                       uint64_t now,
                                       laghu_critical_css_record *record) {
  laghu_rum_value value;
  if (rum != NULL &&
      laghu_rum_engine_read(rum, LAGHU_RUM_RECORD_CRITICAL_CSS, key, now,
                            record, sizeof(*record), &value) &&
      value.length == sizeof(*record) &&
      record->version == LAGHU_CRITICAL_CSS_VERSION &&
      strcmp(record->template_key, key) == 0)
    return true;
  return false;
}

static bool laghu_critical_write_record(laghu_rum_engine *rum,
                                        laghu_critical_css_record *record) {
  return rum != NULL &&
         laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_CRITICAL_CSS,
                                  record->template_key, record->updated_at,
                                  record, sizeof(*record), NULL);
}

static bool laghu_critical_json_uint(const char **cursor, unsigned int *value) {
  unsigned long parsed = 0U;
  const char *start = *cursor;
  while (isdigit((unsigned char)**cursor)) {
    parsed = parsed * 10U + (unsigned int)(**cursor - '0');
    if (parsed > 65535U) return false;
    ++*cursor;
  }
  if (*cursor == start) return false;
  *value = (unsigned int)parsed;
  return true;
}

bool laghu_runtime_parse_critical_css_beacon(
    laghu_buffer json, laghu_critical_css_beacon *record) {
  char *text;
  char *template_at;
  char *bucket_at;
  char *rules_at;
  const char *cursor;
  size_t index = 0U;
  if (record == NULL || json.data == NULL || json.length == 0U ||
      json.length > 16384U || memchr(json.data, '\0', json.length) != NULL)
    return false;
  text = malloc(json.length + 1U);
  if (text == NULL) return false;
  memcpy(text, json.data, json.length);
  text[json.length] = '\0';
  memset(record, 0, sizeof(*record));
  template_at = strstr(text, "\"template\":\"");
  bucket_at = strstr(text, "\"bucket\":");
  rules_at = strstr(text, "\"rules\":[");
  if (template_at == NULL || bucket_at == NULL || rules_at == NULL) goto bad;
  template_at += sizeof("\"template\":\"") - 1U;
  if (strlen(template_at) < LAGHU_SHA256_HEX_LENGTH + 1U ||
      template_at[LAGHU_SHA256_HEX_LENGTH] != '"')
    goto bad;
  memcpy(record->template_key, template_at, LAGHU_SHA256_HEX_LENGTH);
  record->template_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
  if (!laghu_critical_hash(record->template_key)) goto bad;
  cursor = bucket_at + sizeof("\"bucket\":") - 1U;
  if (!laghu_critical_json_uint(&cursor, &record->viewport_bucket) ||
      record->viewport_bucket > 1U)
    goto bad;
  cursor = rules_at + sizeof("\"rules\":[") - 1U;
  while (*cursor != ']') {
    unsigned int rule;
    if (index == LAGHU_CRITICAL_CSS_MAX_RULES ||
        !laghu_critical_json_uint(&cursor, &rule) ||
        rule >= LAGHU_CRITICAL_CSS_MAX_RULES)
      goto bad;
    record->rules[index++] = (uint16_t)rule;
    if (*cursor == ',')
      ++cursor;
    else if (*cursor != ']')
      goto bad;
  }
  record->rule_count = (unsigned int)index;
  free(text);
  return true;
bad:
  free(text);
  return false;
}

typedef struct {
  const laghu_critical_css_beacon *beacon;
  const char *policy_key;
  uint64_t now;
  unsigned int ttl_seconds;
} laghu_critical_merge_context;

static bool laghu_critical_merge(void *data, size_t length, void *opaque) {
  laghu_critical_css_record *record = data;
  laghu_critical_merge_context *context = opaque;
  unsigned int index, bucket;
  if (length != sizeof(*record) ||
      record->version != LAGHU_CRITICAL_CSS_VERSION ||
      strcmp(record->template_key, context->beacon->template_key) != 0 ||
      strcmp(record->policy_key, context->policy_key) != 0 ||
      context->now < record->updated_at ||
      context->now - record->updated_at > context->ttl_seconds)
    return false;
  bucket = context->beacon->viewport_bucket;
  for (index = 0U; index < context->beacon->rule_count; ++index) {
    unsigned int rule = context->beacon->rules[index];
    record->critical_rules[bucket][rule / 8U] |=
        (unsigned char)(1U << (rule % 8U));
  }
  if (record->observation_count[bucket] < UINT16_MAX)
    ++record->observation_count[bucket];
  ++record->generation;
  record->updated_at = context->now;
  return true;
}

bool laghu_critical_css_apply_beacon(laghu_rum_engine *rum,
                                     const char *cache_path,
                                     const char *policy_key, uint64_t now,
                                     unsigned int ttl_seconds,
                                     const laghu_critical_css_beacon *beacon) {
  laghu_critical_css_record record;
  laghu_critical_merge_context context;
  if (rum == NULL || cache_path == NULL || !laghu_critical_hash(policy_key) ||
      beacon == NULL || beacon->viewport_bucket > 1U || ttl_seconds == 0U)
    return false;
  if (!laghu_critical_read_record(rum, beacon->template_key, now, &record) ||
      strcmp(record.policy_key, policy_key) != 0 || now < record.updated_at ||
      now - record.updated_at > ttl_seconds)
    return false;
  context.beacon = beacon;
  context.policy_key = policy_key;
  context.now = now;
  context.ttl_seconds = ttl_seconds;
  return laghu_rum_engine_update(rum, LAGHU_RUM_RECORD_CRITICAL_CSS,
                                 beacon->template_key, now,
                                 laghu_critical_merge, &context, NULL);
}

static bool laghu_critical_equal(const unsigned char *data, size_t length,
                                 const char *value) {
  size_t i;
  if (strlen(value) != length) return false;
  for (i = 0U; i < length; ++i)
    if (tolower(data[i]) != tolower((unsigned char)value[i])) return false;
  return true;
}

static const unsigned char *laghu_critical_find(const unsigned char *data,
                                                size_t length,
                                                const char *needle) {
  size_t index;
  size_t needle_length = strlen(needle);
  for (index = 0U; index + needle_length <= length; ++index)
    if (memcmp(data + index, needle, needle_length) == 0) return data + index;
  return NULL;
}

static bool laghu_critical_attr(const unsigned char *tag, size_t length,
                                const char *name, const unsigned char **value,
                                size_t *value_length) {
  size_t p = 1U;
  while (p < length && !isspace(tag[p]) && tag[p] != '>') ++p;
  while (p < length) {
    size_t begin, end, start;
    unsigned char quote = 0U;
    while (p < length && (isspace(tag[p]) || tag[p] == '/')) ++p;
    begin = p;
    while (p < length && (isalnum(tag[p]) || tag[p] == '-' || tag[p] == '_'))
      ++p;
    end = p;
    if (begin == end) break;
    while (p < length && isspace(tag[p])) ++p;
    if (p >= length || tag[p] != '=') continue;
    ++p;
    while (p < length && isspace(tag[p])) ++p;
    if (p < length && (tag[p] == '\'' || tag[p] == '"')) quote = tag[p++];
    start = p;
    while (p < length && ((quote && tag[p] != quote) ||
                          (!quote && !isspace(tag[p]) && tag[p] != '>')))
      ++p;
    if (laghu_critical_equal(tag + begin, end - begin, name)) {
      *value = tag + start;
      *value_length = p - start;
      return true;
    }
    if (quote && p < length) ++p;
  }
  return false;
}

static bool laghu_critical_rules(laghu_buffer css, laghu_critical_rule *rules,
                                 unsigned int *count) {
  size_t start = 0U, p = 0U;
  unsigned int found = 0U;
  int depth = 0;
  unsigned char quote = 0U;
  bool comment = false;
  while (p < css.length) {
    unsigned char c = css.data[p];
    if (comment) {
      if (c == '*' && p + 1U < css.length && css.data[p + 1U] == '/') {
        comment = false;
        p += 2U;
      } else
        ++p;
      continue;
    }
    if (quote) {
      if (c == '\\' && p + 1U < css.length)
        p += 2U;
      else {
        if (c == quote) quote = 0U;
        ++p;
      }
      continue;
    }
    if (c == '/' && p + 1U < css.length && css.data[p + 1U] == '*') {
      comment = true;
      p += 2U;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      ++p;
      continue;
    }
    if (c == '{')
      ++depth;
    else if (c == '}') {
      if (--depth < 0) return false;
      if (depth == 0) {
        size_t lead = start;
        while (lead < p && isspace(css.data[lead])) ++lead;
        if (found == LAGHU_CRITICAL_CSS_MAX_RULES) return false;
        rules[found].start = start;
        rules[found].end = p + 1U;
        rules[found].foundational =
            lead < p && css.data[lead] == '@' &&
            (strncmp((const char *)css.data + lead, "@font-face", 10U) == 0 ||
             strncmp((const char *)css.data + lead, "@keyframes", 10U) == 0 ||
             strncmp((const char *)css.data + lead, "@-webkit-keyframes",
                     18U) == 0);
        ++found;
        start = p + 1U;
      }
    } else if (c == ';' && depth == 0) {
      start = p + 1U;
    }
    ++p;
  }
  if (depth != 0 || quote || comment) return false;
  *count = found;
  return true;
}

static bool laghu_critical_template_key(laghu_buffer html,
                                        const char *page_path,
                                        const laghu_stylesheet_record *sheet,
                                        const char *policy_key,
                                        char key[LAGHU_RUNTIME_KEY_SIZE]) {
  unsigned char structure[8192U];
  size_t used = 0U, p = 0U;
  char material[10240U];
  int length;
  while (p < html.length && used + 2U < sizeof(structure)) {
    const unsigned char *open = memchr(html.data + p, '<', html.length - p);
    size_t q;
    if (open == NULL) break;
    q = (size_t)(open - html.data) + 1U;
    if (q < html.length && html.data[q] == '/') structure[used++] = '/';
    if (q < html.length &&
        (html.data[q] == '/' || html.data[q] == '!' || html.data[q] == '?'))
      ++q;
    while (q < html.length && (isalnum(html.data[q]) || html.data[q] == '-'))
      structure[used++] = (unsigned char)tolower(html.data[q++]);
    {
      const unsigned char *tag_end =
          memchr(html.data + q, '>', html.length - q);
      const unsigned char *attribute;
      size_t attribute_length;
      if (tag_end != NULL &&
          laghu_critical_attr(open, (size_t)(tag_end - open + 1U), "id",
                              &attribute, &attribute_length) &&
          used + attribute_length + 1U < sizeof(structure)) {
        structure[used++] = '#';
        memcpy(structure + used, attribute, attribute_length);
        used += attribute_length;
      }
      if (tag_end != NULL &&
          laghu_critical_attr(open, (size_t)(tag_end - open + 1U), "class",
                              &attribute, &attribute_length) &&
          used + attribute_length + 1U < sizeof(structure)) {
        structure[used++] = '.';
        memcpy(structure + used, attribute, attribute_length);
        used += attribute_length;
      }
    }
    structure[used++] = '\n';
    p = q;
  }
  length = snprintf(material, sizeof(material),
                    "critical-template-v%u\n%s\n%s\n%s\n%s\n%.*s",
                    LAGHU_CRITICAL_CSS_VERSION, page_path, sheet->source_hash,
                    sheet->dependency_key, policy_key, (int)used, structure);
  return length > 0 && (size_t)length < sizeof(material) &&
         laghu_sha256_hex(
             (laghu_buffer){(const unsigned char *)material, (size_t)length},
             key);
}

bool laghu_runtime_prioritize_critical_css(
    laghu_rum_engine *rum, const char *cache_path, laghu_buffer html,
    const char *page_path, const char *page_origin, const char *policy_key,
    uint32_t capability_mask, uint64_t now, unsigned int ttl_seconds,
    unsigned int inline_limit, unsigned int outline_threshold,
    unsigned int viewport_width, bool beacon_enabled,
    bool csp_allows_inline_styles, bool csp_allows_self_styles,
    bool csp_allows_self_scripts, laghu_runtime_html_result *result) {
  const unsigned char *link = NULL, *body = NULL, *scan;
  size_t link_start = 0U, link_end = 0U, href_length = 0U, rel_length = 0U;
  const unsigned char *href = NULL, *rel = NULL, *media = NULL;
  size_t media_length = 0U;
  char url[LAGHU_RUNTIME_PATH_SIZE], template_key[LAGHU_RUNTIME_KEY_SIZE];
  laghu_stylesheet_record sheet;
  laghu_critical_css_record learning;
  laghu_runtime_cache_entry entry;
  unsigned char *css = NULL, *output = NULL;
  laghu_critical_rule rules[LAGHU_CRITICAL_CSS_MAX_RULES];
  unsigned int rule_count = 0U, i,
               bucket = viewport_width && viewport_width < 768U ? 0U : 1U;
  size_t critical_length = 0U, output_length, cursor;
  static const char deferred_prefix[] = "/.laghu/css/";
  bool ready = false;
  bool created = false;
  (void)page_origin;
  if (result == NULL || rum == NULL || cache_path == NULL ||
      page_path == NULL || policy_key == NULL || inline_limit > 65536U)
    return false;
  memset(result, 0, sizeof(*result));
  scan = html.data;
  while ((scan = laghu_critical_find(
              scan, html.length - (size_t)(scan - html.data), "<link")) !=
         NULL) {
    const unsigned char *end =
        memchr(scan, '>', html.length - (size_t)(scan - html.data));
    if (end == NULL) return true;
    if (laghu_critical_attr(scan, (size_t)(end - scan + 1U), "rel", &rel,
                            &rel_length) &&
        laghu_critical_equal(rel, rel_length, "stylesheet")) {
      if (link != NULL) return true;
      link = scan;
      link_start = (size_t)(scan - html.data);
      link_end = (size_t)(end - html.data + 1U);
    }
    scan = end + 1U;
  }
  if (link == NULL ||
      laghu_critical_find(html.data, html.length, "<style") != NULL ||
      !laghu_critical_attr(link, link_end - link_start, "href", &href,
                           &href_length) ||
      href_length == 0U || href[0] != '/' || href_length >= sizeof(url))
    return true;
  if (laghu_critical_find(link, link_end - link_start, "integrity") ||
      laghu_critical_find(link, link_end - link_start, "nonce") ||
      laghu_critical_find(link, link_end - link_start, "disabled") ||
      laghu_critical_find(link, link_end - link_start, "alternate"))
    return true;
  memcpy(url, href, href_length);
  url[href_length] = '\0';
  if (!laghu_stylesheet_lookup(cache_path, url, policy_key, capability_mask,
                               inline_limit, outline_threshold, now,
                               ttl_seconds, &sheet) ||
      !sheet.ready || sheet.derived_length > sheet.source_length ||
      !laghu_critical_template_key(html, page_path, &sheet, policy_key,
                                   template_key))
    return true;
  memset(&learning, 0, sizeof(learning));
  if (laghu_critical_read_record(rum, template_key, now, &learning) &&
      strcmp(learning.stylesheet_key, sheet.dependency_key) == 0 &&
      strcmp(learning.policy_key, policy_key) == 0 &&
      now >= learning.updated_at && now - learning.updated_at <= ttl_seconds)
    ready = learning.observation_count[bucket] >= LAGHU_CRITICAL_CSS_QUORUM;
  else {
    learning.version = LAGHU_CRITICAL_CSS_VERSION;
    memcpy(learning.template_key, template_key, sizeof(template_key));
    memcpy(learning.stylesheet_url, url, href_length + 1U);
    memcpy(learning.stylesheet_key, sheet.dependency_key,
           sizeof(learning.stylesheet_key));
    memcpy(learning.policy_key, policy_key, sizeof(learning.policy_key));
    learning.updated_at = now;
    if (!laghu_critical_write_record(rum, &learning)) return true;
    created = true;
  }
  if (ready && csp_allows_inline_styles && csp_allows_self_styles &&
      laghu_runtime_cache_lookup_variant(cache_path, sheet.derived_key,
                                         &entry)) {
    css = malloc(entry.length + 1U);
    if (css == NULL || !laghu_runtime_cache_read(&entry, css, entry.length))
      goto fail;
    css[entry.length] = '\0';
    if (!laghu_critical_rules((laghu_buffer){css, entry.length}, rules,
                              &rule_count))
      goto unchanged;
    for (i = 0U; i < rule_count; ++i)
      if (rules[i].foundational ||
          (learning.critical_rules[bucket][i / 8U] & (1U << (i % 8U))))
        critical_length += rules[i].end - rules[i].start;
    if (critical_length == 0U || critical_length > inline_limit) goto unchanged;
    body = laghu_critical_find(html.data, html.length, "</body>");
    if (body == NULL || (size_t)(body - html.data) < link_end) goto unchanged;
    output_length = html.length - (link_end - link_start) + critical_length +
                    sizeof("<style></style>") - 1U + (link_end - link_start) -
                    href_length + sizeof(deferred_prefix) - 1U +
                    LAGHU_SHA256_HEX_LENGTH;
    output = malloc(output_length + 1U);
    if (output == NULL) goto fail;
    cursor = 0U;
#define APPEND(data_, length_)                   \
  do {                                           \
    memcpy(output + cursor, (data_), (length_)); \
    cursor += (length_);                         \
  } while (0)
    APPEND(html.data, link_start);
    APPEND("<style", 6U);
    if (laghu_critical_attr(link, link_end - link_start, "media", &media,
                            &media_length)) {
      APPEND(" media=\"", 8U);
      APPEND(media, media_length);
      APPEND("\"", 1U);
    }
    APPEND(">", 1U);
    for (i = 0U; i < rule_count; ++i)
      if (rules[i].foundational ||
          (learning.critical_rules[bucket][i / 8U] & (1U << (i % 8U))))
        APPEND(css + rules[i].start, rules[i].end - rules[i].start);
    APPEND("</style>", 8U);
    APPEND(html.data + link_end, (size_t)(body - html.data) - link_end);
    APPEND(link, (size_t)(href - link));
    APPEND(deferred_prefix, sizeof(deferred_prefix) - 1U);
    APPEND(sheet.derived_key, LAGHU_SHA256_HEX_LENGTH);
    APPEND(href + href_length,
           link_end - (size_t)(href + href_length - html.data));
    APPEND(body, html.length - (size_t)(body - html.data));
#undef APPEND
    output[cursor] = '\0';
    result->data = output;
    result->length = cursor;
    result->rewritten = true;
    {
      char material[LAGHU_RUNTIME_KEY_SIZE * 3U + 64U];
      int n = snprintf(material, sizeof(material),
                       "critical-v1\n%s\n%s\n%u\n%u", template_key,
                       sheet.dependency_key, bucket, learning.generation);
      if (n <= 0 || (size_t)n >= sizeof(material) ||
          !laghu_sha256_hex(
              (laghu_buffer){(const unsigned char *)material, (size_t)n},
              result->dependency_key))
        goto fail;
    }
    free(css);
    return true;
  }
unchanged:
  free(css);
  if (created) return true;
  if (beacon_enabled && csp_allows_self_scripts) {
    body = laghu_critical_find(html.data, html.length, "</body>");
    if (body != NULL) {
      const char before[] =
          "<script src=\"/.laghu/beacon/critical-css.js\" defer "
          "data-laghu-critical=\"";
      const char after[] = "\"></script>";
      size_t at = (size_t)(body - html.data);
      output_length = html.length + sizeof(before) - 1U +
                      LAGHU_SHA256_HEX_LENGTH + sizeof(after) - 1U;
      output = malloc(output_length + 1U);
      if (output == NULL) return false;
      memcpy(output, html.data, at);
      memcpy(output + at, before, sizeof(before) - 1U);
      memcpy(output + at + sizeof(before) - 1U, template_key,
             LAGHU_SHA256_HEX_LENGTH);
      memcpy(output + at + sizeof(before) - 1U + LAGHU_SHA256_HEX_LENGTH, after,
             sizeof(after) - 1U);
      memcpy(output + at + sizeof(before) - 1U + LAGHU_SHA256_HEX_LENGTH +
                 sizeof(after) - 1U,
             body, html.length - at);
      output[output_length] = '\0';
      result->data = output;
      result->length = output_length;
      result->rewritten = true;
      (void)laghu_sha256_hex((laghu_buffer){output, output_length},
                             result->dependency_key);
    }
  }
  return true;
fail:
  free(css);
  free(output);
  memset(result, 0, sizeof(*result));
  return false;
}
