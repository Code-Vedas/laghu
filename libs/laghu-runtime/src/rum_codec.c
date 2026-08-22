// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/catalog.h"
#include "laghu/css.h"
#include "laghu/instrumentation.h"
#include "laghu/lcp.h"
#include "laghu/rum.h"
#include "rum_internal.h"

typedef struct {
  unsigned char *data;
  size_t capacity;
  size_t offset;
} laghu_rum_codec;

static bool laghu_rum_codec_bytes(laghu_rum_codec *codec, void *value, size_t length, bool write) {
  if (codec->offset > codec->capacity || length > codec->capacity - codec->offset) return false;
  if (write)
    memcpy(codec->data + codec->offset, value, length);
  else
    memcpy(value, codec->data + codec->offset, length);
  codec->offset += length;
  return true;
}

static bool laghu_rum_codec_u16(laghu_rum_codec *codec, uint16_t *value, bool write) {
  unsigned char bytes[2];
  if (write) {
    bytes[0] = (unsigned char)(*value & 0xffU);
    bytes[1] = (unsigned char)(*value >> 8U);
  }
  if (!laghu_rum_codec_bytes(codec, bytes, sizeof(bytes), write)) return false;
  if (!write) *value = (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8U));
  return true;
}

static bool laghu_rum_codec_u32(laghu_rum_codec *codec, uint32_t *value, bool write) {
  unsigned char bytes[4];
  unsigned int index;
  if (write)
    for (index = 0U; index < 4U; ++index) bytes[index] = (unsigned char)(*value >> (index * 8U));
  if (!laghu_rum_codec_bytes(codec, bytes, sizeof(bytes), write)) return false;
  if (!write) {
    *value = 0U;
    for (index = 0U; index < 4U; ++index) *value |= (uint32_t)bytes[index] << (index * 8U);
  }
  return true;
}

static bool laghu_rum_codec_u64(laghu_rum_codec *codec, uint64_t *value, bool write) {
  unsigned char bytes[8];
  unsigned int index;
  if (write)
    for (index = 0U; index < 8U; ++index) bytes[index] = (unsigned char)(*value >> (index * 8U));
  if (!laghu_rum_codec_bytes(codec, bytes, sizeof(bytes), write)) return false;
  if (!write) {
    *value = 0U;
    for (index = 0U; index < 8U; ++index) *value |= (uint64_t)bytes[index] << (index * 8U);
  }
  return true;
}

static bool laghu_rum_codec_record(laghu_rum_record_type type, void *record, laghu_rum_codec *codec, bool write) {
  unsigned int i, j, k;
#define BYTES(field)                                                                \
  do {                                                                              \
    if (!laghu_rum_codec_bytes(codec, (field), sizeof(field), write)) return false; \
  } while (0)
#define U16(field)                                                  \
  do {                                                              \
    if (!laghu_rum_codec_u16(codec, &(field), write)) return false; \
  } while (0)
#define U32(field)                                            \
  do {                                                        \
    uint32_t v = (uint32_t)(field);                           \
    if (!laghu_rum_codec_u32(codec, &v, write)) return false; \
    if (!write) (field) = v;                                  \
  } while (0)
#define U64(field)                                                  \
  do {                                                              \
    if (!laghu_rum_codec_u64(codec, &(field), write)) return false; \
  } while (0)
  if (type == LAGHU_RUM_RECORD_IMAGE) {
    laghu_rum_image_record *r = record;
    unsigned char flag = r->above_fold ? 1U : 0U;
    BYTES(r->identity);
    U64(r->updated_at);
    U32(r->width);
    U32(r->height);
    U32(r->mobile_width);
    U32(r->mobile_height);
    U32(r->viewport_width);
    U32(r->dpr_hundredths);
    if (!laghu_rum_codec_bytes(codec, &flag, 1U, write) || flag > 1U) return false;
    if (!write) r->above_fold = flag != 0U;
  } else if (type == LAGHU_RUM_RECORD_CRITICAL_CSS) {
    laghu_critical_css_record *r = record;
    U32(r->version);
    BYTES(r->template_key);
    BYTES(r->stylesheet_url);
    BYTES(r->stylesheet_key);
    BYTES(r->policy_key);
    U64(r->updated_at);
    U32(r->generation);
    for (i = 0U; i < 4U; ++i) U16(r->observation_count[i]);
    BYTES(r->critical_rules);
  } else if (type == LAGHU_RUM_RECORD_INSTRUMENTATION) {
    laghu_rum_instrumentation_record *r = record;
    U32(r->version);
    BYTES(r->template_key);
    BYTES(r->provider_digest);
    BYTES(r->policy_key);
    U64(r->updated_at);
    U32(r->script_count);
    BYTES(r->script_keys);
    for (i = 0U; i < 2U; ++i) {
      U64(r->observations[i]);
      for (j = 0U; j < 5U; ++j) U64(r->metric_sums[i][j]);
      for (j = 0U; j < 5U; ++j) U32(r->metric_maxima[i][j]);
      for (j = 0U; j < LAGHU_RUM_HISTOGRAMS; ++j)
        for (k = 0U; k < LAGHU_RUM_BUCKETS; ++k) U64(r->histograms[i][j][k]);
      U64(r->errors[i]);
      U64(r->rejections[i]);
      for (j = 0U; j < LAGHU_INSTRUMENTATION_MAX_SCRIPTS; ++j) {
        U64(r->script_observations[i][j]);
        U64(r->script_before_dcl[i][j]);
        U64(r->script_long_tasks[i][j]);
      }
    }
    U32(r->media_count);
    BYTES(r->media_kind);
    BYTES(r->media_keys);
    BYTES(r->media_resource_count);
    BYTES(r->media_resource_keys);
    for (i = 0U; i < 4U; ++i) {
      U16(r->lcp_observations[i]);
      U16(r->lcp_unresolved[i]);
      for (j = 0U; j < LAGHU_LCP_MAX_CANDIDATES; ++j) U16(r->lcp_candidates[i][j]);
      for (j = 0U; j < LAGHU_LCP_MAX_CANDIDATES; ++j)
        for (k = 0U; k < LAGHU_LCP_MAX_RESOURCES; ++k) U16(r->lcp_resources[i][j][k]);
    }
  } else {
    return laghu_rum_codec_bytes(codec, record, codec->capacity, write);
  }
#undef U64
#undef U32
#undef U16
#undef BYTES
  return true;
}

