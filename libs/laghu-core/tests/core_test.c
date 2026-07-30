// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static laghu_config enabled_config(void) {
  laghu_config config;

  laghu_config_init(&config);
  config.mode = LAGHU_MODE_ON;
  config.preset = LAGHU_PRESET_BALANCED;
  config.rewrite_level = LAGHU_REWRITE_LEVEL_UNSET;
  config.allow_api = LAGHU_MODE_OFF;
  return config;
}

static laghu_response html_response(void) {
  laghu_response response = {
      .status = 200U,
      .request_path = "/",
      .content_type = "text/html; charset=utf-8",
      .cache_control = "public, max-age=60",
      .has_authorization = false,
  };

  return response;
}

static void assert_policy(laghu_preset preset, uint32_t expected_filters,
                          laghu_risk_level expected_risk, bool allow_lossy,
                          bool allow_structural_rewrite,
                          bool allow_resource_inlining,
                          bool allow_script_reordering) {
  laghu_policy policy;

  assert(laghu_resolve_policy(preset, &policy));
  assert(policy.preset == preset);
  assert(policy.rewrite_level == LAGHU_REWRITE_LEVEL_UNSET);
  assert(policy.filter_families == expected_filters);
  assert(policy.risk_level == expected_risk);
  assert(policy.allow_lossy == allow_lossy);
  assert(policy.allow_structural_rewrite == allow_structural_rewrite);
  assert(policy.allow_resource_inlining == allow_resource_inlining);
  assert(policy.allow_script_reordering == allow_script_reordering);
  assert(!policy.allow_experimental);
  assert(policy.image_quality ==
         (preset == LAGHU_PRESET_SAFE        ? 0U
          : preset == LAGHU_PRESET_ECOMMERCE ? 85U
          : preset == LAGHU_PRESET_BALANCED || preset == LAGHU_PRESET_BLOG
              ? 82U
              : 75U));
}

