// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_FONTS_H
#define LAGHU_FONTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"
#include "laghu/image.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_FONT_PROVIDER_MAX 16U
#define LAGHU_FONT_PROVIDER_RULE_MAX 8U
#define LAGHU_FONT_PROVIDER_ID_SIZE 64U
#define LAGHU_FONT_PROVIDER_HOST_SIZE 256U
#define LAGHU_FONT_PROVIDER_PREFIX_SIZE 512U
#define LAGHU_FONT_CSS_MAX_BYTES 262144U
#define LAGHU_FONT_FETCH_PROFILE_VERSION 1U
typedef struct {
  char host[LAGHU_FONT_PROVIDER_HOST_SIZE];
  char path_prefix[LAGHU_FONT_PROVIDER_PREFIX_SIZE];
} laghu_font_provider_rule;

typedef struct {
  char id[LAGHU_FONT_PROVIDER_ID_SIZE];
  laghu_font_provider_rule stylesheets[LAGHU_FONT_PROVIDER_RULE_MAX];
  unsigned int stylesheet_count;
  laghu_font_provider_rule redirects[LAGHU_FONT_PROVIDER_RULE_MAX];
  unsigned int redirect_count;
  laghu_font_provider_rule assets[LAGHU_FONT_PROVIDER_RULE_MAX];
  unsigned int asset_count;
  unsigned int max_css_bytes;
  unsigned int ttl_seconds;
  char digest[LAGHU_RUNTIME_KEY_SIZE];
} laghu_font_provider;

typedef struct {
  laghu_font_provider providers[LAGHU_FONT_PROVIDER_MAX];
  unsigned int count;
  char digest[LAGHU_RUNTIME_KEY_SIZE];
} laghu_font_provider_set;

typedef struct {
  char provider_id[LAGHU_FONT_PROVIDER_ID_SIZE];
  char provider_digest[LAGHU_RUNTIME_KEY_SIZE];
  char normalized_url[LAGHU_RUNTIME_PATH_SIZE];
  char variant_key[LAGHU_RUNTIME_KEY_SIZE];
  size_t css_length;
  uint64_t fetched_at;
  uint64_t retry_after;
  unsigned int ttl_seconds;
  bool ready;
  bool terminally_excluded;
} laghu_font_stylesheet_record;
bool laghu_font_providers_load(const char *path, laghu_font_provider_set *set,
                               char *error, size_t error_size);
const laghu_font_provider *laghu_font_provider_match(
    const laghu_font_provider_set *set, const char *url);
const laghu_font_provider *laghu_font_provider_by_id(
    const laghu_font_provider_set *set, const char *id);
bool laghu_font_provider_url_allowed(const laghu_font_provider *provider,
                                     const char *url, bool redirect,
                                     bool asset);
bool laghu_font_css_validate(const laghu_font_provider *provider,
                             laghu_buffer css);
bool laghu_font_stylesheet_key(const char *url, const char *provider_digest,
                               char output[LAGHU_RUNTIME_KEY_SIZE]);
bool laghu_font_stylesheet_publish(const char *cache_path,
                                   const laghu_font_stylesheet_record *record);
bool laghu_font_stylesheet_lookup(const char *cache_path, const char *url,
                                  const laghu_font_provider *provider,
                                  uint64_t now,
                                  laghu_font_stylesheet_record *record);

#ifdef __cplusplus
}
#endif

#endif
