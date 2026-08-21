// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/cache.h"
#include "laghu/instrumentation.h"
#include "laghu/javascript.h"
#include "laghu/queue.h"
#include "laghu/rum.h"
#include "laghu/types.h"
#include "test_fixture.h"

static void publish_javascript_fixture(const char *cache_path, const char *url, const char *policy, const char *target, const char *source,
                                       const char *derived, uint32_t flags, uint64_t updated, char variant[LAGHU_RUNTIME_KEY_SIZE]) {
  unsigned char catalog[1908U] = {0};
  uint64_t magic = UINT64_C(0x4c414748554a5343);
  uint64_t source_length = strlen(source), derived_length = strlen(derived);
  uint32_t version = 3U, module = flags & 1U;
  char canonical[LAGHU_RUNTIME_PATH_SIZE + LAGHU_RUNTIME_KEY_SIZE + LAGHU_JAVASCRIPT_TARGET_SIZE + 64U];
  char catalog_key[LAGHU_RUNTIME_KEY_SIZE], checksum[LAGHU_RUNTIME_KEY_SIZE];
  char path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_runtime_cache_entry entry;
  FILE *file;
  int length;
  assert(laghu_sha256_hex((laghu_buffer){(const unsigned char *)derived, derived_length}, variant));
  assert(laghu_runtime_cache_publish(cache_path, variant, variant, variant, "application/javascript", "swc-test",
                                     (laghu_buffer){(const unsigned char *)derived, derived_length}, &entry));
  memcpy(catalog, &magic, sizeof(magic));
  memcpy(catalog + 8U, &version, sizeof(version));
  memcpy(catalog + 12U, &module, sizeof(module));
  memcpy(catalog + 16U, &updated, sizeof(updated));
  memcpy(catalog + 24U, &source_length, sizeof(source_length));
  memcpy(catalog + 32U, &derived_length, sizeof(derived_length));
  strcpy((char *)catalog + 40U, url);
  strcpy((char *)catalog + 1064U, policy);
  strcpy((char *)catalog + 1129U, target);
  strcpy((char *)catalog + 1706U, variant);
  memcpy(catalog + 1836U, &flags, sizeof(flags));
  assert(laghu_sha256_hex((laghu_buffer){catalog, 1840U}, checksum));
  strcpy((char *)catalog + 1840U, checksum);
  length = snprintf(canonical, sizeof(canonical), "laghu-js-url-v1\n%s\n%s\n%s\n%s", url, policy, target, module != 0U ? "module" : "classic");
  assert(length > 0 && (size_t)length < sizeof(canonical));
  assert(laghu_sha256_hex((laghu_buffer){(const unsigned char *)canonical, (size_t)length}, catalog_key));
  length = snprintf(path, sizeof(path), "%s/javascript-%s.meta", cache_path, catalog_key);
  assert(length > 0 && (size_t)length < sizeof(path));
  file = fopen(path, "wb");
  assert(file != NULL);
  assert(fwrite(catalog, 1U, sizeof(catalog), file) == sizeof(catalog));
  assert(fclose(file) == 0);
}

static const unsigned char *javascript_find(const unsigned char *data, size_t length, const char *needle) {
  size_t index, needle_length = strlen(needle);
  if (data == NULL || needle_length == 0U || needle_length > length) return NULL;
  for (index = 0U; index <= length - needle_length; ++index)
    if (memcmp(data + index, needle, needle_length) == 0) return data + index;
  return NULL;
}