static void assert_rewrite_policy(
    laghu_rewrite_level rewrite_level, uint32_t expected_filters,
    laghu_risk_level expected_risk, bool allow_lossy,
    bool allow_structural_rewrite, bool allow_resource_inlining,
    bool allow_script_reordering, bool allow_experimental) {
  laghu_policy policy;

  assert(laghu_resolve_rewrite_level(rewrite_level, &policy));
  assert(policy.preset == LAGHU_PRESET_UNSET);
  assert(policy.rewrite_level == rewrite_level);
  assert(policy.filter_families == expected_filters);
  assert(policy.risk_level == expected_risk);
  assert(policy.allow_lossy == allow_lossy);
  assert(policy.allow_structural_rewrite == allow_structural_rewrite);
  assert(policy.allow_resource_inlining == allow_resource_inlining);
  assert(policy.allow_script_reordering == allow_script_reordering);
  assert(policy.allow_experimental == allow_experimental);
  assert(policy.image_quality ==
         (rewrite_level == LAGHU_REWRITE_LEVEL_PASSTHROUGH ? 0U
          : rewrite_level == LAGHU_REWRITE_LEVEL_CORE ||
                  rewrite_level == LAGHU_REWRITE_LEVEL_BANDWIDTH
              ? 82U
              : 75U));
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
  assert(result.rewrite_level == LAGHU_REWRITE_LEVEL_UNSET);
  assert(result.allow_api == LAGHU_MODE_OFF);
  assert(result.image_quality == LAGHU_IMAGE_QUALITY_UNSET);
  assert(result.image_beacon == LAGHU_MODE_OFF);
  assert(result.critical_css_beacon == LAGHU_MODE_OFF);
  assert(result.instrumentation_beacon == LAGHU_MODE_OFF);
  assert(result.instrumentation_sample_rate ==
         LAGHU_INSTRUMENTATION_SAMPLE_RATE_DEFAULT);
  assert(result.image_inline_limit == LAGHU_IMAGE_INLINE_LIMIT_DEFAULT);
  assert(result.image_metadata_limit == LAGHU_IMAGE_METADATA_LIMIT_DEFAULT);
  assert(result.image_metadata_ttl == LAGHU_IMAGE_METADATA_TTL_DEFAULT);
  assert(result.css_inline_limit == LAGHU_CSS_INLINE_LIMIT_DEFAULT);
  assert(result.css_outline_threshold == LAGHU_CSS_OUTLINE_THRESHOLD_DEFAULT);
  assert(result.javascript_inline_limit ==
         LAGHU_JAVASCRIPT_INLINE_LIMIT_DEFAULT);
  assert(result.javascript_outline_threshold ==
         LAGHU_JAVASCRIPT_OUTLINE_THRESHOLD_DEFAULT);

  parent.mode = LAGHU_MODE_ON;
  parent.preset = LAGHU_PRESET_SAFE;
  parent.allow_api = LAGHU_MODE_ON;
  parent.image_quality = 91U;
  parent.image_beacon = LAGHU_MODE_ON;
  parent.critical_css_beacon = LAGHU_MODE_ON;
  parent.instrumentation_beacon = LAGHU_MODE_ON;
  parent.instrumentation_sample_rate = 25U;
  parent.image_inline_limit = 4096U;
  parent.image_metadata_limit = 5000U;
  parent.image_metadata_ttl = 86400U;
  parent.css_inline_limit = 8192U;
  parent.css_outline_threshold = 16384U;
  parent.javascript_inline_limit = 4096U;
  parent.javascript_outline_threshold = 32768U;
  child.preset = LAGHU_PRESET_STATIC;
  laghu_config_merge(&result, &parent, &child);
  assert(result.mode == LAGHU_MODE_ON);
  assert(result.preset == LAGHU_PRESET_STATIC);
  assert(result.rewrite_level == LAGHU_REWRITE_LEVEL_UNSET);
  assert(result.allow_api == LAGHU_MODE_ON);
  assert(result.image_quality == 91U);
  assert(result.image_beacon == LAGHU_MODE_ON);
  assert(result.critical_css_beacon == LAGHU_MODE_ON);
  assert(result.instrumentation_beacon == LAGHU_MODE_ON);
  assert(result.instrumentation_sample_rate == 25U);
  assert(result.image_inline_limit == 4096U);
  assert(result.image_metadata_limit == 5000U);
  assert(result.image_metadata_ttl == 86400U);
  assert(result.css_inline_limit == 8192U);
  assert(result.css_outline_threshold == 16384U);
  assert(result.javascript_inline_limit == 4096U);
  assert(result.javascript_outline_threshold == 32768U);

  child.mode = LAGHU_MODE_OFF;
  child.allow_api = LAGHU_MODE_OFF;
  laghu_config_merge(&result, &parent, &child);
  assert(result.mode == LAGHU_MODE_OFF);
  assert(result.preset == LAGHU_PRESET_STATIC);
  assert(result.rewrite_level == LAGHU_REWRITE_LEVEL_UNSET);
  assert(result.allow_api == LAGHU_MODE_OFF);
  assert(result.image_quality == 91U);

  laghu_config_init(&child);
  child.rewrite_level = LAGHU_REWRITE_LEVEL_BANDWIDTH;
  laghu_config_merge(&result, &parent, &child);
  assert(result.preset == LAGHU_PRESET_UNSET);
  assert(result.rewrite_level == LAGHU_REWRITE_LEVEL_BANDWIDTH);
  assert(result.allow_api == LAGHU_MODE_ON);
  assert(result.image_quality == 91U);

  parent.preset = LAGHU_PRESET_UNSET;
  parent.rewrite_level = LAGHU_REWRITE_LEVEL_CORE;
  laghu_config_init(&child);
  child.rewrite_level = LAGHU_REWRITE_LEVEL_ALL;
  laghu_config_merge(&result, &parent, &child);
  assert(result.preset == LAGHU_PRESET_UNSET);
  assert(result.rewrite_level == LAGHU_REWRITE_LEVEL_ALL);

  laghu_config_init(&child);
  child.preset = LAGHU_PRESET_SAFE;
  laghu_config_merge(&result, &parent, &child);
  assert(result.preset == LAGHU_PRESET_SAFE);
  assert(result.rewrite_level == LAGHU_REWRITE_LEVEL_UNSET);
}

