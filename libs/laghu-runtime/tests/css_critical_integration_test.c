// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/cache.h"
#include "laghu/csp.h"
#include "laghu/css.h"
#include "laghu/fonts.h"
#include "laghu/queue.h"
#include "laghu/rum.h"
#include "laghu/types.h"
#include "test_fixture.h"

static void test_font_css(const laghu_test_workspace *workspace) {
  static const char configuration[] =
      "provider google_fonts\n"
      "stylesheet fonts.googleapis.com /css\n"
      "stylesheet fonts.googleapis.com /css2\n"
      "asset fonts.gstatic.com /\n"
      "max_css_bytes 262144\n"
      "ttl_seconds 604800\n"
      "end\n"
      "provider fontsource_cdn\n"
      "stylesheet cdn.jsdelivr.net /fontsource/css/\n"
      "asset cdn.jsdelivr.net /fontsource/fonts/\n"
      "end\n";
  static const unsigned char google_css[] =
      "@font-face{font-family:'Roboto';src:url(https://fonts.gstatic.com/s/"
      "roboto/v1/a.woff2) format('woff2')}";
  static const unsigned char hostile_css[] = "@font-face{src:url(https://example.test/a.woff2)}";
  static const unsigned char page[] =
      "<html><head><link rel=stylesheet href=\"https://fonts.googleapis."
      "com/css2?family=Roboto\"></head><body></body></html>";
  laghu_font_provider_set providers;
  const laghu_font_provider *google;
  const laghu_font_provider *fontsource;
  laghu_test_queue_pair queue;
  laghu_runtime_job job;
  laghu_runtime_html_result rewritten;
  laghu_runtime_cache_entry entry;
  laghu_font_stylesheet_record record = {0};
  char path[LAGHU_RUNTIME_PATH_SIZE], css_key[LAGHU_RUNTIME_KEY_SIZE];
  char error[128];
  unsigned char ignored[1];

  assert(laghu_test_workspace_path(workspace, "font-providers.conf", path, sizeof(path)));
  assert(laghu_test_workspace_write(workspace, "font-providers.conf", (const unsigned char *)configuration, sizeof(configuration) - 1U));
  assert(laghu_font_providers_load(path, &providers, error, sizeof(error)));
  assert(providers.count == 2U);
  google = laghu_font_provider_match(&providers, "https://fonts.googleapis.com/css2?family=Roboto&display=swap");
  fontsource = laghu_font_provider_match(&providers, "https://cdn.jsdelivr.net/fontsource/css/open-sans@5.2.7/index.css");
  assert(google != NULL && strcmp(google->id, "google_fonts") == 0);
  assert(laghu_font_provider_url_allowed(&providers.providers[1], "https://cdn.jsdelivr.net/fontsource/css/open-sans@5.2.7/index.css", false, false));
  assert(fontsource != NULL && strcmp(fontsource->id, "fontsource_cdn") == 0);
  assert(laghu_font_provider_match(&providers, "https://fonts.googleapis.com/evil") == NULL);
  assert(laghu_font_css_validate(google, (laghu_buffer){google_css, sizeof(google_css) - 1U}));
  assert(!laghu_font_css_validate(google, (laghu_buffer){hostile_css, sizeof(hostile_css) - 1U}));
  assert(laghu_test_queue_pair_open(&queue, workspace, "fonts.queue", 2U, 1U));
  assert(laghu_runtime_rewrite_font_css(&queue.producer, workspace->path, &providers, (laghu_buffer){page, sizeof(page) - 1U}, 100U, true, NULL,
                                        2048U, &rewritten));
  assert(!rewritten.rewritten && rewritten.dependencies_pending);
  assert(laghu_runtime_queue_try_take(&queue.consumer, &job, ignored, sizeof(ignored)));
  assert(job.kind == LAGHU_RUNTIME_JOB_FONT_CSS);
  assert(strcmp(job.provider_id, "google_fonts") == 0);
  assert(laghu_sha256_hex((laghu_buffer){google_css, sizeof(google_css) - 1U}, css_key));
  assert(laghu_runtime_cache_publish(workspace->path, css_key, css_key, css_key, "text/css", "test-font-fetch",
                                     (laghu_buffer){google_css, sizeof(google_css) - 1U}, &entry));
  strcpy(record.provider_id, google->id);
  strcpy(record.provider_digest, google->digest);
  strcpy(record.normalized_url, "https://fonts.googleapis.com/css2?family=Roboto");
  strcpy(record.variant_key, css_key);
  record.css_length = sizeof(google_css) - 1U;
  record.fetched_at = 100U;
  record.ttl_seconds = 604800U;
  record.ready = true;
  assert(laghu_font_stylesheet_publish(workspace->path, &record));
  assert(laghu_runtime_rewrite_font_css(&queue.producer, workspace->path, &providers, (laghu_buffer){page, sizeof(page) - 1U}, 101U, true, NULL,
                                        2048U, &rewritten));
  assert(rewritten.rewritten && !rewritten.dependencies_pending);
  assert(strstr((const char *)rewritten.data, "<style>@font-face") != NULL);
  assert(strstr((const char *)rewritten.data, "font-display:swap") != NULL);
  assert(rewritten.link_header_count == 1U);
  assert(strstr(rewritten.link_headers[0], "rel=preload; as=font") != NULL);
  assert(strstr((const char *)rewritten.data, "fonts.googleapis.com") == NULL);
  laghu_runtime_html_result_release(&rewritten);
  laghu_test_queue_pair_close(&queue);
}

