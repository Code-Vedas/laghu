// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "laghu/runtime.h"

static bool laghu_csp_equal(const char *value, size_t length,
                            const char *expected) {
  size_t index;
  if (strlen(expected) != length) return false;
  for (index = 0U; index < length; ++index)
    if (tolower((unsigned char)value[index]) !=
        tolower((unsigned char)expected[index]))
      return false;
  return true;
}

static bool laghu_csp_prefix_equal(const char *left, const char *right,
                                   size_t length) {
  size_t index;
  for (index = 0U; index < length; ++index)
    if (tolower((unsigned char)left[index]) !=
        tolower((unsigned char)right[index]))
      return false;
  return true;
}

static bool laghu_csp_nonce_hash(const char *value, size_t length,
                                 unsigned char output[32U]) {
  size_t index;
  unsigned int padding = 0U;
  char hex[LAGHU_RUNTIME_KEY_SIZE];
  if (length == 0U || length > 128U) return false;
  for (index = 0U; index < length; ++index) {
    unsigned char byte = (unsigned char)value[index];
    if (byte == '=') {
      size_t suffix;
      padding = (unsigned int)(length - index);
      if (padding > 2U) return false;
      for (suffix = index; suffix < length; ++suffix)
        if (value[suffix] != '=') return false;
      break;
    } else if (padding != 0U || !(isalnum(byte) || byte == '+' || byte == '/' ||
                                  byte == '-' || byte == '_'))
      return false;
  }
  if (!laghu_sha256_hex((laghu_buffer){(const unsigned char *)value, length},
                        hex))
    return false;
  for (index = 0U; index < 32U; ++index) {
    unsigned int high =
        (unsigned int)(hex[index * 2U] <= '9' ? hex[index * 2U] - '0'
                                              : hex[index * 2U] - 'a' + 10);
    unsigned int low = (unsigned int)(hex[index * 2U + 1U] <= '9'
                                          ? hex[index * 2U + 1U] - '0'
                                          : hex[index * 2U + 1U] - 'a' + 10);
    output[index] = (unsigned char)((high << 4U) | low);
  }
  return true;
}

static int laghu_csp_meta_content(const unsigned char *meta,
                                  const unsigned char *close,
                                  const unsigned char **content,
                                  size_t *content_length) {
  const unsigned char *cursor = meta + 5U;
  bool enforcing = false;
  bool found_content = false;
  while (cursor < close) {
    const unsigned char *name;
    const unsigned char *value;
    size_t name_length;
    size_t value_length;
    unsigned char quote = 0U;
    while (cursor < close && isspace(*cursor)) ++cursor;
    if (cursor == close || *cursor == '/') break;
    name = cursor;
    while (cursor < close && !isspace(*cursor) && *cursor != '=' &&
           *cursor != '/')
      ++cursor;
    name_length = (size_t)(cursor - name);
    while (cursor < close && isspace(*cursor)) ++cursor;
    if (cursor == close || *cursor != '=') {
      while (cursor < close && !isspace(*cursor)) ++cursor;
      continue;
    }
    ++cursor;
    while (cursor < close && isspace(*cursor)) ++cursor;
    if (cursor == close) return enforcing ? -1 : 0;
    if (*cursor == '\'' || *cursor == '"') quote = *cursor++;
    value = cursor;
    if (quote != 0U) {
      while (cursor < close && *cursor != quote) ++cursor;
      if (cursor == close) return enforcing ? -1 : 0;
    } else {
      while (cursor < close && !isspace(*cursor) && *cursor != '>') ++cursor;
    }
    value_length = (size_t)(cursor - value);
    if (quote != 0U) ++cursor;
    if (laghu_csp_equal((const char *)name, name_length, "http-equiv") &&
        laghu_csp_equal((const char *)value, value_length,
                        "Content-Security-Policy"))
      enforcing = true;
    else if (laghu_csp_equal((const char *)name, name_length, "content")) {
      *content = value;
      *content_length = value_length;
      found_content = true;
    }
  }
  if (!enforcing) return 0;
  return found_content && *content_length != 0U ? 1 : -1;
}