static void test_preset_parser(void) {
  static const struct {
    const char *name;
    laghu_preset preset;
  } cases[] = {
      {"safe", LAGHU_PRESET_SAFE},
      {"balanced", LAGHU_PRESET_BALANCED},
      {"aggressive", LAGHU_PRESET_AGGRESSIVE},
      {"ecommerce", LAGHU_PRESET_ECOMMERCE},
      {"blog", LAGHU_PRESET_BLOG},
      {"static", LAGHU_PRESET_STATIC},
  };
  laghu_preset preset = LAGHU_PRESET_UNSET;
  size_t index;

  for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    assert(laghu_parse_preset(cases[index].name, &preset));
    assert(preset == cases[index].preset);
    assert(strcmp(laghu_preset_name(preset), cases[index].name) == 0);
  }

  assert(!laghu_parse_preset("unknown", &preset));
  assert(!laghu_parse_preset(NULL, &preset));
  assert(!laghu_parse_preset("safe", NULL));
  assert(strcmp(laghu_preset_name(LAGHU_PRESET_UNSET), "unset") == 0);
}

static void test_preset_policies(void) {
  const uint32_t safe = LAGHU_FILTER_IMAGE_LOSSLESS |
                        LAGHU_FILTER_IMAGE_METADATA |
                        LAGHU_FILTER_IMAGE_DIMENSIONS;
  const uint32_t balanced =
      safe | LAGHU_FILTER_IMAGE_MODERN | LAGHU_FILTER_IMAGE_RESPONSIVE |
      LAGHU_FILTER_IMAGE_LAZYLOAD | LAGHU_FILTER_HTML_MINIFY |
      LAGHU_FILTER_CSS_MINIFY | LAGHU_FILTER_JAVASCRIPT_MINIFY |
      LAGHU_FILTER_RESOURCE_HINTS | LAGHU_FILTER_CACHE_EXTENSION;
  const uint32_t aggressive =
      balanced | LAGHU_FILTER_RESOURCE_COMBINE | LAGHU_FILTER_RESOURCE_INLINE |
      LAGHU_FILTER_CRITICAL_CSS | LAGHU_FILTER_JAVASCRIPT_DEFER;
  const uint32_t ecommerce =
      safe | LAGHU_FILTER_IMAGE_MODERN | LAGHU_FILTER_IMAGE_RESPONSIVE |
      LAGHU_FILTER_IMAGE_LAZYLOAD | LAGHU_FILTER_CSS_MINIFY |
      LAGHU_FILTER_RESOURCE_HINTS | LAGHU_FILTER_CACHE_EXTENSION;
  const uint32_t blog = balanced | LAGHU_FILTER_RESOURCE_INLINE |
                        LAGHU_FILTER_CRITICAL_CSS |
                        LAGHU_FILTER_JAVASCRIPT_DEFER;

  assert_policy(LAGHU_PRESET_SAFE, safe, LAGHU_RISK_CONSERVATIVE, false, false,
                false, false);
  assert_policy(LAGHU_PRESET_BALANCED, balanced, LAGHU_RISK_MODERATE, true,
                true, false, false);
  assert_policy(LAGHU_PRESET_AGGRESSIVE, aggressive, LAGHU_RISK_EXPANSIVE, true,
                true, true, true);
  assert_policy(LAGHU_PRESET_ECOMMERCE, ecommerce, LAGHU_RISK_CONSERVATIVE,
                true, true, false, false);
  assert_policy(LAGHU_PRESET_BLOG, blog, LAGHU_RISK_MODERATE, true, true, true,
                true);
  assert_policy(LAGHU_PRESET_STATIC, aggressive | LAGHU_FILTER_IMMUTABLE_CACHE,
                LAGHU_RISK_EXPANSIVE, true, true, true, true);

  assert(!laghu_resolve_policy(LAGHU_PRESET_UNSET, &(laghu_policy){0}));
  assert(!laghu_resolve_policy(LAGHU_PRESET_SAFE, NULL));
}

