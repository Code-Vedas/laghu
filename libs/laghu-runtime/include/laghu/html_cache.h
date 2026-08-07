// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_HTML_CACHE_H
#define LAGHU_HTML_CACHE_H

#include "laghu/cache.h"

typedef enum {
  LAGHU_HTML_CACHE_MISS = 0,
  LAGHU_HTML_CACHE_FRESH,
  LAGHU_HTML_CACHE_STALE
} laghu_html_cache_state;

typedef struct {
  laghu_runtime_cache_entry entry;
  uint64_t stored_at;
  laghu_html_cache_state state;
} laghu_html_cache_record;

bool laghu_html_cache_key(const char *origin, const char *request_path,
                          char output[LAGHU_RUNTIME_KEY_SIZE]);
bool laghu_html_cache_publish(const char *cache_path, const char *origin,
                              const char *request_path, const char *validator,
                              laghu_buffer body, uint64_t now,
                              laghu_html_cache_record *record);
bool laghu_html_cache_lookup(const char *cache_path, const char *origin,
                             const char *request_path, uint64_t now,
                             unsigned int ttl, unsigned int stale_ttl,
                             laghu_html_cache_record *record);
/* A confirmed 304 renews only its matching stored representation.  The
 * metadata update is one filesystem operation, so it cannot replace a newer
 * published body. */
bool laghu_html_cache_renew(const char *cache_path, const char *origin,
                            const char *request_path, const char *validator,
                            uint64_t now, laghu_html_cache_record *record);

#endif