static bool laghu_csp_origin_source(const laghu_csp_policy *policy,
                                    const char *token, size_t length) {
  const char *origin = policy->origin;
  size_t origin_length = strlen(origin);
  const char *scheme = strstr(origin, "://");
  const char *origin_authority;
  size_t origin_scheme_length;
  size_t origin_authority_length;
  const char *source_authority = token;
  size_t source_authority_length = length;
  const char *source_scheme;
  const char *source_path;
  if (laghu_csp_equal(token, length, "'self'") ||
      laghu_csp_equal(token, length, "*"))
    return true;
  if (origin_length != 0U && length == origin_length &&
      laghu_csp_equal(token, length, origin))
    return true;
  if (scheme != NULL && length == (size_t)(scheme - origin) + 1U &&
      token[length - 1U] == ':' &&
      laghu_csp_prefix_equal(token, origin, length - 1U))
    return true;
  if (scheme == NULL) return false;
  origin_scheme_length = (size_t)(scheme - origin);
  origin_authority = scheme + 3U;
  origin_authority_length = origin_length - (size_t)(origin_authority - origin);
  source_scheme = NULL;
  {
    size_t index;
    for (index = 0U; index + 3U <= length; ++index)
      if (token[index] == ':' && token[index + 1U] == '/' &&
          token[index + 2U] == '/') {
        source_scheme = token + index;
        break;
      }
  }
  if (source_scheme != NULL) {
    size_t source_scheme_length = (size_t)(source_scheme - token);
    if (source_scheme_length != origin_scheme_length ||
        !laghu_csp_prefix_equal(token, origin, origin_scheme_length))
      return false;
    source_authority = source_scheme + 3U;
    source_authority_length = length - (size_t)(source_authority - token);
  }
  source_path = memchr(source_authority, '/', source_authority_length);
  if (source_path != NULL) {
    size_t path_length =
        source_authority_length - (size_t)(source_path - source_authority);
    if (path_length != 1U) return false;
    source_authority_length = (size_t)(source_path - source_authority);
  }
  if (source_authority_length == 1U && source_authority[0] == '*') return true;
  if (source_authority_length == origin_authority_length &&
      laghu_csp_prefix_equal(source_authority, origin_authority,
                             origin_authority_length))
    return true;
  if (source_authority_length > 2U && source_authority[0] == '*' &&
      source_authority[1] == '.' &&
      origin_authority_length > source_authority_length - 1U &&
      laghu_csp_prefix_equal(origin_authority + origin_authority_length -
                                 (source_authority_length - 1U),
                             source_authority + 1U,
                             source_authority_length - 1U))
    return true;
  return false;
}

static void laghu_csp_parse_sources(laghu_csp_policy *policy,
                                    laghu_csp_entry *entry,
                                    laghu_csp_source_list *list,
                                    const char *start, const char *end) {
  const char *cursor = start;
  list->present = true;
  while (cursor < end) {
    const char *token;
    const char *token_end;
    while (cursor < end && isspace((unsigned char)*cursor)) ++cursor;
    token = cursor;
    while (cursor < end && !isspace((unsigned char)*cursor)) ++cursor;
    token_end = cursor;
    if (token == token_end) continue;
    if (laghu_csp_equal(token, (size_t)(token_end - token), "'unsafe-inline'"))
      list->unsafe_inline = true;
    else if (laghu_csp_equal(token, (size_t)(token_end - token),
                             "'unsafe-hashes'"))
      list->unsafe_hashes = true;
    else if (laghu_csp_equal(token, (size_t)(token_end - token),
                             "'strict-dynamic'"))
      list->strict_dynamic = true;
    else if ((size_t)(token_end - token) > 8U && token[0] == '\'' &&
             laghu_csp_equal(token + 1U, 6U, "nonce-") &&
             token_end[-1] == '\'') {
      unsigned char hash[32U];
      unsigned int index;
      if (!laghu_csp_nonce_hash(token + 7U, (size_t)(token_end - token) - 8U,
                                hash)) {
        list->invalid = true;
        continue;
      }
      for (index = 0U; index < entry->nonce_count; ++index)
        if (memcmp(hash, entry->nonce_hashes[index], sizeof(hash)) == 0) break;
      if (index == entry->nonce_count) {
        if (entry->nonce_count >= LAGHU_CSP_MAX_NONCES) {
          list->invalid = true;
          continue;
        }
        memcpy(entry->nonce_hashes[index], hash, sizeof(hash));
        ++entry->nonce_count;
      }
      list->nonce_mask |= 1U << index;
    } else if ((size_t)(token_end - token) > 9U && token[0] == '\'' &&
               (laghu_csp_equal(token + 1U, 7U, "sha256-") ||
                laghu_csp_equal(token + 1U, 7U, "sha384-") ||
                laghu_csp_equal(token + 1U, 7U, "sha512-"))) {
      list->has_hash =
          token_end[-1] == '\'' &&
          laghu_csp_nonce_hash(token + 8U, (size_t)(token_end - token) - 9U,
                               (unsigned char[32U]){0});
      if (!list->has_hash) list->invalid = true;
    } else if (laghu_csp_equal(token, (size_t)(token_end - token), "data:"))
      list->allows_data = true;
    else if (laghu_csp_origin_source(policy, token,
                                     (size_t)(token_end - token)))
      list->allows_self = true;
  }
}