bool laghu_rum_encode(laghu_rum_record_type type, const void *record, size_t length, unsigned char *encoded, size_t capacity,
                      size_t *encoded_length) {
  laghu_rum_codec codec = {encoded, capacity, 0U};
  unsigned char *copy;
  bool success;
  if (type == LAGHU_RUM_RECORD_DECISION) {
    if (length > capacity) return false;
    memcpy(encoded, record, length);
    *encoded_length = length;
    return true;
  }
  copy = malloc(length);
  if (copy == NULL) return false;
  memcpy(copy, record, length);
  success = laghu_rum_codec_record(type, copy, &codec, true);
  free(copy);
  if (success) *encoded_length = codec.offset;
  return success;
}

bool laghu_rum_decode(laghu_rum_record_type type, const unsigned char *encoded, size_t length, void *record, size_t record_length) {
  laghu_rum_codec codec = {(unsigned char *)encoded, length, 0U};
  if (type == LAGHU_RUM_RECORD_DECISION) {
    if (length != record_length) return false;
    memcpy(record, encoded, length);
    return true;
  }
  memset(record, 0, record_length);
  return laghu_rum_codec_record(type, record, &codec, false) && codec.offset == length;
}

bool laghu_rum_same_identity(laghu_rum_record_type type, const void *left, const void *right, size_t length) {
  if (type == LAGHU_RUM_RECORD_IMAGE && length == sizeof(laghu_rum_image_record))
    return strcmp(((const laghu_rum_image_record *)left)->identity, ((const laghu_rum_image_record *)right)->identity) == 0;
  if (type == LAGHU_RUM_RECORD_CRITICAL_CSS && length == sizeof(laghu_critical_css_record)) {
    const laghu_critical_css_record *a = left, *b = right;
    return strcmp(a->template_key, b->template_key) == 0 && strcmp(a->stylesheet_key, b->stylesheet_key) == 0 &&
           strcmp(a->policy_key, b->policy_key) == 0;
  }
  if (type == LAGHU_RUM_RECORD_INSTRUMENTATION && length == sizeof(laghu_rum_instrumentation_record)) {
    const laghu_rum_instrumentation_record *a = left, *b = right;
    return strcmp(a->template_key, b->template_key) == 0 && strcmp(a->provider_digest, b->provider_digest) == 0 &&
           strcmp(a->policy_key, b->policy_key) == 0 && a->script_count == b->script_count &&
           memcmp(a->script_keys, b->script_keys, sizeof(a->script_keys)) == 0 && a->media_count == b->media_count &&
           memcmp(a->media_kind, b->media_kind, sizeof(a->media_kind)) == 0 && memcmp(a->media_keys, b->media_keys, sizeof(a->media_keys)) == 0 &&
           memcmp(a->media_resource_count, b->media_resource_count, sizeof(a->media_resource_count)) == 0 &&
           memcmp(a->media_resource_keys, b->media_resource_keys, sizeof(a->media_resource_keys)) == 0;
  }
  if (type == LAGHU_RUM_RECORD_DECISION) return memcmp(left, right, length) == 0;
  return false;
}

