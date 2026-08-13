// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/service_config.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SOURCE_ROOT "/srv/assets"
#define TEST_CACHE_ROOT "/tmp/laghu-cache"
#define TEST_CACHE_URI "file:///tmp/laghu-cache"
#define TEST_PARENT_CACHE_ROOT "/tmp/parent-cache"
#define TEST_PARENT_CACHE_URI "file:///tmp/parent-cache"
#define TEST_CHILD_CACHE_ROOT "/tmp/child-cache"
#define TEST_CHILD_CACHE_URI "file:///tmp/child-cache"

static void test_descriptors(void) {
  laghu_service_setting setting;
  for (setting = LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND; setting < LAGHU_SERVICE_SETTING_COUNT; ++setting) {
    const laghu_service_setting_descriptor *descriptor = laghu_service_setting_describe(setting);
    assert(descriptor != NULL);
    assert(descriptor->setting == setting);
    assert(descriptor->name != NULL && descriptor->name[0] != '\0');
    assert(laghu_service_setting_find(descriptor->name) == setting);
  }
  assert(laghu_service_setting_find("FileCacheSize") == LAGHU_SERVICE_SETTING_FILE_CACHE_SIZE);
  assert(laghu_service_setting_find("--rum-store") == LAGHU_SERVICE_SETTING_RUM_STORE);
  assert(laghu_service_setting_find("not-a-setting") == LAGHU_SERVICE_SETTING_UNKNOWN);
}

static void test_config_size(void) { assert(sizeof(laghu_service_config) < 65536U); }

static void test_descriptor_matrix(void) {
  static const struct {
    laghu_service_setting setting;
    const char *value;
  } cases[] = {{LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND, "file:///tmp/cache"},
               {LAGHU_SERVICE_SETTING_IMAGE_CACHE, "/tmp/cache"},
               {LAGHU_SERVICE_SETTING_FILE_CACHE_SIZE, "1m"},
               {LAGHU_SERVICE_SETTING_FILE_CACHE_INODE_LIMIT, "16"},
               {LAGHU_SERVICE_SETTING_FILE_CACHE_CLEAN_INTERVAL, "1s"},
               {LAGHU_SERVICE_SETTING_FILE_CACHE_METADATA_SIZE, "16k"},
               {LAGHU_SERVICE_SETTING_WORKER_QUEUE, "/tmp/jobs.queue"},
               {LAGHU_SERVICE_SETTING_HTML_REFRESH_QUEUE, "/tmp/html-refresh.queue"},
               {LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_QUEUE, "/tmp/chrome.queue"},
               {LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_TIMEOUT, "1500"},
               {LAGHU_SERVICE_SETTING_FONT_FETCH_QUEUE, "/tmp/fonts.queue"},
               {LAGHU_SERVICE_SETTING_FONT_PROVIDER_CONFIG, "/tmp/fonts.conf"},
               {LAGHU_SERVICE_SETTING_JAVASCRIPT_QUEUE, "/tmp/javascript.queue"},
               {LAGHU_SERVICE_SETTING_JAVASCRIPT_TARGET, "defaults and supports es6-module and not dead"},
               {LAGHU_SERVICE_SETTING_JAVASCRIPT_OBSERVATION_CONFIG, "/tmp/observe"},
               {LAGHU_SERVICE_SETTING_JAVASCRIPT_DEFER_CONFIG, "/tmp/defer"},
               {LAGHU_SERVICE_SETTING_ASSET_OFFLOAD_CONFIG, "/tmp/assets"},
               {LAGHU_SERVICE_SETTING_ASSET_UPLOAD_QUEUE, "/tmp/assets.queue"},
               {LAGHU_SERVICE_SETTING_RUM_STORE, "local:"},
               {LAGHU_SERVICE_SETTING_RUM_STORE_LOCAL_SNAPSHOT, "/tmp/rum.snapshot"},
               {LAGHU_SERVICE_SETTING_RUM_STORE_CLIENT_LIBRARY, "/tmp/rum.so"},
               {LAGHU_SERVICE_SETTING_RUM_STORE_REQUIRED, "on"},
               {LAGHU_SERVICE_SETTING_RUM_STORE_TIMEOUT, "10"},
               {LAGHU_SERVICE_SETTING_RUM_STORE_TTL, "3600s"},
               {LAGHU_SERVICE_SETTING_RUM_STORE_RETRY_LIMIT, "0"},
               {LAGHU_SERVICE_SETTING_RUM_STORE_SYNC_INTERVAL, "1s"},
               {LAGHU_SERVICE_SETTING_RUM_STORE_MEMORY_LIMIT, "16k"},
               {LAGHU_SERVICE_SETTING_RUM_STORE_PENDING_LIMIT, "16k"},
               {LAGHU_SERVICE_SETTING_LOAD_FROM_FILE, "mapped"},
               {LAGHU_SERVICE_SETTING_PURGE_METHOD, "PURGE"},
               {LAGHU_SERVICE_SETTING_PURGE_QUERY, "on"},
               {LAGHU_SERVICE_SETTING_STATISTICS, "on"},
               {LAGHU_SERVICE_SETTING_METRICS, "on"},
               {LAGHU_SERVICE_SETTING_READINESS, "on"},
               {LAGHU_SERVICE_SETTING_READINESS_POLICY, "strict"},
               {LAGHU_SERVICE_SETTING_PURGE_TOKEN_FILE, "/tmp/token"},
               {LAGHU_SERVICE_SETTING_CACHE_FLUSH_FILE, "/tmp/flush"},
               {LAGHU_SERVICE_SETTING_PURGE_ALLOW, "192.0.2.0/24"},
               {LAGHU_SERVICE_SETTING_TRUSTED_PROXY, "2001:db8::/32"}};
  laghu_service_diagnostic diagnostic;
  size_t index;
  for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    laghu_service_config config;
    laghu_service_config_init(&config);
    assert(laghu_service_config_apply(&config, cases[index].setting, cases[index].value, &diagnostic));
    if (!laghu_service_setting_describe(cases[index].setting)->repeatable)
      assert(!laghu_service_config_apply(&config, cases[index].setting, cases[index].value, &diagnostic));
  }
  {
    laghu_service_config config;
    laghu_service_config_init(&config);
    assert(laghu_service_config_apply_pair(&config, LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP, "https://assets.example/", TEST_SOURCE_ROOT, &diagnostic));
    assert(
        !laghu_service_config_apply_pair(&config, LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP, "https://assets.example/", TEST_SOURCE_ROOT, &diagnostic));
  }
}