static void test_css_derivation(const char *path, const char *policy_key) {
  static const unsigned char css[] = "/*! keep */\n.hero { color: red; margin: 0  0; } /* remove */\n";
  static const unsigned char leaf_css[] = ".leaf { color: blue; margin: 0  0  0  0; } /* leaf remove */";
  static const unsigned char root_css[] = "@import \"/leaf.css\" print; .root { color: red; padding: 0  0; }";
  laghu_runtime_css_result stylesheet;
  laghu_runtime_html_result markup;

  assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){css, sizeof(css) - 1U}, "/site.css", "https://example.test", policy_key, 0x55aaU,
                                   900000U, 604800U, true, false, 2048U, 8192U, &stylesheet));
  assert(!stylesheet.rewritten && stylesheet.published);
  laghu_runtime_css_result_release(&stylesheet);
  assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){css, sizeof(css) - 1U}, "/site.css", "https://example.test", policy_key, 0x55aaU,
                                   900001U, 604800U, true, false, 2048U, 8192U, &stylesheet));
  assert(stylesheet.rewritten && !stylesheet.published);
  assert(strstr((const char *)stylesheet.data, "/*! keep */") != NULL);
  assert(strstr((const char *)stylesheet.data, "remove") == NULL);
  laghu_runtime_css_result_release(&stylesheet);

  assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){leaf_css, sizeof(leaf_css) - 1U}, "/leaf.css", "https://example.test", policy_key,
                                   0x55aaU, 900010U, 604800U, true, false, 2048U, 8192U, &stylesheet));
  assert(stylesheet.published && !stylesheet.rewritten);
  laghu_runtime_css_result_release(&stylesheet);
  assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){leaf_css, sizeof(leaf_css) - 1U}, "/leaf.css", "https://example.test", policy_key,
                                   0x55aaU, 900011U, 604800U, true, false, 2048U, 8192U, &stylesheet));
  assert(stylesheet.rewritten);
  laghu_runtime_css_result_release(&stylesheet);
  assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){root_css, sizeof(root_css) - 1U}, "/root.css", "https://example.test", policy_key,
                                   0x55aaU, 900012U, 604800U, true, true, 2048U, 8192U, &stylesheet));
  assert(stylesheet.published && !stylesheet.rewritten);
  laghu_runtime_css_result_release(&stylesheet);
  assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){root_css, sizeof(root_css) - 1U}, "/root.css", "https://example.test", policy_key,
                                   0x55aaU, 900013U, 604800U, true, true, 2048U, 8192U, &stylesheet));
  assert(stylesheet.rewritten && strstr((const char *)stylesheet.data, "@import") == NULL);
  assert(strstr((const char *)stylesheet.data, "@media print{") != NULL);
  assert(strstr((const char *)stylesheet.data, ".leaf") != NULL);
  laghu_runtime_css_result_release(&stylesheet);
  {
    static const unsigned char cycle_a[] = "@import '/cycle-b.css';";
    static const unsigned char cycle_b[] = "@import '/cycle-a.css';";
    assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){cycle_a, sizeof(cycle_a) - 1U}, "/cycle-a.css", "https://example.test", policy_key,
                                     0x55aaU, 900014U, 604800U, true, true, 2048U, 8192U, &stylesheet));
    assert(stylesheet.dependencies_pending && !stylesheet.rewritten);
    laghu_runtime_css_result_release(&stylesheet);
    assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){cycle_b, sizeof(cycle_b) - 1U}, "/cycle-b.css", "https://example.test", policy_key,
                                     0x55aaU, 900015U, 604800U, true, true, 2048U, 8192U, &stylesheet));
    assert(!stylesheet.rewritten && !stylesheet.dependencies_pending);
    laghu_runtime_css_result_release(&stylesheet);
  }
  {
    static const unsigned char linked[] =
        "<html><head><link rel=\"stylesheet\" href=\"/site.css\"></head>"
        "<body></body></html>";
    static const unsigned char nonce_linked[] =
        "<html><head><link rel=\"stylesheet\" href=\"/site.css\" "
        "nonce=\"c2FmZQ==\"></head><body></body></html>";
    static laghu_csp_policy nonce_csp;
    assert(laghu_runtime_rewrite_css_markup(path, (laghu_buffer){linked, sizeof(linked) - 1U}, "/index.html", "https://example.test", policy_key,
                                            0x55aaU, 900002U, 604800U, true, false, false, 0U, true, true, 2048U, 8192U, &markup));
    assert(!markup.rewritten && markup.dependencies_pending);
    laghu_runtime_html_result_release(&markup);
    assert(laghu_runtime_rewrite_css_markup(path, (laghu_buffer){linked, sizeof(linked) - 1U}, "/index.html", "https://example.test", policy_key,
                                            0x55aaU, 900003U, 604800U, true, false, false, 0U, true, true, 2048U, 8192U, &markup));
    assert(markup.rewritten && !markup.dependencies_pending);
    assert(strstr((const char *)markup.data, "<style>") != NULL);
    assert(strstr((const char *)markup.data, "href=") == NULL);
    laghu_runtime_html_result_release(&markup);
    assert(laghu_runtime_rewrite_css_markup(path, (laghu_buffer){linked, sizeof(linked) - 1U}, "/index.html", "https://example.test", policy_key,
                                            0x55aaU, 900003U, 604800U, true, false, false, 0U, false, true, 2048U, 8192U, &markup));
    assert(!markup.rewritten && !markup.dependencies_pending);
    laghu_runtime_html_result_release(&markup);
    laghu_csp_policy_init(&nonce_csp, "https://example.test");
    assert(laghu_csp_policy_add(&nonce_csp, "style-src 'self' 'nonce-c2FmZQ=='", sizeof("style-src 'self' 'nonce-c2FmZQ=='") - 1U));
    assert(laghu_csp_allows_inline_style(&nonce_csp, (const unsigned char *)"c2FmZQ==", 8U));
    assert(laghu_runtime_rewrite_css_markup_csp(path, (laghu_buffer){nonce_linked, sizeof(nonce_linked) - 1U}, "/index.html", "https://example.test",
                                                policy_key, 0x55aaU, 900003U, 604800U, true, false, false, 0U, &nonce_csp, 2048U, 8192U, &markup));
    assert(!markup.rewritten && markup.dependencies_pending);
    laghu_runtime_html_result_release(&markup);
    assert(laghu_runtime_rewrite_css_markup_csp(path, (laghu_buffer){nonce_linked, sizeof(nonce_linked) - 1U}, "/index.html", "https://example.test",
                                                policy_key, 0x55aaU, 900004U, 604800U, true, false, false, 0U, &nonce_csp, 2048U, 8192U, &markup));
    assert(markup.rewritten && strstr((const char *)markup.data, "<style nonce=\"c2FmZQ==\">") != NULL);
    laghu_runtime_html_result_release(&markup);
  }
  {
    static const unsigned char long_leaf_css[] = ".linked { color: green; margin: 0  0  0  0; }";
    char long_path[256U];
    char import_html[512U];
    size_t index;
    int length;
    long_path[0] = '/';
    for (index = 1U; index < 190U; ++index) long_path[index] = 'a';
    strcpy(long_path + 190U, ".css");
    assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){long_leaf_css, sizeof(long_leaf_css) - 1U}, long_path, "https://example.test",
                                     policy_key, 0x55aaU, 900020U, 604800U, true, false, 2048U, 8192U, &stylesheet));
    laghu_runtime_css_result_release(&stylesheet);
    assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){long_leaf_css, sizeof(long_leaf_css) - 1U}, long_path, "https://example.test",
                                     policy_key, 0x55aaU, 900021U, 604800U, true, false, 2048U, 8192U, &stylesheet));
    assert(stylesheet.rewritten);
    laghu_runtime_css_result_release(&stylesheet);
    length = snprintf(import_html, sizeof(import_html), "<style>@import \"%s\" print;</style>", long_path);
    assert(length > 0 && (size_t)length < sizeof(import_html));
    assert(laghu_runtime_rewrite_css_markup(path, (laghu_buffer){(const unsigned char *)import_html, (size_t)length}, "/index.html",
                                            "https://example.test", policy_key, 0x55aaU, 900022U, 604800U, false, true, false, 0U, false, true, 2048U,
                                            8192U, &markup));
    assert(!markup.rewritten && markup.dependencies_pending);
    laghu_runtime_html_result_release(&markup);
    assert(laghu_runtime_rewrite_css_markup(path, (laghu_buffer){(const unsigned char *)import_html, (size_t)length}, "/index.html",
                                            "https://example.test", policy_key, 0x55aaU, 900023U, 604800U, false, true, false, 0U, false, true, 2048U,
                                            8192U, &markup));
    assert(markup.rewritten && strstr((const char *)markup.data, "<link rel=\"stylesheet\"") != NULL);
    assert(strstr((const char *)markup.data, "media=\"print\"") != NULL);
    assert(strstr((const char *)markup.data, "@import") == NULL);
    laghu_runtime_html_result_release(&markup);
  }
}