static void test_javascript_defer(const laghu_test_workspace *workspace) {
  static const char config[] =
      "# administrator approvals\n"
      "defer /assets/analytics.js\n"
      "defer /assets/checkout.js template=/checkout/\n"
      "interaction https://cdn.example.test/analytics.js "
      "template=/checkout/\n";
  static laghu_javascript_defer_set defer;
  laghu_rum_instrumentation_record baseline = {0}, current = {0};
  char path[LAGHU_RUNTIME_PATH_SIZE], error[128];

  assert(laghu_test_workspace_path(workspace, "javascript-defer.conf", path, sizeof(path)));
  assert(laghu_test_workspace_write(workspace, "javascript-defer.conf", (const unsigned char *)config, sizeof(config) - 1U));
  assert(laghu_javascript_defer_load(path, &defer, error, sizeof(error)));
  assert(defer.count == 3U && strlen(defer.digest) == 64U);
  assert(laghu_javascript_defer_approved(&defer, "/assets/analytics.js", "/anywhere/"));
  assert(laghu_javascript_defer_approved(&defer, "/assets/checkout.js", "/checkout/"));
  assert(!laghu_javascript_defer_approved(&defer, "/assets/checkout.js", "/account/"));
  assert(laghu_javascript_interaction_approved(&defer, "https://cdn.example.test/analytics.js", "/checkout/"));
  assert(!laghu_javascript_interaction_approved(&defer, "https://cdn.example.test/analytics.js", "/account/"));
  assert(!laghu_javascript_interaction_approved(&defer, "https://cdn.example.test/analytics.js?debug=1", "/checkout/"));
  assert(laghu_test_workspace_write(workspace, "javascript-defer-invalid.conf",
                                    (const unsigned char *)"interaction https://cdn.example.test/a.js?bad=1\n",
                                    sizeof("interaction https://cdn.example.test/a.js?bad=1\n") - 1U));
  assert(laghu_test_workspace_path(workspace, "javascript-defer-invalid.conf", path, sizeof(path)));
  assert(!laghu_javascript_defer_load(path, &defer, error, sizeof(error)));
  assert(laghu_test_workspace_write(workspace, "javascript-defer-invalid.conf",
                                    (const unsigned char *)"interaction https://cdn.example.test:8443/a.js\n",
                                    sizeof("interaction https://cdn.example.test:8443/a.js\n") - 1U));
  assert(!laghu_javascript_defer_load(path, &defer, error, sizeof(error)));
  baseline.version = current.version = LAGHU_INSTRUMENTATION_VERSION;
  baseline.script_count = current.script_count = 1U;
  baseline.observations[1] = 100U;
  baseline.script_observations[1][0] = 90U;
  baseline.histograms[1][1][2] = 100U;
  assert(laghu_javascript_defer_recommended(&baseline, 1U, 0U));
  baseline.histograms[1][1][1] = 100U;
  baseline.histograms[1][1][2] = 0U;
  assert(!laghu_javascript_defer_recommended(&baseline, 1U, 0U));
  baseline.histograms[1][1][2] = 100U;
  current.observations[1] = 50U;
  baseline.errors[1] = 1U;
  current.errors[1] = 2U;
  assert(laghu_javascript_defer_rollback_recommended(&baseline, &current, 1U));
}

static void test_javascript_yield(void) {
  static const unsigned char cooperative[] =
      "<html><body><script data-laghu-yield=\"cooperative\" "
      "nonce=\"yieldNonce-1\">async function render(){await "
      "window.Laghu.yield()}</script></body></html>";
  static const unsigned char unmarked[] =
      "<html><body><script nonce=\"yieldNonce-1\">app()</script></body>"
      "</html>";
  static const unsigned char missing_nonce[] =
      "<html><body><script data-laghu-yield=\"cooperative\">app()</script>"
      "</body></html>";
  laghu_csp_policy csp;
  laghu_runtime_html_result result;

  laghu_csp_policy_init(&csp, "https://example.test");
  assert(laghu_csp_policy_add(&csp, "script-src 'nonce-yieldNonce-1'", strlen("script-src 'nonce-yieldNonce-1'")));
  assert(laghu_runtime_add_javascript_yield((laghu_buffer){cooperative, sizeof(cooperative) - 1U}, &csp, true, &result));
  assert(result.rewritten && result.length > sizeof(cooperative) - 1U);
  assert(javascript_find(result.data, result.length, "nonce=\"yieldNonce-1\"") != NULL);
  assert(javascript_find(result.data, result.length, "scheduler.postTask") != NULL);
  assert(javascript_find(result.data, result.length, "requestIdleCallback") != NULL);
  assert(javascript_find(result.data, result.length, "g.setTimeout(d,0)") != NULL);
  assert(javascript_find(result.data, result.length, "data-laghu-yield=\"cooperative\"") != NULL);
  laghu_runtime_html_result_release(&result);

  assert(laghu_runtime_add_javascript_yield((laghu_buffer){unmarked, sizeof(unmarked) - 1U}, &csp, true, &result));
  assert(!result.rewritten && result.data == NULL);
  assert(laghu_runtime_add_javascript_yield((laghu_buffer){missing_nonce, sizeof(missing_nonce) - 1U}, &csp, true, &result));
  assert(!result.rewritten && result.data == NULL);
  assert(laghu_runtime_add_javascript_yield((laghu_buffer){cooperative, sizeof(cooperative) - 1U}, &csp, false, &result));
  assert(!result.rewritten && result.data == NULL);
  laghu_csp_policy_init(&csp, "https://example.test");
  assert(laghu_csp_policy_add(&csp, "script-src 'none'", strlen("script-src 'none'")));
  assert(laghu_runtime_add_javascript_yield((laghu_buffer){cooperative, sizeof(cooperative) - 1U}, &csp, true, &result));
  assert(!result.rewritten && result.data == NULL);
}