static void test_apply_and_merge(void) {
  laghu_service_config parent, child, merged;
  laghu_service_diagnostic diagnostic;
  laghu_service_config_init(&parent);
  laghu_service_config_init(&child);
  assert(laghu_service_config_apply(&parent, LAGHU_SERVICE_SETTING_FILE_CACHE_SIZE, "2m", &diagnostic));
  assert(laghu_service_config_apply(&parent, LAGHU_SERVICE_SETTING_METRICS, "on", &diagnostic));
  assert(laghu_service_config_apply(&parent, LAGHU_SERVICE_SETTING_HTML_REFRESH_QUEUE, "/tmp/parent-html-refresh.queue", &diagnostic));
  assert(laghu_service_config_apply(&parent, LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_QUEUE, "/tmp/parent-chrome.queue", &diagnostic));
  assert(laghu_service_config_apply(&parent, LAGHU_SERVICE_SETTING_LOAD_FROM_FILE, "mapped", &diagnostic));
  assert(laghu_service_config_apply_pair(&parent, LAGHU_SERVICE_SETTING_FILE_SOURCE_MAP, "https://assets.example/", TEST_SOURCE_ROOT, &diagnostic));
  assert(laghu_service_config_apply(&child, LAGHU_SERVICE_SETTING_FILE_CACHE_SIZE, "4m", &diagnostic));
  assert(laghu_service_config_apply(&child, LAGHU_SERVICE_SETTING_READINESS_POLICY, "strict", &diagnostic));
  assert(laghu_service_config_apply(&child, LAGHU_SERVICE_SETTING_HTML_REFRESH_QUEUE, "/tmp/child-html-refresh.queue", &diagnostic));
  assert(laghu_service_config_merge(&merged, &parent, &child, &diagnostic));
  assert(merged.cache_limits.size_limit == 4U * 1024U * 1024U);
  assert(merged.metrics);
  assert(merged.readiness_strict);
  assert(strcmp(merged.html_refresh_queue, "/tmp/child-html-refresh.queue") == 0);
  assert(strcmp(merged.chrome_analysis_queue, "/tmp/parent-chrome.queue") == 0);
  assert(merged.source_policy.mapping_count == 1U);
  assert(laghu_service_config_apply(&child, LAGHU_SERVICE_SETTING_METRICS, "off", &diagnostic));
  assert(laghu_service_config_merge(&merged, &parent, &child, &diagnostic));
  assert(!merged.metrics);
  assert(laghu_service_config_merge(&child, &parent, &child, &diagnostic));
  assert(child.cache_limits.size_limit == 4U * 1024U * 1024U);
  assert(!child.metrics);
  assert(!laghu_service_config_apply(&child, LAGHU_SERVICE_SETTING_FILE_CACHE_SIZE, "8m", &diagnostic));
  assert(diagnostic.code == LAGHU_SERVICE_DIAGNOSTIC_DUPLICATE);
  assert(!laghu_service_config_apply(&child, LAGHU_SERVICE_SETTING_RUM_STORE_TIMEOUT, "9", &diagnostic));
  assert(diagnostic.code == LAGHU_SERVICE_DIAGNOSTIC_RANGE);

  laghu_service_config_init(&parent);
  laghu_service_config_init(&child);
  assert(laghu_service_config_apply(&parent, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND, TEST_PARENT_CACHE_URI, &diagnostic));
  assert(laghu_service_config_apply(&child, LAGHU_SERVICE_SETTING_IMAGE_CACHE, TEST_CHILD_CACHE_ROOT, &diagnostic));
  assert(laghu_service_config_merge(&merged, &parent, &child, &diagnostic));
  assert(strcmp(merged.file_cache_backend, "") == 0);
  assert(strcmp(merged.image_cache, TEST_CHILD_CACHE_ROOT) == 0);
  assert(laghu_service_config_finalize(&merged, &(laghu_service_finalize_options){0}, &diagnostic));

  laghu_service_config_init(&parent);
  laghu_service_config_init(&child);
  assert(laghu_service_config_apply(&parent, LAGHU_SERVICE_SETTING_IMAGE_CACHE, TEST_PARENT_CACHE_ROOT, &diagnostic));
  assert(laghu_service_config_apply(&child, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND, TEST_CHILD_CACHE_URI, &diagnostic));
  assert(laghu_service_config_merge(&merged, &parent, &child, &diagnostic));
  assert(strcmp(merged.image_cache, "") == 0);
  assert(strcmp(merged.file_cache_backend, TEST_CHILD_CACHE_URI) == 0);
  assert(laghu_service_config_finalize(&merged, &(laghu_service_finalize_options){0}, &diagnostic));
}

