// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/config.h"

#include <assert.h>
#include <string.h>

int main(void) {
  laghu_config config;
  char error[128U];

  laghu_config_init(&config);
  assert(laghu_config_setting_find("preset") == LAGHU_CONFIG_SETTING_PRESET);
  assert(laghu_config_setting_find("Preset") == LAGHU_CONFIG_SETTING_PRESET);
  assert(laghu_config_setting_find("--rewrite-level") ==
         LAGHU_CONFIG_SETTING_REWRITE_LEVEL);
  assert(laghu_config_setting_find("HtmlCacheOrigin") ==
         LAGHU_CONFIG_SETTING_HTML_CACHE_ORIGIN);
  assert(laghu_config_setting_find("--not-a-setting") ==
         LAGHU_CONFIG_SETTING_UNKNOWN);

  assert(laghu_config_setting_apply(&config, LAGHU_CONFIG_SETTING_PRESET,
                                    "balanced", error, sizeof(error)));
  assert(!laghu_config_setting_apply(&config, LAGHU_CONFIG_SETTING_PRESET,
                                     "safe", error, sizeof(error)));
  assert(strstr(error, "conflicting") != NULL);

  laghu_config_init(&config);
  assert(laghu_config_setting_apply(&config,
                                    LAGHU_CONFIG_SETTING_TRANSFORM_MEMORY_LIMIT,
                                    "32m", error, sizeof(error)));
  assert(config.transform_memory_limit == 32U * 1024U * 1024U);
  assert(!laghu_config_setting_apply(&config,
                                     LAGHU_CONFIG_SETTING_TRANSFORM_DEADLINE_MS,
                                     "4", error, sizeof(error)));
  assert(laghu_config_setting_apply(&config,
                                    LAGHU_CONFIG_SETTING_TRANSFORM_DEADLINE_MS,
                                    "50", error, sizeof(error)));

  assert(laghu_config_setting_apply(&config,
                                    LAGHU_CONFIG_SETTING_HTML_CACHE_ORIGIN,
                                    "https://origin.example", error,
                                    sizeof(error)));
  assert(laghu_config_setting_apply(&config,
                                    LAGHU_CONFIG_SETTING_HTML_CACHE_TTL, "30",
                                    error, sizeof(error)));
  assert(laghu_config_setting_apply(
      &config, LAGHU_CONFIG_SETTING_HTML_CACHE_STALE_TTL, "300", error,
      sizeof(error)));
  assert(config.html_cache_ttl == 30U);
  assert(config.html_cache_stale_ttl == 300U);
  assert(!laghu_config_setting_apply(
      &config, LAGHU_CONFIG_SETTING_HTML_CACHE_ORIGIN,
      "https://other.example", error, sizeof(error)));
  laghu_config_init(&config);
  assert(!laghu_config_setting_apply(&config,
                                     LAGHU_CONFIG_SETTING_HTML_CACHE_ORIGIN,
                                     "http://origin.example", error,
                                     sizeof(error)));
  assert(!laghu_config_setting_apply(&config,
                                     LAGHU_CONFIG_SETTING_HTML_CACHE_ORIGIN,
                                     "https://origin.example/path", error,
                                     sizeof(error)));
  assert(!laghu_config_setting_apply(&config,
                                     LAGHU_CONFIG_SETTING_HTML_CACHE_TTL, "0",
                                     error, sizeof(error)));
  assert(!laghu_config_setting_apply(
      &config, LAGHU_CONFIG_SETTING_HTML_CACHE_TTL, "3601", error,
      sizeof(error)));
  assert(!laghu_config_setting_apply(
      &config, LAGHU_CONFIG_SETTING_HTML_CACHE_STALE_TTL, "86401", error,
      sizeof(error)));

  laghu_config_init(&config);
  assert(laghu_config_setting_apply(&config,
                                    LAGHU_CONFIG_SETTING_ALLOW_RESOURCES,
                                    "/assets/*", error, sizeof(error)));
  assert(!laghu_config_setting_apply(&config,
                                     LAGHU_CONFIG_SETTING_DISALLOW_RESOURCES,
                                     "/assets/*", error, sizeof(error)));

  laghu_config_init(&config);
  assert(laghu_config_setting_apply(&config, LAGHU_CONFIG_SETTING_DOMAIN,
                                    "https://cdn.example", error,
                                    sizeof(error)));
  assert(laghu_config_setting_apply_pair(
      &config, LAGHU_CONFIG_SETTING_MAP_REWRITE_DOMAIN,
      "https://cdn.example", "https://origin.example", error,
      sizeof(error)));
  assert(laghu_config_setting_apply_pair(
      &config, LAGHU_CONFIG_SETTING_SHARD_DOMAIN, "https://cdn.example",
      "https://one.example,https://two.example", error, sizeof(error)));
  assert(config.domain_policy.mapping_count == 1U);
  assert(config.domain_policy.group_count == 1U);
  assert(config.domain_policy.groups[0].shard_count == 2U);
  assert(laghu_domain_policy_validate(&config.domain_policy));
  assert(!laghu_config_setting_apply_pair(
      &config, LAGHU_CONFIG_SETTING_MAP_PROXY_DOMAIN, "https://cdn.example",
      "https://origin.example", error, sizeof(error)));
  return 0;
}
