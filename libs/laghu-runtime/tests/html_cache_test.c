// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/html_cache.h"

#include <assert.h>
#include <string.h>

#include "test_fixture.h"

int main(void) {
  char root[LAGHU_RUNTIME_PATH_SIZE];
  unsigned char body[] = "<html>cached</html>";
  laghu_html_cache_record record;
  laghu_cache_limits limits;
  assert(laghu_test_directory(root, sizeof(root)));
  laghu_cache_limits_init(&limits);
  assert(laghu_cache_backend_register_path(root, &limits));
  assert(laghu_html_cache_publish(root, "https://origin.example", "/index.html", "\"v1\"", (laghu_buffer){body, sizeof(body) - 1U}, 100U, &record));
  assert(laghu_html_cache_publish(root, "https://origin.example", "/without-record.html", "\"v1\"", (laghu_buffer){body, sizeof(body) - 1U}, 100U,
                                  NULL));
  assert(laghu_html_cache_lookup(root, "https://origin.example", "/index.html", record.stored_at, 30U, 30U, &record));
  assert(record.state == LAGHU_HTML_CACHE_FRESH);
  assert(laghu_html_cache_lookup(root, "https://origin.example", "/index.html", record.stored_at + 30U, 30U, 30U, &record));
  assert(record.state == LAGHU_HTML_CACHE_STALE);
  assert(laghu_html_cache_lookup(root, "https://origin.example", "/index.html", record.stored_at + 60U, 30U, 30U, &record));
  assert(record.state == LAGHU_HTML_CACHE_MISS);
  assert(laghu_html_cache_renew(root, "https://origin.example", "/index.html", "\"v1\"", 400U, &record));
  assert(record.stored_at == 400U);
  assert(laghu_html_cache_lookup(root, "https://origin.example", "/index.html", 429U, 30U, 30U, &record));
  assert(record.state == LAGHU_HTML_CACHE_FRESH);
  assert(!laghu_html_cache_renew(root, "https://origin.example", "/index.html", "\"other\"", 500U, &record));
  assert(!laghu_html_cache_key("https://origin.example", "relative", (char[LAGHU_RUNTIME_KEY_SIZE]){0}));
  return 0;
}
