// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/runtime.h"

static const char *laghu_beacon_field(const char *json, const char *name) {
  char needle[64U];
  int length = snprintf(needle, sizeof(needle), "\"%s\"", name);
  const char *field;
  if (length <= 0 || (size_t)length >= sizeof(needle) ||
      (field = strstr(json, needle)) == NULL) {
    return NULL;
  }
  field += length;
  while (isspace((unsigned char)*field)) {
    ++field;
  }
  if (*field++ != ':') {
    return NULL;
  }
  while (isspace((unsigned char)*field)) {
    ++field;
  }
  return field;
}

static bool laghu_beacon_unsigned(const char *json, const char *name,
                                  unsigned int minimum, unsigned int maximum,
                                  unsigned int *output) {
  const char *field = laghu_beacon_field(json, name);
  char *end;
  unsigned long value;
  if (field == NULL || !isdigit((unsigned char)*field)) {
    return false;
  }
  value = strtoul(field, &end, 10);
  if (end == field || value < minimum || value > maximum) {
    return false;
  }
  *output = (unsigned int)value;
  return true;
}

static bool laghu_beacon_boolean(const char *json, const char *name,
                                 bool *output) {
  const char *field = laghu_beacon_field(json, name);
  if (field != NULL && strncmp(field, "true", 4U) == 0) {
    *output = true;
    return true;
  }
  if (field != NULL && strncmp(field, "false", 5U) == 0) {
    *output = false;
    return true;
  }
  return false;
}

bool laghu_runtime_parse_image_beacon(laghu_buffer json,
                                      laghu_image_beacon_record *record) {
  char *text;
  const char *url;
  const char *end;
  size_t length;
  if (record == NULL || json.data == NULL || json.length == 0U ||
      json.length > 16384U || memchr(json.data, '\0', json.length) != NULL) {
    return false;
  }
  text = malloc(json.length + 1U);
  if (text == NULL) {
    return false;
  }
  memcpy(text, json.data, json.length);
  text[json.length] = '\0';
  memset(record, 0, sizeof(*record));
  url = laghu_beacon_field(text, "url");
  if (url == NULL || *url++ != '"' || (end = strchr(url, '"')) == NULL) {
    free(text);
    return false;
  }
  length = (size_t)(end - url);
  if (length == 0U || length >= sizeof(record->normalized_url) ||
      url[0] != '/' || (length > 1U && url[1] == '/') ||
      memchr(url, '?', length) != NULL || memchr(url, '#', length) != NULL) {
    free(text);
    return false;
  }
  memcpy(record->normalized_url, url, length);
  record->normalized_url[length] = '\0';
  if ((strncmp(record->normalized_url, "/api", 4U) == 0 &&
       (record->normalized_url[4] == '\0' ||
        record->normalized_url[4] == '/')) ||
      (strncmp(record->normalized_url, "/graphql", 8U) == 0 &&
       (record->normalized_url[8] == '\0' ||
        record->normalized_url[8] == '/'))) {
    free(text);
    return false;
  }
  if (!laghu_beacon_unsigned(text, "width", 1U, LAGHU_IMAGE_MAX_DIMENSION,
                             &record->width) ||
      !laghu_beacon_unsigned(text, "height", 1U, LAGHU_IMAGE_MAX_DIMENSION,
                             &record->height) ||
      !laghu_beacon_unsigned(text, "viewport_width", 1U,
                             LAGHU_IMAGE_MAX_DIMENSION,
                             &record->viewport_width) ||
      !laghu_beacon_unsigned(text, "dpr_hundredths", 100U, 400U,
                             &record->dpr_hundredths) ||
      !laghu_beacon_boolean(text, "above_fold", &record->above_fold) ||
      !laghu_beacon_boolean(text, "mobile", &record->mobile)) {
    free(text);
    return false;
  }
  free(text);
  return true;
}

bool laghu_catalog_apply_beacon(const char *cache_path, const char *policy_key,
                                uint32_t capability_mask, uint64_t now,
                                unsigned int ttl_seconds,
                                const laghu_image_beacon_record *beacon) {
  laghu_catalog_record catalog;
  if (beacon == NULL ||
      !laghu_catalog_lookup_url(cache_path, beacon->normalized_url, policy_key,
                                capability_mask, now, ttl_seconds, &catalog)) {
    return false;
  }
  if (beacon->mobile) {
    catalog.learned_mobile_width = beacon->width;
    catalog.learned_mobile_height = beacon->height;
  } else {
    catalog.learned_width = beacon->width;
    catalog.learned_height = beacon->height;
  }
  catalog.learned_viewport_width = beacon->viewport_width;
  catalog.learned_dpr_hundredths = beacon->dpr_hundredths;
  catalog.learned_above_fold = beacon->above_fold;
  catalog.learned_at = now;
  catalog.updated_at = now;
  catalog.last_accessed_at = now;
  return laghu_catalog_publish_url(cache_path, &catalog);
}