static void test_cidr_fixtures(void) {
  static const char *const valid[] = {
      "0.0.0.0/0",           "192.0.2.0/24", "255.255.255.255/32", "::/0", "::1/128", "2001:db8::/32", "2001:db8::1/128", "2001:db8:0:0:0:0:0:1/128",
      "::ffff:192.0.2.0/120"};
  static const char *const invalid[] = {"1.2.3/24",         "1.2.3.4.5/32",    "256.0.0.0/8",      "01.2.3.4/32",           "192.0.2.0/33",
                                        "192.0.2.0/24 ",    "2001:db8:::1/64", "1:2:3:4:5:6:7/64", "1:2:3:4:5:6:7:8:9/128", "12345::/64",
                                        "fe80::1%zone/128", " 2001:db8::/32",  "2001:db8:: /32",   "2001:db8::/129"};
  size_t index;
  for (index = 0U; index < sizeof(valid) / sizeof(valid[0]); ++index) {
    laghu_service_cidr cidr;
    assert(laghu_service_cidr_parse(valid[index], &cidr));
    {
      char address[46U];
      const char *slash = strrchr(valid[index], '/');
      unsigned char expected[16] = {0};
      size_t length = (size_t)(slash - valid[index]);
      int family = strchr(valid[index], ':') == NULL ? AF_INET : AF_INET6;
      assert(length < sizeof(address));
      memcpy(address, valid[index], length);
      address[length] = '\0';
      assert(inet_pton(family, address, expected) == 1);
      assert(memcmp(cidr.address, expected, family == AF_INET ? 4U : 16U) == 0);
    }
  }
  for (index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
    laghu_service_cidr cidr;
    assert(!laghu_service_cidr_parse(invalid[index], &cidr));
  }
}

