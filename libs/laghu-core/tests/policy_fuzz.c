// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/core.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  laghu_config config;
  laghu_policy policy;
  char *value;
  if (size > LAGHU_QUERY_OVERRIDE_SIZE * 2U) return 0;
  value = malloc(size + 1U);
  if (value == NULL) return 0;
  memcpy(value, data, size);
  value[size] = '\0';
  laghu_config_init(&config);
  config.mode = LAGHU_MODE_ON;
  config.preset = LAGHU_PRESET_BALANCED;
  config.query_filter_overrides = LAGHU_MODE_ON;
  (void)laghu_resource_pattern_valid(value);
  (void)laghu_resource_rule_add(&config, true, value);
  (void)laghu_resource_allowed(&config, value);
  (void)laghu_vary_supported(value);
  (void)laghu_apply_query_filter_overrides(&config, value, &policy, NULL, NULL);
  free(value);
  return 0;
}
