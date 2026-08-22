// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/html_cache.h"

#if defined(_WIN32)
#include <sys/utime.h>
#else
#include <fcntl.h>
#endif
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "laghu/types.h"

static bool laghu_html_cache_touch(const char *path, uint64_t now) {
  if (path == NULL || now > (uint64_t)INT64_MAX) return false;
#if defined(_WIN32)
  {
    struct _utimbuf times = {.actime = (__time64_t)now, .modtime = (__time64_t)now};
    return _utime64(path, &times) == 0;
  }
#else
  {
    struct timespec times[2];
    times[0] = (struct timespec){.tv_sec = (time_t)now, .tv_nsec = 0L};
    times[1] = times[0];
    return utimensat(AT_FDCWD, path, times, 0) == 0;
  }
#endif
}

bool laghu_html_cache_key(const char *origin, const char *request_path, char output[LAGHU_RUNTIME_KEY_SIZE]) {
  char canonical[LAGHU_RUNTIME_PATH_SIZE + LAGHU_DOMAIN_ORIGIN_SIZE + 2U];
  int written;
  if (origin == NULL || request_path == NULL || output == NULL || origin[0] == '\0' || request_path[0] != '/') return false;
  written = snprintf(canonical, sizeof(canonical), "%s\n%s", origin, request_path);
  return written > 0 && (size_t)written < sizeof(canonical) &&
         laghu_sha256_hex((laghu_buffer){(const unsigned char *)canonical, (size_t)written}, output);
}

bool laghu_html_cache_publish(const char *cache_path, const char *origin, const char *request_path, const char *validator, laghu_buffer body,
                              uint64_t now, laghu_html_cache_record *record) {
  char key[LAGHU_RUNTIME_KEY_SIZE];
  laghu_runtime_cache_entry entry;
  struct stat information;
  (void)now;
  if (body.data == NULL || body.length == 0U || !laghu_html_cache_key(origin, request_path, key) ||
      !laghu_runtime_cache_publish(cache_path, key, key, validator == NULL ? "" : validator, "text/html", "laghu-html-cache", body, &entry))
    return false;
  if (record == NULL) return true;
  memset(record, 0, sizeof(*record));
  record->entry = entry;
  if (stat(entry.variant_path, &information) != 0 || information.st_mtime < 0) return false;
  record->stored_at = (uint64_t)information.st_mtime;
  record->state = LAGHU_HTML_CACHE_FRESH;
  return true;
}

bool laghu_html_cache_lookup(const char *cache_path, const char *origin, const char *request_path, uint64_t now, unsigned int ttl,
                             unsigned int stale_ttl, laghu_html_cache_record *record) {
  char key[LAGHU_RUNTIME_KEY_SIZE];
  struct stat information;
  uint64_t stored, fresh_until, stale_until;
  laghu_runtime_cache_entry entry;
  if (record == NULL) return false;
  memset(record, 0, sizeof(*record));
  if (ttl == 0U || !laghu_html_cache_key(origin, request_path, key) || !laghu_runtime_cache_lookup_variant(cache_path, key, &entry) ||
      stat(entry.variant_path, &information) != 0 || information.st_mtime < 0)
    return true;
  stored = (uint64_t)information.st_mtime;
  if (stored > now || ttl > UINT64_MAX - stored) return true;
  fresh_until = stored + ttl;
  if (stale_ttl > UINT64_MAX - fresh_until) return true;
  stale_until = fresh_until + stale_ttl;
  if (now >= stale_until) return true;
  record->entry = entry;
  record->stored_at = stored;
  record->state = now < fresh_until ? LAGHU_HTML_CACHE_FRESH : LAGHU_HTML_CACHE_STALE;
  return true;
}

bool laghu_html_cache_renew(const char *cache_path, const char *origin, const char *request_path, const char *validator, uint64_t now,
                            laghu_html_cache_record *record) {
  char key[LAGHU_RUNTIME_KEY_SIZE];
  laghu_runtime_cache_entry entry;
  struct stat information;
  if (record == NULL || validator == NULL || validator[0] == '\0' || !laghu_html_cache_key(origin, request_path, key)) {
    return false;
  }
  if (!laghu_runtime_cache_lookup_variant(cache_path, key, &entry) || strcmp(entry.validator, validator) != 0) {
    return false;
  }
  if (!laghu_html_cache_touch(entry.variant_path, now) || stat(entry.variant_path, &information) != 0 || information.st_mtime < 0) {
    return false;
  }
  memset(record, 0, sizeof(*record));
  record->entry = entry;
  record->stored_at = (uint64_t)information.st_mtime;
  record->state = LAGHU_HTML_CACHE_FRESH;
  return true;
}