bool laghu_rum_record_merge(laghu_rum_record_type type, void *target, const void *delta, size_t length) {
  unsigned int i, j, k;
  if (!laghu_rum_same_identity(type, target, delta, length)) return false;
  if (type == LAGHU_RUM_RECORD_DECISION) return true;
  if (type == LAGHU_RUM_RECORD_IMAGE) {
    laghu_rum_image_record *a = target;
    const laghu_rum_image_record *b = delta;
#define LAGHU_RUM_MAX(field) \
  if (b->field > a->field) a->field = b->field
    LAGHU_RUM_MAX(updated_at);
    LAGHU_RUM_MAX(width);
    LAGHU_RUM_MAX(height);
    LAGHU_RUM_MAX(mobile_width);
    LAGHU_RUM_MAX(mobile_height);
    LAGHU_RUM_MAX(viewport_width);
    LAGHU_RUM_MAX(dpr_hundredths);
#undef LAGHU_RUM_MAX
    a->above_fold = a->above_fold || b->above_fold;
    return true;
  }
  if (type == LAGHU_RUM_RECORD_CRITICAL_CSS) {
    laghu_critical_css_record *a = target;
    const laghu_critical_css_record *b = delta;
    if (b->updated_at > a->updated_at) a->updated_at = b->updated_at;
    a->generation = UINT32_MAX - a->generation < b->generation ? UINT32_MAX : a->generation + b->generation;
    for (i = 0U; i < 4U; ++i) {
      unsigned int sum = a->observation_count[i] + b->observation_count[i];
      a->observation_count[i] = (uint16_t)(sum > UINT16_MAX ? UINT16_MAX : sum);
      for (j = 0U; j < LAGHU_CRITICAL_CSS_MAX_RULES / 8U; ++j) a->critical_rules[i][j] |= b->critical_rules[i][j];
    }
    return true;
  }
  if (type == LAGHU_RUM_RECORD_INSTRUMENTATION) {
    laghu_rum_instrumentation_record *a = target;
    const laghu_rum_instrumentation_record *b = delta;
    if (b->updated_at > a->updated_at) a->updated_at = b->updated_at;
    for (i = 0U; i < 2U; ++i) {
#define SAT_ADD(target_, value_)                                                       \
  do {                                                                                 \
    (target_) = UINT64_MAX - (target_) < (value_) ? UINT64_MAX : (target_) + (value_); \
  } while (0)
      SAT_ADD(a->observations[i], b->observations[i]);
      SAT_ADD(a->errors[i], b->errors[i]);
      SAT_ADD(a->rejections[i], b->rejections[i]);
      for (j = 0U; j < 5U; ++j) {
        SAT_ADD(a->metric_sums[i][j], b->metric_sums[i][j]);
        if (b->metric_maxima[i][j] > a->metric_maxima[i][j]) a->metric_maxima[i][j] = b->metric_maxima[i][j];
      }
      for (j = 0U; j < LAGHU_RUM_HISTOGRAMS; ++j)
        for (k = 0U; k < LAGHU_RUM_BUCKETS; ++k) SAT_ADD(a->histograms[i][j][k], b->histograms[i][j][k]);
      for (j = 0U; j < a->script_count; ++j) {
        SAT_ADD(a->script_observations[i][j], b->script_observations[i][j]);
        SAT_ADD(a->script_before_dcl[i][j], b->script_before_dcl[i][j]);
        SAT_ADD(a->script_long_tasks[i][j], b->script_long_tasks[i][j]);
      }
#undef SAT_ADD
    }
    for (i = 0U; i < 4U; ++i) {
      unsigned int sum = a->lcp_observations[i] + b->lcp_observations[i];
      a->lcp_observations[i] = (uint16_t)(sum > UINT16_MAX ? UINT16_MAX : sum);
      sum = a->lcp_unresolved[i] + b->lcp_unresolved[i];
      a->lcp_unresolved[i] = (uint16_t)(sum > UINT16_MAX ? UINT16_MAX : sum);
      for (j = 0U; j < LAGHU_LCP_MAX_CANDIDATES; ++j) {
        sum = a->lcp_candidates[i][j] + b->lcp_candidates[i][j];
        a->lcp_candidates[i][j] = (uint16_t)(sum > UINT16_MAX ? UINT16_MAX : sum);
        for (k = 0U; k < LAGHU_LCP_MAX_RESOURCES; ++k) {
          sum = a->lcp_resources[i][j][k] + b->lcp_resources[i][j][k];
          a->lcp_resources[i][j][k] = (uint16_t)(sum > UINT16_MAX ? UINT16_MAX : sum);
        }
      }
    }
    return true;
  }
  return false;
}

void laghu_rum_zero_observations(laghu_rum_record_type type, void *data, size_t length) {
  if (type == LAGHU_RUM_RECORD_IMAGE && length == sizeof(laghu_rum_image_record)) {
    laghu_rum_image_record *record = data;
    record->width = record->height = record->mobile_width = 0U;
    record->mobile_height = record->viewport_width = record->dpr_hundredths = 0U;
    record->above_fold = false;
  } else if (type == LAGHU_RUM_RECORD_CRITICAL_CSS && length == sizeof(laghu_critical_css_record)) {
    laghu_critical_css_record *record = data;
    memset(record->observation_count, 0, sizeof(record->observation_count));
    memset(record->critical_rules, 0, sizeof(record->critical_rules));
    record->generation = 0U;
  } else if (type == LAGHU_RUM_RECORD_INSTRUMENTATION && length == sizeof(laghu_rum_instrumentation_record)) {
    laghu_rum_instrumentation_record *record = data;
    memset(record->observations, 0, sizeof(*record) - offsetof(laghu_rum_instrumentation_record, observations));
  }
}