static void test_rewrite_level_parser_and_policies(void) {
  static const struct {
    const char *name;
    laghu_rewrite_level rewrite_level;
  } cases[] = {
      {"passthrough", LAGHU_REWRITE_LEVEL_PASSTHROUGH},
      {"core", LAGHU_REWRITE_LEVEL_CORE},
      {"bandwidth", LAGHU_REWRITE_LEVEL_BANDWIDTH},
      {"all", LAGHU_REWRITE_LEVEL_ALL},
      {"experimental", LAGHU_REWRITE_LEVEL_EXPERIMENTAL},
  };
  const uint32_t safe = LAGHU_FILTER_IMAGE_LOSSLESS |
                        LAGHU_FILTER_IMAGE_METADATA |
                        LAGHU_FILTER_IMAGE_DIMENSIONS;
  const uint32_t balanced =
      safe | LAGHU_FILTER_IMAGE_MODERN | LAGHU_FILTER_IMAGE_RESPONSIVE |
      LAGHU_FILTER_IMAGE_LAZYLOAD | LAGHU_FILTER_HTML_MINIFY |
      LAGHU_FILTER_CSS_MINIFY | LAGHU_FILTER_JAVASCRIPT_MINIFY |
      LAGHU_FILTER_RESOURCE_HINTS | LAGHU_FILTER_CACHE_EXTENSION;
  const uint32_t bandwidth =
      LAGHU_FILTER_IMAGE_LOSSLESS | LAGHU_FILTER_IMAGE_METADATA |
      LAGHU_FILTER_IMAGE_MODERN | LAGHU_FILTER_HTML_MINIFY |
      LAGHU_FILTER_CSS_MINIFY | LAGHU_FILTER_JAVASCRIPT_MINIFY |
      LAGHU_FILTER_CACHE_EXTENSION;
  const uint32_t all =
      balanced | LAGHU_FILTER_RESOURCE_COMBINE | LAGHU_FILTER_RESOURCE_INLINE |
      LAGHU_FILTER_CRITICAL_CSS | LAGHU_FILTER_JAVASCRIPT_DEFER |
      LAGHU_FILTER_IMMUTABLE_CACHE;
  laghu_rewrite_level rewrite_level = LAGHU_REWRITE_LEVEL_UNSET;
  laghu_config config = enabled_config();
  laghu_policy policy;
  size_t index;

  for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    assert(laghu_parse_rewrite_level(cases[index].name, &rewrite_level));
    assert(rewrite_level == cases[index].rewrite_level);
    assert(strcmp(laghu_rewrite_level_name(rewrite_level), cases[index].name) ==
           0);
  }

  assert(!laghu_parse_rewrite_level("unknown", &rewrite_level));
  assert(!laghu_parse_rewrite_level(NULL, &rewrite_level));
  assert(!laghu_parse_rewrite_level("core", NULL));
  assert(strcmp(laghu_rewrite_level_name(LAGHU_REWRITE_LEVEL_UNSET), "unset") ==
         0);

  assert_rewrite_policy(LAGHU_REWRITE_LEVEL_PASSTHROUGH, 0U,
                        LAGHU_RISK_CONSERVATIVE, false, false, false, false,
                        false);
  assert_rewrite_policy(LAGHU_REWRITE_LEVEL_CORE, balanced, LAGHU_RISK_MODERATE,
                        true, true, false, false, false);
  assert_rewrite_policy(LAGHU_REWRITE_LEVEL_BANDWIDTH, bandwidth,
                        LAGHU_RISK_MODERATE, true, false, false, false, false);
  assert_rewrite_policy(LAGHU_REWRITE_LEVEL_ALL, all, LAGHU_RISK_EXPANSIVE,
                        true, true, true, true, false);
  assert_rewrite_policy(LAGHU_REWRITE_LEVEL_EXPERIMENTAL, all,
                        LAGHU_RISK_EXPANSIVE, true, true, true, true, true);
  assert(!laghu_resolve_rewrite_level(LAGHU_REWRITE_LEVEL_UNSET, &policy));
  assert(!laghu_resolve_rewrite_level(LAGHU_REWRITE_LEVEL_CORE, NULL));

  assert(laghu_resolve_config_policy(&config, &policy));
  assert(policy.preset == LAGHU_PRESET_BALANCED);
  assert(policy.image_quality == 82U);
  config.image_quality = 91U;
  assert(laghu_resolve_config_policy(&config, &policy));
  assert(policy.image_quality == 91U);
  config.image_quality = 101U;
  assert(!laghu_resolve_config_policy(&config, &policy));
  config.image_quality = LAGHU_IMAGE_QUALITY_UNSET;
  config.rewrite_level = LAGHU_REWRITE_LEVEL_CORE;
  assert(!laghu_resolve_config_policy(&config, &policy));
  config.preset = LAGHU_PRESET_UNSET;
  assert(laghu_resolve_config_policy(&config, &policy));
  assert(policy.rewrite_level == LAGHU_REWRITE_LEVEL_CORE);
  config.rewrite_level = LAGHU_REWRITE_LEVEL_UNSET;
  assert(!laghu_resolve_config_policy(&config, &policy));
  assert(!laghu_resolve_config_policy(NULL, &policy));
  assert(!laghu_resolve_config_policy(&config, NULL));
}

