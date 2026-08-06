// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/domain.h"

#include <stdlib.h>
#include <string.h>

#include "laghu/markup.h"

typedef struct {
  unsigned char *data;
  size_t length;
  size_t capacity;
} laghu_domain_builder;

static bool laghu_domain_append(laghu_domain_builder *builder,
                                const void *data, size_t length) {
  size_t required;
  unsigned char *grown;
  if (length > SIZE_MAX - builder->length - 1U) return false;
  required = builder->length + length + 1U;
  if (required > builder->capacity) {
    size_t capacity = builder->capacity == 0U ? 256U : builder->capacity;
    while (capacity < required) {
      if (capacity > SIZE_MAX / 2U) {
        capacity = required;
        break;
      }
      capacity *= 2U;
    }
    grown = realloc(builder->data, capacity);
    if (grown == NULL) return false;
    builder->data = grown;
    builder->capacity = capacity;
  }
  if (length != 0U) memcpy(builder->data + builder->length, data, length);
  builder->length += length;
  builder->data[builder->length] = '\0';
  return true;
}

static bool laghu_domain_url(const laghu_domain_policy *policy,
                             laghu_buffer value, char output[4096U]) {
  char input[4096U];
  if (value.length == 0U || value.length >= sizeof(input)) return false;
  memcpy(input, value.data, value.length);
  input[value.length] = '\0';
  return laghu_domain_url_rewrite(policy, input, output, 4096U);
}

static bool laghu_domain_append_url(laghu_domain_builder *builder,
                                    laghu_buffer value,
                                    const laghu_domain_policy *policy,
                                    bool *changed) {
  char rewritten[4096U];
  if (laghu_domain_url(policy, value, rewritten)) {
    *changed = true;
    return laghu_domain_append(builder, rewritten, strlen(rewritten));
  }
  return laghu_domain_append(builder, value.data, value.length);
}

static bool laghu_domain_attribute_is(const laghu_html_attribute *attribute,
                                      const char *name) {
  size_t length = strlen(name);
  return attribute->name.length == length &&
         memcmp(attribute->name.data, name, length) == 0;
}

static bool laghu_domain_append_srcset(laghu_domain_builder *builder,
                                       laghu_buffer value,
                                       const laghu_domain_policy *policy,
                                       bool *changed) {
  laghu_srcset_candidate candidate;
  size_t cursor = 0U, emitted = 0U;
  while (laghu_srcset_next(value, &cursor, &candidate)) {
    size_t start = (size_t)(candidate.url.data - value.data);
    if (!laghu_domain_append(builder, value.data + emitted, start - emitted) ||
        !laghu_domain_append_url(builder, candidate.url, policy, changed))
      return false;
    emitted = start + candidate.url.length;
  }
  return laghu_domain_append(builder, value.data + emitted, value.length - emitted);
}

static bool laghu_domain_append_style(laghu_domain_builder *builder,
                                      laghu_buffer value,
                                      const laghu_domain_policy *policy,
                                      bool *changed) {
  laghu_domain_rewrite_result rewritten;
  bool appended;
  if (!laghu_domain_rewrite_css(value, policy, &rewritten)) return false;
  if (!rewritten.rewritten) return laghu_domain_append(builder, value.data, value.length);
  appended = laghu_domain_append(builder, rewritten.data, rewritten.length);
  laghu_domain_rewrite_result_release(&rewritten);
  if (appended) *changed = true;
  return appended;
}

