#include "laghu/core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static laghu_config enabled_config(void) {
  laghu_config config;

  laghu_config_init(&config);
  config.mode = LAGHU_MODE_ON;
  config.preset = LAGHU_PRESET_BALANCED;
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
  assert(policy.filter_families == expected_filters);
  assert(policy.risk_level == expected_risk);
  assert(policy.allow_lossy == allow_lossy);
  assert(policy.allow_structural_rewrite == allow_structural_rewrite);
  assert(policy.allow_resource_inlining == allow_resource_inlining);
  assert(policy.allow_script_reordering == allow_script_reordering);
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
  assert(result.allow_api == LAGHU_MODE_OFF);

  parent.mode = LAGHU_MODE_ON;
  parent.preset = LAGHU_PRESET_SAFE;
  parent.allow_api = LAGHU_MODE_ON;
  child.preset = LAGHU_PRESET_STATIC;
  laghu_config_merge(&result, &parent, &child);
  assert(result.mode == LAGHU_MODE_ON);
  assert(result.preset == LAGHU_PRESET_STATIC);
  assert(result.allow_api == LAGHU_MODE_ON);

  child.mode = LAGHU_MODE_OFF;
  child.allow_api = LAGHU_MODE_OFF;
  laghu_config_merge(&result, &parent, &child);
  assert(result.mode == LAGHU_MODE_OFF);
  assert(result.preset == LAGHU_PRESET_STATIC);
  assert(result.allow_api == LAGHU_MODE_OFF);
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

static void test_decision_precedence(void) {
  laghu_config config = enabled_config();
  laghu_response response = html_response();

  assert(laghu_decide(&config, &response) == LAGHU_DECISION_PASS);
  assert(laghu_decide(NULL, &response) == LAGHU_DECISION_BYPASS_DISABLED);
  assert(laghu_decide(&config, NULL) == LAGHU_DECISION_BYPASS_ERROR);

  config.preset = LAGHU_PRESET_UNSET;
  assert(laghu_decide(&config, &response) == LAGHU_DECISION_BYPASS_ERROR);
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
                "41a2e18ecc1a671dd9b2adc1b99a5d36e792ca3fdfbb3de5555ce6eb1bd"
                "ac78e") == 0);

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

  changed_policy.filter_families = UINT32_MAX;
  assert(!laghu_variant_key((laghu_buffer){abc, sizeof(abc) - 1U},
                            &changed_policy, changed_key));
  assert(changed_key[0] == '\0');
}

int main(void) {
  test_config_defaults_and_inheritance();
  test_preset_parser();
  test_preset_policies();
  test_decision_precedence();
  test_api_path_policy();
  test_candidate_finalization();
  test_hashing();

  puts("laghu_core_test: all tests passed");
  return 0;
}