static void test_decision_precedence(void) {
  laghu_config config = enabled_config();
  laghu_response response = html_response();

  assert(laghu_decide(&config, &response) == LAGHU_DECISION_PASS);
  assert(laghu_decide(NULL, &response) == LAGHU_DECISION_BYPASS_DISABLED);
  assert(laghu_decide(&config, NULL) == LAGHU_DECISION_BYPASS_ERROR);

  config.preset = LAGHU_PRESET_UNSET;
  assert(laghu_decide(&config, &response) == LAGHU_DECISION_BYPASS_ERROR);
  config.preset = LAGHU_PRESET_BALANCED;

  config.preset = LAGHU_PRESET_UNSET;
  config.rewrite_level = LAGHU_REWRITE_LEVEL_PASSTHROUGH;
  assert(laghu_decide(&config, NULL) == LAGHU_DECISION_BYPASS_PASSTHROUGH);
  assert(strcmp(laghu_decision_name(LAGHU_DECISION_BYPASS_PASSTHROUGH),
                "bypass-passthrough") == 0);
  config.rewrite_level = LAGHU_REWRITE_LEVEL_CORE;
  assert(laghu_decide(&config, &response) == LAGHU_DECISION_PASS);
  config.rewrite_level = LAGHU_REWRITE_LEVEL_UNSET;
  config.preset = LAGHU_PRESET_BALANCED;

  response.status = 304U;
  response.has_authorization = true;
  assert(laghu_decide(&config, &response) == LAGHU_DECISION_BYPASS_STATUS);
  response.status = 200U;

  response.request_path = "/api/private";
  assert(laghu_decide(&config, &response) == LAGHU_DECISION_BYPASS_AUTHORIZED);
  response.has_authorization = false;

  response.cache_control = "Public, NO-STORE";
  assert(laghu_decide(&config, &response) == LAGHU_DECISION_BYPASS_PRIVATE);
  response.cache_control = NULL;

  assert(laghu_decide(&config, &response) == LAGHU_DECISION_BYPASS_API);
  response.content_type = "application/json";
  assert(laghu_decide(&config, &response) == LAGHU_DECISION_BYPASS_API);

  config.allow_api = LAGHU_MODE_ON;
  assert(laghu_decide(&config, &response) ==
         LAGHU_DECISION_BYPASS_CONTENT_TYPE);

  config.mode = LAGHU_MODE_OFF;
  assert(laghu_decide(&config, NULL) == LAGHU_DECISION_BYPASS_DISABLED);

  assert(strcmp(laghu_decision_name(LAGHU_DECISION_BYPASS_API), "bypass-api") ==
         0);
  assert(strcmp(laghu_decision_name((laghu_decision)999), "bypass-error") == 0);
}

static void test_api_path_policy(void) {
  static const char *excluded[] = {"/api", "/api/", "/api/v1/products",
                                   "/graphql", "/graphql/query"};
  static const char *allowed[] = {
      NULL, "", "/", "/apiary", "/api-v1", "/API", "/graphql-ui", "/graphqled"};
  laghu_config config = enabled_config();
  laghu_response response = html_response();
  size_t index;

  for (index = 0U; index < sizeof(excluded) / sizeof(excluded[0]); ++index) {
    response.request_path = excluded[index];
    assert(laghu_decide(&config, &response) == LAGHU_DECISION_BYPASS_API);
  }

  for (index = 0U; index < sizeof(allowed) / sizeof(allowed[0]); ++index) {
    response.request_path = allowed[index];
    assert(laghu_decide(&config, &response) == LAGHU_DECISION_PASS);
  }

  config.allow_api = LAGHU_MODE_ON;
  response.request_path = "/api/v1/products";
  assert(laghu_decide(&config, &response) == LAGHU_DECISION_PASS);
}