static bool laghu_domain_html_tag(laghu_domain_builder *builder,
                                  const laghu_html_tag *tag,
                                  const laghu_domain_policy *policy,
                                  bool *changed) {
  static const char *const attributes[] = {"src", "href", "poster", "data",
                                           "action", "srcset", "style"};
  laghu_html_attribute found[sizeof(attributes) / sizeof(attributes[0])];
  size_t count = 0U, index, cursor = 0U;
  for (index = 0U; index < sizeof(attributes) / sizeof(attributes[0]); ++index) {
    laghu_html_attribute attribute;
    if (laghu_html_tag_attribute(tag, attributes[index], &attribute) &&
        attribute.has_value)
      found[count++] = attribute;
  }
  for (index = 0U; index < count; ++index) {
    size_t later;
    for (later = index + 1U; later < count; ++later) {
      if (found[later].value.data < found[index].value.data) {
        laghu_html_attribute swap = found[index];
        found[index] = found[later];
        found[later] = swap;
      }
    }
  }
  for (index = 0U; index < count; ++index) {
    size_t start = (size_t)(found[index].value.data - tag->source.data);
    if (start < cursor || !laghu_domain_append(builder, tag->source.data + cursor,
                                                start - cursor))
      return false;
    if ((laghu_domain_attribute_is(&found[index], "srcset") &&
         !laghu_domain_append_srcset(builder, found[index].value, policy,
                                     changed)) ||
        (laghu_domain_attribute_is(&found[index], "style") &&
         !laghu_domain_append_style(builder, found[index].value, policy,
                                    changed)) ||
        (!laghu_domain_attribute_is(&found[index], "srcset") &&
         !laghu_domain_attribute_is(&found[index], "style") &&
         !laghu_domain_append_url(builder, found[index].value, policy,
                                  changed)))
      return false;
    cursor = start + found[index].value.length;
  }
  return laghu_domain_append(builder, tag->source.data + cursor,
                             tag->source.length - cursor);
}

bool laghu_domain_rewrite_html(laghu_buffer input,
                               const laghu_domain_policy *policy,
                               laghu_domain_rewrite_result *result) {
  laghu_domain_builder builder = {0};
  size_t cursor = 0U, emitted = 0U;
  laghu_html_tag tag;
  bool changed = false;
  if (result == NULL || !laghu_domain_policy_validate(policy) ||
      (input.data == NULL && input.length != 0U))
    return false;
  memset(result, 0, sizeof(*result));
  while (laghu_html_next_tag(input, &cursor, &tag)) {
    if (!laghu_domain_append(&builder, input.data + emitted, tag.start - emitted) ||
        !laghu_domain_html_tag(&builder, &tag, policy, &changed))
      goto failed;
    emitted = tag.end;
  }
  if (!laghu_domain_append(&builder, input.data + emitted, input.length - emitted))
    goto failed;
  if (changed) {
    result->data = builder.data;
    result->length = builder.length;
    result->rewritten = true;
  } else {
    free(builder.data);
  }
  return true;
failed:
  free(builder.data);
  return false;
}

bool laghu_domain_rewrite_css(laghu_buffer input,
                              const laghu_domain_policy *policy,
                              laghu_domain_rewrite_result *result) {
  laghu_domain_builder builder = {0};
  size_t cursor = 0U, emitted = 0U;
  bool changed = false;
  if (result == NULL || !laghu_domain_policy_validate(policy) ||
      (input.data == NULL && input.length != 0U))
    return false;
  memset(result, 0, sizeof(*result));
  while (cursor + 4U <= input.length) {
    size_t value_start, value_end;
    unsigned char quote = 0U;
    if (memcmp(input.data + cursor, "url(", 4U) != 0) {
      ++cursor;
      continue;
    }
    value_start = cursor + 4U;
    while (value_start < input.length &&
           (input.data[value_start] == ' ' || input.data[value_start] == '\t'))
      ++value_start;
    if (value_start < input.length &&
        (input.data[value_start] == '\'' || input.data[value_start] == '"'))
      quote = input.data[value_start++];
    value_end = value_start;
    while (value_end < input.length &&
           (quote != 0U ? input.data[value_end] != quote
                       : input.data[value_end] != ')'))
      ++value_end;
    if (value_end == input.length ||
        (quote != 0U && (value_end + 1U == input.length ||
                          input.data[value_end + 1U] != ')'))) {
      ++cursor;
      continue;
    }
    if (!laghu_domain_append(&builder, input.data + emitted,
                             value_start - emitted) ||
        !laghu_domain_append_url(
            &builder, (laghu_buffer){input.data + value_start,
                                     value_end - value_start},
            policy, &changed))
      goto failed;
    cursor = quote == 0U ? value_end + 1U : value_end + 2U;
    emitted = value_end;
  }
  if (!laghu_domain_append(&builder, input.data + emitted, input.length - emitted))
    goto failed;
  if (changed) {
    result->data = builder.data;
    result->length = builder.length;
    result->rewritten = true;
  } else {
    free(builder.data);
  }
  return true;
failed:
  free(builder.data);
  return false;
}

void laghu_domain_rewrite_result_release(laghu_domain_rewrite_result *result) {
  if (result != NULL) {
    free(result->data);
    memset(result, 0, sizeof(*result));
  }
}