static void test_javascript_rewrite(const laghu_test_workspace *workspace, const char *policy_key) {
  laghu_test_queue_pair pair;
  laghu_rum_options rum_options;
  laghu_rum_engine *rum_engine;
  laghu_runtime_job taken;
  unsigned char received[1024U];
  char javascript_target[LAGHU_JAVASCRIPT_TARGET_SIZE];
  static const unsigned char payload[] = "runtime payload";

  assert(laghu_test_queue_pair_open(&pair, workspace, "jobs.queue", 1U, 1024U));
  assert(laghu_javascript_target_normalize("  Defaults   AND supports ES6-module and not dead  ", javascript_target));
  assert(strcmp(javascript_target, "defaults and supports es6-module and not dead") == 0);
  assert(!laghu_javascript_target_normalize("extends ../browser", javascript_target));
  {
    laghu_runtime_javascript_result javascript;
    assert(laghu_runtime_rewrite_javascript(&pair.producer, workspace->path, (laghu_buffer){payload, sizeof(payload) - 1U}, "/application.js",
                                            policy_key, "last 2 chrome versions", false, true, &javascript));
    assert(!javascript.rewritten && javascript.published);
    assert(laghu_runtime_queue_try_take(&pair.consumer, &taken, received, sizeof(received)));
    assert(taken.kind == LAGHU_RUNTIME_JOB_JAVASCRIPT);
    assert(taken.filters == UINT64_C(2));
    assert(strcmp(taken.javascript_target, "last 2 chrome versions") == 0);
    assert(taken.payload.length == sizeof(payload) - 1U);
    laghu_runtime_javascript_result_release(&javascript);
  }
  {
    static const unsigned char html[] =
        "<html><body><script>function publicName(longLocal) { return "
        "longLocal + 1; }</script></body></html>";
    static const unsigned char optimized[] = "function publicName(n){return n+1}";
    laghu_runtime_html_result javascript_page;
    laghu_runtime_cache_entry javascript_entry;
    char variant[LAGHU_RUNTIME_KEY_SIZE];
    assert(laghu_runtime_rewrite_javascript_html(&pair.producer, workspace->path, (laghu_buffer){html, sizeof(html) - 1U}, "/inline", policy_key,
                                                 "last 2 chrome versions", NULL, 100U, 60U, NULL, NULL, NULL, 1U, NULL, false, false, false, false,
                                                 false, false, 2048U, 8192U, &javascript_page));
    assert(!javascript_page.rewritten && javascript_page.dependencies_pending && javascript_page.job_published);
    laghu_runtime_html_result_release(&javascript_page);
    assert(laghu_runtime_queue_try_take(&pair.consumer, &taken, received, sizeof(received)));
    assert(taken.kind == LAGHU_RUNTIME_JOB_JAVASCRIPT);
    assert(laghu_sha256_hex((laghu_buffer){optimized, sizeof(optimized) - 1U}, variant));
    assert(laghu_runtime_cache_publish(workspace->path, taken.index_key, variant, taken.validator, "application/javascript", "swc-test",
                                       (laghu_buffer){optimized, sizeof(optimized) - 1U}, &javascript_entry));
    assert(laghu_runtime_rewrite_javascript_html(&pair.producer, workspace->path, (laghu_buffer){html, sizeof(html) - 1U}, "/inline", policy_key,
                                                 "last 2 chrome versions", NULL, 101U, 60U, NULL, NULL, NULL, 1U, NULL, false, false, false, false,
                                                 false, false, 2048U, 8192U, &javascript_page));
    assert(javascript_page.rewritten && javascript_page.length < sizeof(html) - 1U);
    assert(!javascript_page.job_published);
    assert(javascript_find(javascript_page.data, javascript_page.length, "function publicName(n){return n+1}") != NULL);
    laghu_runtime_html_result_release(&javascript_page);
  }
  {
    static const char source[] =
        "function outlinedPublic(veryLongLocal){return veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+"
        "veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+"
        "veryLongLocal;}";
    static const char derived[] = "function outlinedPublic(n){return n+n+n+n+n+n+n+n+n+n+n+n+n+n+n+n}";
    static const unsigned char html[] =
        "<html><body><script>function outlinedPublic(veryLongLocal){return veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+"
        "veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+veryLongLocal+"
        "veryLongLocal+veryLongLocal+veryLongLocal;}</script></body></html>";
    laghu_runtime_html_result page;
    laghu_runtime_cache_entry entry;
    char variant[LAGHU_RUNTIME_KEY_SIZE];
    char expected[LAGHU_RUNTIME_KEY_SIZE + 32U];
    assert(laghu_runtime_rewrite_javascript_html(&pair.producer, workspace->path, (laghu_buffer){html, sizeof(html) - 1U}, "/outline", policy_key,
                                                 "last 2 chrome versions", NULL, 100U, 60U, NULL, NULL, NULL, 1U, NULL, false, false, false, false,
                                                 true, false, 2048U, 1U, &page));
    assert(!page.rewritten && page.dependencies_pending && page.job_published);
    laghu_runtime_html_result_release(&page);
    assert(laghu_runtime_queue_try_take(&pair.consumer, &taken, received, sizeof(received)));
    publish_javascript_fixture(workspace->path, "/outline#script-1", policy_key, "last 2 chrome versions", source, derived, 2U, 100U, variant);
    assert(laghu_runtime_cache_publish(workspace->path, taken.index_key, variant, taken.validator, "application/javascript", "swc-test",
                                       (laghu_buffer){(const unsigned char *)derived, sizeof(derived) - 1U}, &entry));
    assert(laghu_runtime_rewrite_javascript_html(&pair.producer, workspace->path, (laghu_buffer){html, sizeof(html) - 1U}, "/outline", policy_key,
                                                 "last 2 chrome versions", NULL, 101U, 60U, NULL, NULL, NULL, 1U, NULL, false, false, false, false,
                                                 true, false, 2048U, 1U, &page));
    assert(page.rewritten);
    assert(snprintf(expected, sizeof(expected), "src=\"/.laghu/js/%s\"></script>", variant) > 0);
    assert(javascript_find(page.data, page.length, expected) != NULL);
    laghu_runtime_html_result_release(&page);
  }
  {
    static const unsigned char html[] =
        "<html><body><script "
        "src=\"https://cdn.example.test/analytics.js\" "
        "nonce=\"abc_DEF-123=\"></script><script "
        "src=\"https://cdn.example.test/chat.js\" "
        "nonce=\"abc_DEF-123=\"></script></body></html>";
    static laghu_javascript_defer_set interaction;
    static laghu_csp_policy csp;
    laghu_runtime_html_result page;
    strcpy(interaction.rules[0].script_path, "https://cdn.example.test/analytics.js");
    strcpy(interaction.rules[0].template_path, "/checkout/");
    interaction.rules[0].mode = LAGHU_JAVASCRIPT_DELAY_INTERACTION;
    strcpy(interaction.rules[1].script_path, "https://cdn.example.test/chat.js");
    interaction.rules[1].mode = LAGHU_JAVASCRIPT_DELAY_INTERACTION;
    interaction.count = 2U;
    laghu_csp_policy_init(&csp, "https://example.test");
    assert(laghu_csp_policy_add(&csp, "script-src 'nonce-abc_DEF-123=' 'strict-dynamic'",
                                sizeof("script-src 'nonce-abc_DEF-123=' 'strict-dynamic'") - 1U));
    assert(laghu_runtime_rewrite_javascript_html(&pair.producer, workspace->path, (laghu_buffer){html, sizeof(html) - 1U}, "/checkout/", policy_key,
                                                 "last 2 chrome versions", &csp, 101U, 60U, NULL, NULL, NULL, 1U, &interaction, true, false, false,
                                                 false, false, false, 2048U, 8192U, &page));
    assert(page.rewritten);
    const unsigned char *loader;
    assert(javascript_find(page.data, page.length, "type=\"application/x-laghu-interaction\"") != NULL);
    assert(javascript_find(page.data, page.length,
                           "data-laghu-interaction-src=\"https://cdn.example.test/"
                           "analytics.js\"") != NULL);
    assert(javascript_find(page.data, page.length,
                           "data-laghu-interaction-src=\"https://cdn.example.test/"
                           "chat.js\"") != NULL);
    loader = javascript_find(page.data, page.length, "data-laghu-interaction-loader");
    assert(loader != NULL);
    assert(javascript_find(loader + 1U, page.length - (size_t)(loader + 1U - page.data), "data-laghu-interaction-loader") != NULL);
    assert(javascript_find(page.data, page.length, "nonce=\"abc_DEF-123=\"") != NULL);
    laghu_runtime_html_result_release(&page);
  }
  {
    static const unsigned char html[] =
        "<html><body><script "
        "src=\"https://cdn.example.test/analytics.js\"></script>"
        "</body></html>";
    static laghu_javascript_defer_set interaction;
    static laghu_csp_policy csp;
    laghu_runtime_html_result page;
    strcpy(interaction.rules[0].script_path, "https://cdn.example.test/analytics.js");
    interaction.rules[0].mode = LAGHU_JAVASCRIPT_DELAY_INTERACTION;
    interaction.count = 1U;
    laghu_csp_policy_init(&csp, "https://example.test");
    assert(laghu_csp_policy_add(&csp, "script-src 'self'", sizeof("script-src 'self'") - 1U));
    assert(laghu_runtime_rewrite_javascript_html(&pair.producer, workspace->path, (laghu_buffer){html, sizeof(html) - 1U}, "/checkout/", policy_key,
                                                 "last 2 chrome versions", &csp, 101U, 60U, NULL, NULL, NULL, 1U, &interaction, true, false, false,
                                                 false, false, false, 2048U, 8192U, &page));
    assert(!page.rewritten);
    laghu_runtime_html_result_release(&page);
  }
  {
    static const unsigned char html[] =
        "<html><body><script "
        "src=\"https://cdn.example.test/analytics.js\" async></script>"
        "</body></html>";
    static laghu_javascript_defer_set interaction;
    laghu_runtime_html_result page;
    strcpy(interaction.rules[0].script_path, "https://cdn.example.test/analytics.js");
    interaction.rules[0].mode = LAGHU_JAVASCRIPT_DELAY_INTERACTION;
    interaction.count = 1U;
    assert(laghu_runtime_rewrite_javascript_html(&pair.producer, workspace->path, (laghu_buffer){html, sizeof(html) - 1U}, "/checkout/", policy_key,
                                                 "last 2 chrome versions", NULL, 101U, 60U, NULL, NULL, NULL, 1U, &interaction, true, false, false,
                                                 false, false, false, 2048U, 8192U, &page));
    assert(!page.rewritten);
    laghu_runtime_html_result_release(&page);
  }
  {
    static const char source[] = "console.log('one'); console.log('two'); console.log('three');";
    static const char derived[] = "console.log(1),console.log(2)";
    static const unsigned char html[] =
        "<html><body><script src=\"/small.js\"></script><p>padding padding "
        "padding</p></body></html>";
    laghu_runtime_html_result page;
    char fixture_variant[LAGHU_RUNTIME_KEY_SIZE];
    publish_javascript_fixture(workspace->path, "/small.js", policy_key, "last 2 chrome versions", source, derived, 2U, 100U, fixture_variant);
    assert(laghu_runtime_rewrite_javascript_html(&pair.producer, workspace->path, (laghu_buffer){html, sizeof(html) - 1U}, "/inline-external",
                                                 policy_key, "last 2 chrome versions", NULL, 101U, 60U, NULL, NULL, NULL, 1U, NULL, false, false,
                                                 false, true, false, false, 2048U, 8192U, &page));
    assert(page.rewritten);
    assert(javascript_find(page.data, page.length, "src=") == NULL);
    assert(javascript_find(page.data, page.length, derived) != NULL);
    laghu_runtime_html_result_release(&page);
  }
  {
    static const char first_source[] =
        "console.log('first value'); console.log('first value again'); "
        "console.log('first value third'); console.log('first value fourth'); "
        "console.log('first value fifth');";
    static const char second_source[] =
        "console.log('second value'); console.log('second value again'); "
        "console.log('second value third'); console.log('second value "
        "fourth'); "
        "console.log('second value fifth');";
    static const char first[] =
        "console.log('first')\n//# sourceMappingURL=/.laghu/js/"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.map";
    static const char second[] =
        "console.log('second')\n//# sourceMappingURL=/.laghu/js/"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.map";
    static const unsigned char html[] =
        "<html><body><script src=\"/assets/application-one-entry.js\" "
        "data-laghu-combine=\"application-main\"></script>\n"
        "<script src=\"/assets/application-two-entry.js\" "
        "data-laghu-combine=\"application-main\"></script></body></html>";
    laghu_runtime_html_result page;
    laghu_runtime_cache_entry bundle_entry, map_entry;
    unsigned char bundle[1024U], map[2048U];
    const unsigned char *route;
    const char *map_route;
    char bundle_key[LAGHU_RUNTIME_KEY_SIZE];
    char map_key[LAGHU_RUNTIME_KEY_SIZE];
    char fixture_variant[LAGHU_RUNTIME_KEY_SIZE];
    assert(laghu_runtime_cache_publish(workspace->path, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "application/json", "swc-test",
                                       (laghu_buffer){(const unsigned char *)"{\"version\":3}", 13U}, &map_entry));
    assert(laghu_runtime_cache_publish(workspace->path, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                                       "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                                       "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "application/json", "swc-test",
                                       (laghu_buffer){(const unsigned char *)"{\"version\":3}", 13U}, &map_entry));
    publish_javascript_fixture(workspace->path, "/assets/application-one-entry.js", policy_key, "last 2 chrome versions", first_source, first,
                               2U | 4U | 8U, 100U, fixture_variant);
    publish_javascript_fixture(workspace->path, "/assets/application-two-entry.js", policy_key, "last 2 chrome versions", second_source, second,
                               2U | 4U | 8U, 100U, fixture_variant);
    assert(laghu_runtime_rewrite_javascript_html(&pair.producer, workspace->path, (laghu_buffer){html, sizeof(html) - 1U}, "/combine", policy_key,
                                                 "last 2 chrome versions", NULL, 101U, 60U, NULL, NULL, NULL, 1U, NULL, false, false, true, false,
                                                 false, true, 2048U, 8192U, &page));
    assert(page.rewritten);
    assert(javascript_find(page.data, page.length, "/.laghu/js/") != NULL);
    assert(javascript_find(page.data, page.length, "data-laghu-combine=\"application-main\"") != NULL);
    route = javascript_find(page.data, page.length, "/.laghu/js/");
    assert(route != NULL);
    memcpy(bundle_key, route + 11U, LAGHU_SHA256_HEX_LENGTH);
    bundle_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
    assert(laghu_runtime_cache_lookup_variant(workspace->path, bundle_key, &bundle_entry));
    assert(bundle_entry.length < sizeof(bundle));
    assert(laghu_runtime_cache_read(&bundle_entry, bundle, bundle_entry.length));
    bundle[bundle_entry.length] = '\0';
    map_route = strstr((const char *)bundle, "sourceMappingURL=/.laghu/js/");
    assert(map_route != NULL);
    memcpy(map_key, map_route + sizeof("sourceMappingURL=/.laghu/js/") - 1U, LAGHU_SHA256_HEX_LENGTH);
    map_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
    assert(laghu_runtime_cache_lookup_variant(workspace->path, map_key, &map_entry));
    assert(map_entry.length < sizeof(map));
    assert(laghu_runtime_cache_read(&map_entry, map, map_entry.length));
    map[map_entry.length] = '\0';
    assert(strstr((const char *)map, "\"version\":3") != NULL);
    assert(strstr((const char *)map, "\"sections\":[") != NULL);
    assert(strstr((const char *)map, "aaaaaaaaaaaaaaaa") != NULL);
    assert(strstr((const char *)map, "bbbbbbbbbbbbbbbb") != NULL);
    laghu_runtime_html_result_release(&page);
  }
  laghu_rum_options_init(&rum_options);
  rum_engine = laghu_rum_engine_create(&rum_options, NULL, 0U);
  assert(rum_engine != NULL);
  {
    static const char source[] = "console.log('approved deferred script with enough source bytes');";
    static const char derived[] = "console.log('deferred')";
    static const unsigned char html[] =
        "<html><body><script "
        "src=\"/assets/deferred.js\"></script></body></html>";
    static const char template_key[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static laghu_javascript_defer_set defer;
    laghu_rum_instrumentation_record record = {0};
    laghu_runtime_html_result page;
    char absolute[] = "https://example.test/assets/deferred.js";
    char fixture_variant[LAGHU_RUNTIME_KEY_SIZE];
    publish_javascript_fixture(workspace->path, "/assets/deferred.js", policy_key, "last 2 chrome versions", source, derived, 16U, 100U,
                               fixture_variant);
    strcpy(defer.rules[0].script_path, "/assets/deferred.js");
    defer.count = 1U;
    memset(defer.digest, 'b', LAGHU_SHA256_HEX_LENGTH);
    defer.digest[LAGHU_SHA256_HEX_LENGTH] = '\0';
    record.version = LAGHU_INSTRUMENTATION_VERSION;
    strcpy(record.template_key, template_key);
    strcpy(record.provider_digest, "none");
    strcpy(record.policy_key, policy_key);
    record.updated_at = 100U;
    record.script_count = 1U;
    assert(laghu_sha256_hex((laghu_buffer){(const unsigned char *)absolute, strlen(absolute)}, record.script_keys[0]));
    record.observations[1] = 100U;
    record.script_observations[1][0] = 90U;
    assert(laghu_rum_engine_publish(rum_engine, LAGHU_RUM_RECORD_INSTRUMENTATION, template_key, 100U, &record, sizeof(record), NULL));
    assert(laghu_runtime_rewrite_javascript_html(&pair.producer, workspace->path, (laghu_buffer){html, sizeof(html) - 1U}, "/page", policy_key,
                                                 "last 2 chrome versions", NULL, 101U, 60U, rum_engine, template_key, "https://example.test", 1U,
                                                 &defer, true, true, false, false, false, false, 2048U, 8192U, &page));
    assert(page.rewritten && javascript_find(page.data, page.length, "src=\"/assets/deferred.js\" defer") != NULL);
    laghu_runtime_html_result_release(&page);
  }
  laghu_rum_engine_destroy(rum_engine);
  laghu_test_queue_pair_close(&pair);
}

int main(void) {
  static const unsigned char payload[] = "runtime payload";
  laghu_test_workspace workspace;
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];

  assert(laghu_test_workspace_create(&workspace));
  assert(laghu_sha256_hex((laghu_buffer){payload, sizeof(payload) - 1U}, policy_key));
  test_javascript_defer(&workspace);
  test_javascript_yield();
  test_javascript_rewrite(&workspace, policy_key);
  assert(laghu_test_workspace_remove(&workspace));
  puts("laghu_runtime_javascript_integration_test: all tests passed");
  return 0;
}