static void test_rewrite_level_safety_precedence(void) {
  static const laghu_rewrite_level rewrite_levels[] = {
      LAGHU_REWRITE_LEVEL_CORE,
      LAGHU_REWRITE_LEVEL_BANDWIDTH,
      LAGHU_REWRITE_LEVEL_ALL,
      LAGHU_REWRITE_LEVEL_EXPERIMENTAL,
  };
  laghu_config config = enabled_config();
  size_t index;

  config.preset = LAGHU_PRESET_UNSET;
  for (index = 0U; index < sizeof(rewrite_levels) / sizeof(rewrite_levels[0]);
       ++index) {
    laghu_response response = html_response();

    config.rewrite_level = rewrite_levels[index];
    assert(laghu_decide(&config, &response) == LAGHU_DECISION_PASS);

    response.status = 304U;
    assert(laghu_decide(&config, &response) == LAGHU_DECISION_BYPASS_STATUS);
    response.status = 200U;

    response.has_authorization = true;
    assert(laghu_decide(&config, &response) ==
           LAGHU_DECISION_BYPASS_AUTHORIZED);
    response.has_authorization = false;

    response.cache_control = "private";
    assert(laghu_decide(&config, &response) == LAGHU_DECISION_BYPASS_PRIVATE);
    response.cache_control = NULL;

    response.request_path = "/api/v1";
    assert(laghu_decide(&config, &response) == LAGHU_DECISION_BYPASS_API);
    response.request_path = "/";

    response.content_type = "application/json";
    assert(laghu_decide(&config, &response) ==
           LAGHU_DECISION_BYPASS_CONTENT_TYPE);
  }
}

static void test_candidate_finalization(void) {
  static const unsigned char original_data[] = "original";
  static const unsigned char smaller_data[] = "tiny";
  static const unsigned char equal_data[] = "changed!";
  static const unsigned char larger_data[] = "larger than original";
  laghu_buffer original = {original_data, sizeof(original_data) - 1U};
  laghu_buffer smaller = {smaller_data, sizeof(smaller_data) - 1U};
  laghu_candidate_result result;

  result = laghu_finalize_candidate(original, smaller, true);
  assert(result.decision == LAGHU_CANDIDATE_ACCEPTED);
  assert(result.original.data == original.data);
  assert(result.original.length == original.length);
  assert(result.selected.data == smaller.data);
  assert(result.selected.length == smaller.length);

  result = laghu_finalize_candidate(original, original, true);
  assert(result.decision == LAGHU_CANDIDATE_REJECTED_IDENTICAL);
  assert(result.selected.data == original.data);

  result = laghu_finalize_candidate(
      original, (laghu_buffer){equal_data, sizeof(equal_data) - 1U}, true);
  assert(result.decision == LAGHU_CANDIDATE_REJECTED_NOT_SMALLER);
  assert(result.selected.data == original.data);

  result = laghu_finalize_candidate(
      original, (laghu_buffer){larger_data, sizeof(larger_data) - 1U}, true);
  assert(result.decision == LAGHU_CANDIDATE_REJECTED_NOT_SMALLER);
  assert(result.selected.data == original.data);

  result = laghu_finalize_candidate(original, (laghu_buffer){NULL, 1U}, true);
  assert(result.decision == LAGHU_CANDIDATE_REJECTED_INVALID);
  assert(result.selected.data == original.data);

  result = laghu_finalize_candidate(original, smaller, false);
  assert(result.decision == LAGHU_CANDIDATE_REJECTED_FAILED);
  assert(result.selected.data == original.data);

  result = laghu_finalize_candidate((laghu_buffer){NULL, 1U}, smaller, true);
  assert(result.decision == LAGHU_CANDIDATE_REJECTED_INVALID);
  assert(result.original.data == NULL);

  result = laghu_finalize_candidate(original, (laghu_buffer){NULL, 0U}, true);
  assert(result.decision == LAGHU_CANDIDATE_ACCEPTED);
  assert(result.selected.length == 0U);
  assert(result.original.data == original.data);
}