static void test_cidrs_and_dependencies(void) {
  laghu_service_config config;
  laghu_service_diagnostic diagnostic;
  laghu_service_finalize_options options = {0};
  laghu_service_cidr cidr;
  unsigned char address[16] = {192U, 0U, 2U, 17U};
  laghu_service_config_init(&config);
  assert(laghu_service_config_apply(&config, LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_TIMEOUT, "1500", &diagnostic));
  assert(!laghu_service_config_finalize(&config, &options, &diagnostic));
  assert(diagnostic.code == LAGHU_SERVICE_DIAGNOSTIC_DEPENDENCY);
  laghu_service_config_init(&config);
  assert(laghu_service_config_apply(&config, LAGHU_SERVICE_SETTING_CHROME_ANALYSIS_QUEUE, "/tmp/chrome.queue", &diagnostic));
  assert(laghu_service_config_finalize(&config, &options, &diagnostic));
  assert(config.chrome_analysis_timeout_ms == 1500U);
  laghu_service_config_init(&config);
  assert(laghu_service_cidr_parse("192.0.2.0/24", &cidr));
  assert(laghu_service_cidr_matches(&cidr, address, LAGHU_SERVICE_CIDR_FAMILY_IPV4));
  assert(!laghu_service_cidr_matches(&cidr, address, LAGHU_SERVICE_CIDR_FAMILY_IPV6));
  assert(!laghu_service_cidr_parse("192.0.2.1/24", &cidr));
  assert(!laghu_service_cidr_parse("not-an-address/24", &cidr));
  assert(laghu_service_config_apply(&config, LAGHU_SERVICE_SETTING_METRICS, "on", &diagnostic));
  options.require_admin_authorization = true;
  assert(!laghu_service_config_finalize(&config, &options, &diagnostic));
  assert(diagnostic.code == LAGHU_SERVICE_DIAGNOSTIC_DEPENDENCY);
  assert(laghu_service_config_apply(&config, LAGHU_SERVICE_SETTING_PURGE_TOKEN_FILE, "/tmp/token", &diagnostic));
  assert(laghu_service_config_apply(&config, LAGHU_SERVICE_SETTING_PURGE_ALLOW, "192.0.2.0/24", &diagnostic));
  assert(laghu_service_config_finalize(&config, &options, &diagnostic));
  laghu_service_config_init(&config);
  {
    unsigned int index;
    char value[32U];
    for (index = 0U; index < LAGHU_SERVICE_CONFIG_MAX_CIDRS; ++index) {
      (void)snprintf(value, sizeof(value), "192.0.%u.0/24", index);
      assert(laghu_service_config_apply(&config, LAGHU_SERVICE_SETTING_TRUSTED_PROXY, value, &diagnostic));
    }
    assert(!laghu_service_config_apply(&config, LAGHU_SERVICE_SETTING_TRUSTED_PROXY, "198.51.100.0/24", &diagnostic));
    assert(diagnostic.code == LAGHU_SERVICE_DIAGNOSTIC_CAPACITY);
  }
}

static void test_cache_backend_normalization(void) {
  laghu_service_config config;
  laghu_service_diagnostic diagnostic;
  laghu_service_finalize_options options = {0};
  options.require_cache = true;
  laghu_service_config_init(&config);
  assert(laghu_service_config_apply(&config, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND, TEST_CACHE_URI, &diagnostic));
  assert(laghu_service_config_finalize(&config, &options, &diagnostic));
  assert(strcmp(config.image_cache, TEST_CACHE_ROOT) == 0);
  {
    laghu_service_config parent, child, merged;
    laghu_service_config_init(&parent);
    laghu_service_config_init(&child);
    assert(laghu_service_config_apply(&parent, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND, TEST_CACHE_URI, &diagnostic));
    assert(laghu_service_config_finalize(&parent, &options, &diagnostic));
    assert(laghu_service_config_merge(&merged, &parent, &child, &diagnostic));
    assert(merged.image_cache[0] == '\0');
    assert(laghu_service_config_validate(&merged, &options, &diagnostic));
    assert(strcmp(merged.image_cache, TEST_CACHE_ROOT) == 0);
  }
  laghu_service_config_init(&config);
  assert(laghu_service_config_apply(&config, LAGHU_SERVICE_SETTING_IMAGE_CACHE, TEST_CACHE_ROOT, &diagnostic));
  assert(laghu_service_config_finalize(&config, &options, &diagnostic));
  assert(strcmp(config.image_cache, TEST_CACHE_ROOT) == 0);
  laghu_service_config_init(&config);
  assert(laghu_service_config_apply(&config, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND, TEST_CACHE_URI, &diagnostic));
  assert(laghu_service_config_apply(&config, LAGHU_SERVICE_SETTING_IMAGE_CACHE, TEST_CHILD_CACHE_ROOT, &diagnostic));
  assert(!laghu_service_config_finalize(&config, &options, &diagnostic));
  assert(diagnostic.code == LAGHU_SERVICE_DIAGNOSTIC_CONFLICT);
  laghu_service_config_init(&config);
  assert(laghu_service_config_apply(&config, LAGHU_SERVICE_SETTING_FILE_CACHE_BACKEND, "redis://cache.example", &diagnostic));
  assert(!laghu_service_config_finalize(&config, &options, &diagnostic));
  assert(diagnostic.code == LAGHU_SERVICE_DIAGNOSTIC_FORMAT);
}

