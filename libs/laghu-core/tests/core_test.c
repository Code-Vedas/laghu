#include "laghu/core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static laghu_config enabled_config(void) {
  laghu_config config;

  laghu_config_init(&config);
  config.mode = LAGHU_MODE_ON;
  config.preset = LAGHU_PRESET_BALANCED;
  return config;
}

static laghu_response html_response(void) {
  laghu_response response = {
      .status = 200U,
      .content_type = "text/html; charset=utf-8",
      .cache_control = "public, max-age=60",
      .has_authorization = false,
  };

  return response;
}

static void test_config_defaults_and_inheritance(void) {
  laghu_config parent;
  laghu_config child;
  laghu_config result;

  laghu_config_init(&parent);
  laghu_config_init(&child);
  laghu_config_merge(&result, &parent, &child);
  assert(result.mode == LAGHU_MODE_OFF);
  assert(result.preset == LAGHU_PRESET_BALANCED);

  parent.mode = LAGHU_MODE_ON;
  parent.preset = LAGHU_PRESET_SAFE;
  child.preset = LAGHU_PRESET_STATIC;
  laghu_config_merge(&result, &parent, &child);
  assert(result.mode == LAGHU_MODE_ON);
  assert(result.preset == LAGHU_PRESET_STATIC);
}

static void test_preset_parser(void) {
  laghu_preset preset = LAGHU_PRESET_UNSET;

  assert(laghu_parse_preset("safe", &preset));
  assert(preset == LAGHU_PRESET_SAFE);
  assert(strcmp(laghu_preset_name(preset), "safe") == 0);
  assert(!laghu_parse_preset("unknown", &preset));
}

static void test_decisions(void) {
  laghu_config config = enabled_config();
  laghu_response response = html_response();

  assert(laghu_decide(&config, &response) == LAGHU_DECISION_PASS);

  response.status = 304U;
  assert(laghu_decide(&config, &response) == LAGHU_DECISION_BYPASS_STATUS);
  response.status = 200U;

  response.has_authorization = true;
  assert(laghu_decide(&config, &response) == LAGHU_DECISION_BYPASS_AUTHORIZED);
  response.has_authorization = false;

  response.cache_control = "Public, NO-STORE";
  assert(laghu_decide(&config, &response) == LAGHU_DECISION_BYPASS_PRIVATE);
  response.cache_control = NULL;

  response.content_type = "application/json";
  assert(laghu_decide(&config, &response) ==
         LAGHU_DECISION_BYPASS_CONTENT_TYPE);

  config.mode = LAGHU_MODE_OFF;
  assert(laghu_decide(&config, &response) == LAGHU_DECISION_BYPASS_DISABLED);
}

int main(void) {
  test_config_defaults_and_inheritance();
  test_preset_parser();
  test_decisions();

  puts("laghu_core_test: all tests passed");
  return 0;
}
