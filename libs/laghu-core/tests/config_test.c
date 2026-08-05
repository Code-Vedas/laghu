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

  laghu_config_init(&config);
  assert(laghu_config_setting_apply(&config,
                                    LAGHU_CONFIG_SETTING_ALLOW_RESOURCES,
                                    "/assets/*", error, sizeof(error)));
  assert(!laghu_config_setting_apply(&config,
                                     LAGHU_CONFIG_SETTING_DISALLOW_RESOURCES,
                                     "/assets/*", error, sizeof(error)));
  return 0;
}