static void test_css_outline_combine_and_csp(const char *path, const char *policy_key) {
  unsigned char *outlined = malloc(10000U);
  unsigned char combined_body[256U];
  laghu_runtime_html_result markup;
  laghu_runtime_css_result sheet;
  laghu_runtime_css_combine_result combined;
  laghu_runtime_cache_entry combined_entry;
  size_t offset = 0U;
  static const unsigned char first_css[] = ".first { color: red; padding: 0  0  0  0; }\n";
  static const unsigned char second_css[] = ".second { color: blue; margin: 0  0  0  0; }\n";
  static const unsigned char combined_html[] =
      "<html><head><link rel=\"stylesheet\" href=\"/first.css\"> \n"
      "<link rel=\"stylesheet\" href=\"/second.css\"></head></html>";
  static const unsigned char split_html[] =
      "<link rel=\"stylesheet\" href=\"/first.css\"><!-- split -->"
      "<link rel=\"stylesheet\" href=\"/second.css\">";
  static const unsigned char media_html[] =
      "<link rel=\"stylesheet\" href=\"/first.css\" media=\"print\">"
      "<link rel=\"stylesheet\" href=\"/second.css\" media=\"screen\">";
  char first_key[LAGHU_RUNTIME_KEY_SIZE], changed_key[LAGHU_RUNTIME_KEY_SIZE];

  assert(outlined != NULL);
  memcpy(outlined + offset, "<style>", 7U);
  offset += 7U;
  while (offset + 32U < 9980U) {
    memcpy(outlined + offset, ".x { color: red; margin: 0 0; } ", 32U);
    offset += 32U;
  }
  memcpy(outlined + offset, "</style>", 8U);
  offset += 8U;
  assert(laghu_runtime_rewrite_css_markup(path, (laghu_buffer){outlined, offset}, "/outline.html", "https://example.test", policy_key, 0x55aaU,
                                          900003U, 604800U, false, true, false, 0U, false, true, 2048U, 8192U, &markup));
  assert(!markup.rewritten && markup.dependencies_pending);
  laghu_runtime_html_result_release(&markup);
  assert(laghu_runtime_rewrite_css_markup(path, (laghu_buffer){outlined, offset}, "/outline.html", "https://example.test", policy_key, 0x55aaU,
                                          900004U, 604800U, false, true, false, 0U, false, true, 2048U, 8192U, &markup));
  assert(!markup.rewritten && markup.dependencies_pending);
  laghu_runtime_html_result_release(&markup);
  assert(laghu_runtime_rewrite_css_markup(path, (laghu_buffer){outlined, offset}, "/outline.html", "https://example.test", policy_key, 0x55aaU,
                                          900005U, 604800U, false, true, false, 0U, false, true, 2048U, 8192U, &markup));
  assert(markup.rewritten && !markup.dependencies_pending);
  assert(strstr((const char *)markup.data, "/.laghu/css/") != NULL);
  laghu_runtime_html_result_release(&markup);
  free(outlined);

  assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){first_css, sizeof(first_css) - 1U}, "/first.css", "https://example.test", policy_key,
                                   0x55aaU, 910000U, 604800U, true, false, 2048U, 8192U, &sheet));
  laghu_runtime_css_result_release(&sheet);
  assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){first_css, sizeof(first_css) - 1U}, "/first.css", "https://example.test", policy_key,
                                   0x55aaU, 910001U, 604800U, true, false, 2048U, 8192U, &sheet));
  laghu_runtime_css_result_release(&sheet);
  assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){second_css, sizeof(second_css) - 1U}, "/second.css", "https://example.test", policy_key,
                                   0x55aaU, 910000U, 604800U, true, false, 2048U, 8192U, &sheet));
  laghu_runtime_css_result_release(&sheet);
  assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){second_css, sizeof(second_css) - 1U}, "/second.css", "https://example.test", policy_key,
                                   0x55aaU, 910001U, 604800U, true, false, 2048U, 8192U, &sheet));
  laghu_runtime_css_result_release(&sheet);
  assert(laghu_runtime_combine_css_markup(path, (laghu_buffer){combined_html, sizeof(combined_html) - 1U}, "/index.html", "https://example.test",
                                          policy_key, 0x55aaU, 910002U, 604800U, 2048U, 8192U, &combined));
  assert(combined.rewritten && !combined.dependencies_pending);
  {
    const char *route = strstr((const char *)combined.data, "/.laghu/css/");
    assert(route != NULL);
    memcpy(first_key, route + sizeof("/.laghu/css/") - 1U, LAGHU_SHA256_HEX_LENGTH);
    first_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
  }
  assert(laghu_runtime_cache_lookup_variant(path, first_key, &combined_entry));
  assert(combined_entry.length < sizeof(combined_body));
  assert(laghu_runtime_cache_read(&combined_entry, combined_body, sizeof(combined_body)));
  combined_body[combined_entry.length] = '\0';
  assert(strstr((const char *)combined_body, ".first") < strstr((const char *)combined_body, ".second"));
  laghu_runtime_css_combine_result_release(&combined);
  {
    static const unsigned char changed_css[] = ".first { color: green; padding: 0  0  0  0; }\n";
    assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){changed_css, sizeof(changed_css) - 1U}, "/first.css", "https://example.test",
                                     policy_key, 0x55aaU, 910004U, 604800U, true, false, 2048U, 8192U, &sheet));
    laghu_runtime_css_result_release(&sheet);
    assert(laghu_runtime_rewrite_css(NULL, path, (laghu_buffer){changed_css, sizeof(changed_css) - 1U}, "/first.css", "https://example.test",
                                     policy_key, 0x55aaU, 910005U, 604800U, true, false, 2048U, 8192U, &sheet));
    laghu_runtime_css_result_release(&sheet);
    assert(laghu_runtime_combine_css_markup(path, (laghu_buffer){combined_html, sizeof(combined_html) - 1U}, "/index.html", "https://example.test",
                                            policy_key, 0x55aaU, 910006U, 604800U, 2048U, 8192U, &combined));
    {
      const char *route = strstr((const char *)combined.data, "/.laghu/css/");
      assert(route != NULL);
      memcpy(changed_key, route + sizeof("/.laghu/css/") - 1U, LAGHU_SHA256_HEX_LENGTH);
      changed_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
    }
    assert(strcmp(first_key, changed_key) != 0);
    laghu_runtime_css_combine_result_release(&combined);
  }
  assert(laghu_runtime_combine_css_markup(path, (laghu_buffer){split_html, sizeof(split_html) - 1U}, "/index.html", "https://example.test",
                                          policy_key, 0x55aaU, 910003U, 604800U, 2048U, 8192U, &combined));
  assert(!combined.rewritten && !combined.dependencies_pending);
  laghu_runtime_css_combine_result_release(&combined);
  assert(laghu_runtime_combine_css_markup(path, (laghu_buffer){media_html, sizeof(media_html) - 1U}, "/index.html", "https://example.test",
                                          policy_key, 0x55aaU, 910003U, 604800U, 2048U, 8192U, &combined));
  assert(!combined.rewritten && !combined.dependencies_pending);
  laghu_runtime_css_combine_result_release(&combined);
  {
    static laghu_csp_policy csp;
    static const unsigned char nonce[] = "secret-value";
    static const unsigned char wrong[] = "wrong";
    assert(sizeof(csp) <= 2048U);
    laghu_csp_policy_init(&csp, "https://example.test");
    assert(laghu_csp_policy_add(&csp,
                                "default-src 'none'; img-src data:; style-src 'self' "
                                "'nonce-secret-value'; script-src 'nonce-secret-value' "
                                "'strict-dynamic' https:",
                                strlen("default-src 'none'; img-src data:; style-src 'self' "
                                       "'nonce-secret-value'; script-src 'nonce-secret-value' "
                                       "'strict-dynamic' https:")));
    assert(laghu_csp_allows_data_image(&csp));
    assert(laghu_csp_allows_inline_style(&csp, nonce, sizeof(nonce) - 1U));
    assert(!laghu_csp_allows_inline_style(&csp, wrong, sizeof(wrong) - 1U));
    assert(laghu_csp_allows_external_style(&csp, NULL, 0U));
    assert(laghu_csp_allows_inline_script(&csp, nonce, sizeof(nonce) - 1U));
    assert(!laghu_csp_allows_external_script(&csp, NULL, 0U));
    assert(laghu_csp_allows_external_script(&csp, nonce, sizeof(nonce) - 1U));
    {
      const unsigned char *bytes = (const unsigned char *)&csp;
      size_t index;
      for (index = 0U; index + sizeof(nonce) - 1U <= sizeof(csp); ++index) assert(memcmp(bytes + index, nonce, sizeof(nonce) - 1U) != 0);
    }
    assert(laghu_csp_policy_add(&csp, "img-src 'none'", sizeof("img-src 'none'") - 1U));
    assert(!laghu_csp_allows_data_image(&csp));
  }
  {
    static laghu_csp_policy csp;
    static const unsigned char meta[] =
        "<meta content = \"style-src 'none'; script-src 'self'\" "
        "HTTP-EQUIV = Content-Security-Policy>"
        "<meta http-equiv=\"Content-Security-Policy-Report-Only\" "
        "content=\"default-src 'none'\">";
    laghu_csp_policy_init(&csp, "https://example.test");
    assert(laghu_csp_policy_add_meta(&csp, (laghu_buffer){meta, sizeof(meta) - 1U}));
    assert(!laghu_csp_allows_inline_style(&csp, NULL, 0U));
    assert(laghu_csp_allows_external_script(&csp, NULL, 0U));
  }
  {
    static laghu_csp_policy csp;
    laghu_csp_policy_init(&csp, "https://example.test");
    assert(laghu_csp_policy_add(&csp,
                                "STYLE-SRC-ELEM 'none'; style-src-elem 'self'; "
                                "script-src 'unsafe-inline' 'sha256-YWJjZA=='",
                                sizeof("STYLE-SRC-ELEM 'none'; style-src-elem 'self'; "
                                       "script-src 'unsafe-inline' 'sha256-YWJjZA=='") -
                                    1U));
    assert(!laghu_csp_allows_external_style(&csp, NULL, 0U));
    assert(!laghu_csp_allows_inline_script(&csp, NULL, 0U));
    assert(laghu_csp_allows_data_image(&csp));
  }
  {
    static laghu_csp_policy csp;
    laghu_csp_policy_init(&csp, "https://app.example.test:8443");
    assert(laghu_csp_policy_add(&csp, "default-src 'self', style-src https:", sizeof("default-src 'self', style-src https:") - 1U));
    assert(laghu_csp_allows_external_style(&csp, NULL, 0U));
    assert(!laghu_csp_allows_inline_style(&csp, NULL, 0U));
    laghu_csp_policy_init(&csp, "https://app.example.test");
    assert(laghu_csp_policy_add(&csp, "style-src https://*.example.test/; script-src *.example.test",
                                sizeof("style-src https://*.example.test/; script-src *.example.test") - 1U));
    assert(laghu_csp_allows_external_style(&csp, NULL, 0U));
    assert(laghu_csp_allows_external_script(&csp, NULL, 0U));
    laghu_csp_policy_init(&csp, "https://app.example.test");
    assert(laghu_csp_policy_add(&csp, "style-src https://app.example.test/assets/", sizeof("style-src https://app.example.test/assets/") - 1U));
    assert(!laghu_csp_allows_external_style(&csp, NULL, 0U));
  }
  {
    static laghu_csp_policy csp;
    char oversized[8193U];
    laghu_csp_policy_init(&csp, "https://example.test");
    assert(!laghu_csp_policy_add(&csp, "script-src 'nonce-@@@'", sizeof("script-src 'nonce-@@@'") - 1U));
    assert(!laghu_csp_allows_external_script(&csp, NULL, 0U));
    memset(oversized, 'x', sizeof(oversized));
    laghu_csp_policy_init(&csp, "https://example.test");
    assert(!laghu_csp_policy_add(&csp, oversized, sizeof(oversized)));
    laghu_csp_policy_init(&csp, "https://example.test");
    assert(!laghu_csp_policy_add(&csp,
                                 "img-src data:, img-src data:, img-src data:, img-src data:, "
                                 "img-src data:",
                                 sizeof("img-src data:, img-src data:, img-src data:, img-src data:, "
                                        "img-src data:") -
                                     1U));
    assert(!laghu_csp_allows_data_image(&csp));
  }
}