static void test_hashing(void) {
  static const unsigned char abc[] = "abc";
  static const unsigned char abd[] = "abd";
  static const unsigned char multi_block[] =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  char output[LAGHU_SHA256_HEX_SIZE];
  char key[LAGHU_SHA256_HEX_SIZE];
  char repeated_key[LAGHU_SHA256_HEX_SIZE];
  char changed_key[LAGHU_SHA256_HEX_SIZE];
  unsigned char overlapping_output[LAGHU_SHA256_HEX_SIZE] = "abc";
  laghu_policy policy;
  laghu_policy changed_policy;

  assert(laghu_sha256_hex((laghu_buffer){NULL, 0U}, output));
  assert(strcmp(output,
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b785"
                "2b855") == 0);

  assert(laghu_sha256_hex((laghu_buffer){abc, sizeof(abc) - 1U}, output));
  assert(strcmp(output,
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f200"
                "15ad") == 0);

  assert(laghu_sha256_hex((laghu_buffer){overlapping_output, 3U},
                          (char *)overlapping_output));
  assert(strcmp((char *)overlapping_output,
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f200"
                "15ad") == 0);

  assert(laghu_sha256_hex((laghu_buffer){multi_block, sizeof(multi_block) - 1U},
                          output));
  assert(strcmp(output,
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db"
                "06c1") == 0);

  output[0] = 'x';
  assert(!laghu_sha256_hex((laghu_buffer){NULL, 1U}, output));
  assert(output[0] == '\0');
  assert(!laghu_sha256_hex((laghu_buffer){abc, sizeof(abc) - 1U}, NULL));

  assert(laghu_resolve_policy(LAGHU_PRESET_BALANCED, &policy));
  assert(
      laghu_variant_key((laghu_buffer){abc, sizeof(abc) - 1U}, &policy, key));
  assert(strcmp(key,
                "45470b0cb1b9412dcebb83da7ffc349d20b283263975677980f979622c194"
                "e97") == 0);

  memcpy(overlapping_output, abc, sizeof(abc));
  assert(laghu_variant_key((laghu_buffer){overlapping_output, 3U}, &policy,
                           (char *)overlapping_output));
  assert(strcmp((char *)overlapping_output, key) == 0);
  assert(laghu_variant_key((laghu_buffer){abc, sizeof(abc) - 1U}, &policy,
                           repeated_key));
  assert(strcmp(key, repeated_key) == 0);

  assert(laghu_variant_key((laghu_buffer){abd, sizeof(abd) - 1U}, &policy,
                           changed_key));
  assert(strcmp(key, changed_key) != 0);

  changed_policy = policy;
  changed_policy.allow_resource_inlining = true;
  assert(laghu_variant_key((laghu_buffer){abc, sizeof(abc) - 1U},
                           &changed_policy, changed_key));
  assert(strcmp(key, changed_key) != 0);

  assert(
      laghu_resolve_rewrite_level(LAGHU_REWRITE_LEVEL_CORE, &changed_policy));
  assert(laghu_variant_key((laghu_buffer){abc, sizeof(abc) - 1U},
                           &changed_policy, changed_key));
  assert(strcmp(key, changed_key) != 0);

  assert(laghu_resolve_rewrite_level(LAGHU_REWRITE_LEVEL_ALL, &changed_policy));
  assert(laghu_variant_key((laghu_buffer){abc, sizeof(abc) - 1U},
                           &changed_policy, changed_key));
  changed_policy.allow_experimental = true;
  assert(laghu_variant_key((laghu_buffer){abc, sizeof(abc) - 1U},
                           &changed_policy, repeated_key));
  assert(strcmp(changed_key, repeated_key) != 0);

  changed_policy.filter_families = UINT32_MAX;
  assert(!laghu_variant_key((laghu_buffer){abc, sizeof(abc) - 1U},
                            &changed_policy, changed_key));
  assert(changed_key[0] == '\0');

  assert(laghu_resolve_policy(LAGHU_PRESET_BALANCED, &changed_policy));
  changed_policy.rewrite_level = (laghu_rewrite_level)99;
  assert(!laghu_variant_key((laghu_buffer){abc, sizeof(abc) - 1U},
                            &changed_policy, changed_key));
  assert(changed_key[0] == '\0');

  assert(
      laghu_resolve_rewrite_level(LAGHU_REWRITE_LEVEL_CORE, &changed_policy));
  changed_policy.preset = (laghu_preset)99;
  assert(!laghu_variant_key((laghu_buffer){abc, sizeof(abc) - 1U},
                            &changed_policy, changed_key));
  assert(changed_key[0] == '\0');
}

int main(void) {
  test_config_defaults_and_inheritance();
  test_preset_parser();
  test_preset_policies();
  test_rewrite_level_parser_and_policies();
  test_decision_precedence();
  test_api_path_policy();
  test_rewrite_level_safety_precedence();
  test_candidate_finalization();
  test_hashing();

  puts("laghu_core_test: all tests passed");
  return 0;
}