static bool laghu_csp_find_directive(const char *value, size_t length,
                                     const char *name, const char **start,
                                     const char **end) {
  const char *cursor = value;
  const char *limit = value + length;
  while (cursor < limit) {
    const char *section_end = memchr(cursor, ';', (size_t)(limit - cursor));
    const char *name_start;
    const char *name_end;
    if (section_end == NULL) section_end = limit;
    name_start = cursor;
    while (name_start < section_end && isspace((unsigned char)*name_start))
      ++name_start;
    name_end = name_start;
    while (name_end < section_end && !isspace((unsigned char)*name_end))
      ++name_end;
    if (laghu_csp_equal(name_start, (size_t)(name_end - name_start), name)) {
      *start = name_end;
      *end = section_end;
      return true;
    }
    cursor = section_end < limit ? section_end + 1U : limit;
  }
  return false;
}

static void laghu_csp_parse_context(laghu_csp_policy *policy,
                                    laghu_csp_entry *entry,
                                    laghu_csp_context context,
                                    const char *value, size_t length) {
  static const char *const names[LAGHU_CSP_CONTEXT_COUNT][3] = {
      {"script-src-elem", "script-src", "default-src"},
      {"script-src-attr", "script-src", "default-src"},
      {"style-src-elem", "style-src", "default-src"},
      {"style-src-attr", "style-src", "default-src"},
      {"img-src", "default-src", NULL}};
  unsigned int priority;
  for (priority = 0U; priority < 3U && names[context][priority] != NULL;
       ++priority) {
    const char *start;
    const char *end;
    if (laghu_csp_find_directive(value, length, names[context][priority],
                                 &start, &end)) {
      laghu_csp_parse_sources(policy, entry, &entry->contexts[context], start,
                              end);
      return;
    }
  }
}

void laghu_csp_policy_init(laghu_csp_policy *policy, const char *page_origin) {
  if (policy == NULL) return;
  memset(policy, 0, sizeof(*policy));
  if (page_origin == NULL || strlen(page_origin) >= sizeof(policy->origin)) {
    policy->invalid = page_origin != NULL;
    return;
  }
  (void)snprintf(policy->origin, sizeof(policy->origin), "%s", page_origin);
}

bool laghu_csp_policy_add(laghu_csp_policy *policy, const char *value,
                          size_t length) {
  const char *cursor = value;
  const char *limit;
  if (policy == NULL || value == NULL || length == 0U || length > 8192U)
    return false;
  limit = value + length;
  while (cursor < limit) {
    const char *end = memchr(cursor, ',', (size_t)(limit - cursor));
    laghu_csp_entry *entry;
    unsigned int context;
    if (end == NULL) end = limit;
    while (cursor < end && isspace((unsigned char)*cursor)) ++cursor;
    if (cursor == end || policy->policy_count >= LAGHU_CSP_MAX_POLICIES) {
      policy->invalid = true;
      return false;
    }
    entry = &policy->policies[policy->policy_count++];
    for (context = 0U; context < LAGHU_CSP_CONTEXT_COUNT; ++context)
      laghu_csp_parse_context(policy, entry, (laghu_csp_context)context, cursor,
                              (size_t)(end - cursor));
    for (context = 0U; context < LAGHU_CSP_CONTEXT_COUNT; ++context)
      if (entry->contexts[context].invalid) {
        policy->invalid = true;
        return false;
      }
    cursor = end < limit ? end + 1U : limit;
  }
  return !policy->invalid;
}