static void test_critical_css(laghu_rum_engine *rum, const char *path, const char *policy_key) {
  static const unsigned char css[] =
      ":root{--tone:red;color-scheme:light "
      "dark}.hero{color:red}.footer{color:"
      "blue}@media(prefers-color-scheme:dark){.hero{color:white}}@font-face{"
      "font-family:x;src:url(x.woff2)}";
  static const unsigned char document[] =
      "<html><head><link rel=\"stylesheet\" href=\"/critical.css\"></head>"
      "<body><div class=hero>hero</div></body></html>";
  laghu_stylesheet_record stylesheet = {0};
  laghu_runtime_cache_entry entry;
  laghu_runtime_html_result page;
  laghu_critical_css_beacon observation = {0};
  char css_key[LAGHU_RUNTIME_KEY_SIZE];
  const char *marker;
  unsigned int index;

  assert(laghu_sha256_hex((laghu_buffer){css, sizeof(css) - 1U}, css_key));
  assert(laghu_runtime_cache_publish(path, css_key, css_key, css_key, "text/css", "critical-test", (laghu_buffer){css, sizeof(css) - 1U}, &entry));
  stylesheet.version = LAGHU_STYLESHEET_CATALOG_VERSION;
  strcpy(stylesheet.normalized_url, "/critical.css");
  strcpy(stylesheet.source_hash, css_key);
  strcpy(stylesheet.source_key, css_key);
  strcpy(stylesheet.derived_key, css_key);
  strcpy(stylesheet.dependency_key, css_key);
  strcpy(stylesheet.policy_key, policy_key);
  stylesheet.capability_mask = 0x55aaU;
  stylesheet.parser_version = LAGHU_CSS_DERIVATION_VERSION;
  stylesheet.inline_limit = 2048U;
  stylesheet.outline_threshold = 8192U;
  stylesheet.source_length = sizeof(css) - 1U;
  stylesheet.derived_length = sizeof(css) - 1U;
  stylesheet.updated_at = 2000U;
  stylesheet.ready = true;
  assert(laghu_stylesheet_publish(path, &stylesheet));
  assert(laghu_runtime_prioritize_critical_css(rum, path, (laghu_buffer){document, sizeof(document) - 1U}, "/critical", "https://example.test",
                                               policy_key, 0x55aaU, 2001U, 604800U, 2048U, 8192U, 1024U, true, NULL, &page));
  assert(!page.rewritten);
  laghu_runtime_html_result_release(&page);
  assert(laghu_runtime_prioritize_critical_css(rum, path, (laghu_buffer){document, sizeof(document) - 1U}, "/critical", "https://example.test",
                                               policy_key, 0x55aaU, 2001U, 604800U, 2048U, 8192U, 1024U, true, NULL, &page));
  assert(page.rewritten);
  marker = strstr((const char *)page.data, "data-laghu-critical=\"");
  assert(marker != NULL);
  marker += sizeof("data-laghu-critical=\"") - 1U;
  memcpy(observation.template_key, marker, LAGHU_SHA256_HEX_LENGTH);
  observation.template_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
  observation.viewport_bucket = 1U;
  observation.rule_count = 1U;
  observation.rules[0] = 0U;
  laghu_runtime_html_result_release(&page);
  for (index = 0U; index < 3U; ++index) assert(laghu_critical_css_apply_beacon(rum, path, policy_key, 2002U + index, 604800U, &observation));
  assert(laghu_runtime_prioritize_critical_css(rum, path, (laghu_buffer){document, sizeof(document) - 1U}, "/critical", "https://example.test",
                                               policy_key, 0x55aaU, 2005U, 604800U, 2048U, 8192U, 1024U, true, NULL, &page));
  assert(strstr((const char *)page.data, "<style>") == NULL);
  laghu_runtime_html_result_release(&page);
  observation.color_scheme_bucket = 1U;
  for (index = 0U; index < 3U; ++index) assert(laghu_critical_css_apply_beacon(rum, path, policy_key, 2005U + index, 604800U, &observation));
  assert(laghu_runtime_prioritize_critical_css(rum, path, (laghu_buffer){document, sizeof(document) - 1U}, "/critical", "https://example.test",
                                               policy_key, 0x55aaU, 2008U, 604800U, 2048U, 8192U, 1024U, true, NULL, &page));
  assert(page.rewritten);
  assert(strstr((const char *)page.data, "<style>:root{--tone:red;color-scheme:light dark}") != NULL);
  assert(strstr((const char *)page.data, "@media(prefers-color-scheme:dark){.hero{color:white}}") != NULL);
  assert(strstr((const char *)page.data, "</style></head><body>") != NULL);
  assert(strstr((const char *)page.data, "/.laghu/css/") != NULL);
  laghu_runtime_html_result_release(&page);
  {
    static const unsigned char json[] =
        "{\"template\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaa\",\"bucket\":0,\"scheme\":1,\"rules\":[0,7]}";
    laghu_critical_css_beacon parsed;
    assert(laghu_runtime_parse_critical_css_beacon((laghu_buffer){json, sizeof(json) - 1U}, &parsed));
    assert(parsed.viewport_bucket == 0U && parsed.color_scheme_bucket == 1U && parsed.rule_count == 2U && parsed.rules[1] == 7U);
  }
}

int main(void) {
  static const unsigned char payload[] = "css-policy";
  laghu_test_workspace workspace;
  laghu_rum_options options;
  laghu_rum_engine *rum;
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];

  assert(laghu_test_workspace_create(&workspace));
  assert(laghu_sha256_hex((laghu_buffer){payload, sizeof(payload) - 1U}, policy_key));
  laghu_rum_options_init(&options);
  rum = laghu_rum_engine_create(&options, NULL, 0U);
  assert(rum != NULL);
  test_font_css(&workspace);
  test_css_derivation(workspace.path, policy_key);
  test_css_outline_combine_and_csp(workspace.path, policy_key);
  test_critical_css(rum, workspace.path, policy_key);
  laghu_rum_engine_destroy(rum);
  assert(laghu_test_workspace_remove(&workspace));
  puts("laghu_runtime_css_critical_integration_test: all tests passed");
  return 0;
}