static void test_prepared_resources(void) {
  laghu_service_config parent, child, merged;
  laghu_service_diagnostic diagnostic;
  laghu_service_finalize_options options = {0};
  laghu_service_config_init(&parent);
  assert(laghu_service_config_prepare_resources(&parent, &diagnostic));
  assert(laghu_service_config_prepare_resources(&parent, &diagnostic));
  assert(laghu_service_config_apply(&parent, LAGHU_SERVICE_SETTING_JAVASCRIPT_OBSERVATION_CONFIG, "/missing/laghu-observations", &diagnostic));
  parent.owned_font_providers = calloc(1U, sizeof(*parent.owned_font_providers));
  parent.font_providers = parent.owned_font_providers;
  assert(parent.font_providers != NULL);
  parent.font_providers->count = 1U;
  assert(!laghu_service_config_prepare_resources(&parent, &diagnostic));
  assert(diagnostic.code == LAGHU_SERVICE_DIAGNOSTIC_IO);
  assert(parent.font_providers == NULL);
  assert(parent.javascript_observations == NULL);
  assert(!laghu_service_config_finalize(&parent, &options, &diagnostic));
  assert(diagnostic.code == LAGHU_SERVICE_DIAGNOSTIC_IO);

  laghu_service_config_init(&parent);
  assert(laghu_service_config_apply(&parent, LAGHU_SERVICE_SETTING_ASSET_OFFLOAD_CONFIG, "/missing/laghu-assets", &diagnostic));
  assert(!laghu_service_config_validate(&parent, &options, &diagnostic));
  assert(diagnostic.code == LAGHU_SERVICE_DIAGNOSTIC_IO);

  laghu_service_config_init(&parent);
  laghu_service_config_init(&child);
  assert(laghu_service_config_apply(&parent, LAGHU_SERVICE_SETTING_JAVASCRIPT_DEFER_CONFIG, "/prepared/parent", &diagnostic));
  parent.owned_javascript_defer = calloc(1U, sizeof(*parent.owned_javascript_defer));
  parent.javascript_defer = parent.owned_javascript_defer;
  assert(parent.javascript_defer != NULL);
  parent.javascript_defer->count = 3U;
  assert(laghu_service_config_merge(&merged, &parent, &child, &diagnostic));
  assert(merged.javascript_defer == parent.javascript_defer);
  assert(merged.owned_javascript_defer == NULL);
  assert(merged.javascript_defer->count == 3U);
  assert(laghu_service_config_apply(&child, LAGHU_SERVICE_SETTING_JAVASCRIPT_DEFER_CONFIG, "/prepared/child", &diagnostic));
  child.owned_javascript_defer = calloc(1U, sizeof(*child.owned_javascript_defer));
  child.javascript_defer = child.owned_javascript_defer;
  assert(child.javascript_defer != NULL);
  child.javascript_defer->count = 2U;
  assert(laghu_service_config_merge(&merged, &parent, &child, &diagnostic));
  assert(merged.javascript_defer == child.javascript_defer);
  assert(merged.owned_javascript_defer == NULL);
  assert(merged.javascript_defer->count == 2U);
  assert(laghu_service_config_merge(&child, &parent, &child, &diagnostic));
  assert(child.javascript_defer != NULL && child.owned_javascript_defer == child.javascript_defer);
  assert(!laghu_service_config_merge(&parent, &parent, &child, &diagnostic));
  assert(diagnostic.code == LAGHU_SERVICE_DIAGNOSTIC_ARGUMENT);
  laghu_service_config_dispose(&merged);
  laghu_service_config_dispose(&child);
  laghu_service_config_dispose(&parent);
}

int main(void) {
  test_descriptors();
  test_config_size();
  test_descriptor_matrix();
  test_apply_and_merge();
  test_cidr_fixtures();
  test_cidrs_and_dependencies();
  test_cache_backend_normalization();
  test_prepared_resources();
  return 0;
}