bool laghu_csp_policy_add_meta(laghu_csp_policy *policy, laghu_buffer html) {
  const unsigned char *cursor = html.data;
  const unsigned char *end = html.data + html.length;
  unsigned int count = 0U;
  while (cursor < end) {
    const unsigned char *meta = memchr(cursor, '<', (size_t)(end - cursor));
    const unsigned char *close;
    const unsigned char *content = NULL;
    size_t content_length = 0U;
    int meta_result;
    if (meta == NULL) break;
    if ((size_t)(end - meta) < 5U ||
        !laghu_csp_equal((const char *)meta, 5U, "<meta") ||
        ((size_t)(end - meta) > 5U && !isspace(meta[5U]) && meta[5U] != '>' &&
         meta[5U] != '/')) {
      cursor = meta + 1U;
      continue;
    }
    close = memchr(meta, '>', (size_t)(end - meta));
    if (close == NULL) {
      policy->invalid = true;
      return false;
    }
    meta_result =
        laghu_csp_meta_content(meta, close, &content, &content_length);
    if (meta_result == 0) {
      cursor = close + 1U;
      continue;
    }
    if (meta_result < 0) {
      policy->invalid = true;
      return false;
    }
    if (++count > LAGHU_CSP_MAX_POLICIES) {
      policy->invalid = true;
      return false;
    }
    if (!laghu_csp_policy_add(policy, (const char *)content, content_length))
      return false;
    cursor = close + 1U;
  }
  return !policy->invalid;
}

static bool laghu_csp_nonce(const laghu_csp_entry *entry,
                            const laghu_csp_source_list *list,
                            const unsigned char *nonce, size_t length) {
  unsigned char hash[32U];
  unsigned int index;
  if (nonce == NULL || !laghu_csp_nonce_hash((const char *)nonce, length, hash))
    return false;
  for (index = 0U; index < entry->nonce_count; ++index)
    if ((list->nonce_mask & (1U << index)) != 0U &&
        memcmp(hash, entry->nonce_hashes[index], sizeof(hash)) == 0)
      return true;
  return false;
}

static bool laghu_csp_all(const laghu_csp_policy *policy,
                          laghu_csp_context context, const unsigned char *nonce,
                          size_t nonce_length, unsigned int operation) {
  unsigned int index;
  if (policy == NULL || (policy->policy_count == 0U && !policy->invalid))
    return true;
  if (policy->invalid) return false;
  for (index = 0U; index < policy->policy_count; ++index) {
    const laghu_csp_entry *entry = &policy->policies[index];
    const laghu_csp_source_list *list = &entry->contexts[context];
    bool allowed = false;
    if (list->invalid) return false;
    if (!list->present) continue;
    if (operation == 0U)
      allowed = list->allows_data;
    else if (operation == 1U)
      allowed =
          laghu_csp_nonce(entry, list, nonce, nonce_length) ||
          (list->unsafe_inline && list->nonce_mask == 0U && !list->has_hash);
    else if (operation == 2U)
      allowed =
          laghu_csp_nonce(entry, list, nonce, nonce_length) ||
          (list->allows_self &&
           !(context == LAGHU_CSP_SCRIPT_ELEMENT && list->strict_dynamic &&
             (list->nonce_mask != 0U || list->has_hash)));
    else
      allowed =
          list->unsafe_inline && list->nonce_mask == 0U && !list->has_hash;
    if (!allowed) return false;
  }
  return true;
}

bool laghu_csp_allows_data_image(const laghu_csp_policy *policy) {
  return laghu_csp_all(policy, LAGHU_CSP_IMAGE, NULL, 0U, 0U);
}
bool laghu_csp_allows_inline_style(const laghu_csp_policy *policy,
                                   const unsigned char *nonce,
                                   size_t nonce_length) {
  return laghu_csp_all(policy, LAGHU_CSP_STYLE_ELEMENT, nonce, nonce_length,
                       1U);
}
bool laghu_csp_allows_external_style(const laghu_csp_policy *policy,
                                     const unsigned char *nonce,
                                     size_t nonce_length) {
  (void)nonce;
  (void)nonce_length;
  return laghu_csp_all(policy, LAGHU_CSP_STYLE_ELEMENT, NULL, 0U, 2U);
}
bool laghu_csp_allows_style_attribute(const laghu_csp_policy *policy) {
  return laghu_csp_all(policy, LAGHU_CSP_STYLE_ATTRIBUTE, NULL, 0U, 3U);
}
bool laghu_csp_allows_inline_script(const laghu_csp_policy *policy,
                                    const unsigned char *nonce,
                                    size_t nonce_length) {
  return laghu_csp_all(policy, LAGHU_CSP_SCRIPT_ELEMENT, nonce, nonce_length,
                       1U);
}
bool laghu_csp_allows_external_script(const laghu_csp_policy *policy,
                                      const unsigned char *nonce,
                                      size_t nonce_length) {
  return laghu_csp_all(policy, LAGHU_CSP_SCRIPT_ELEMENT, nonce, nonce_length,
                       2U);
}
