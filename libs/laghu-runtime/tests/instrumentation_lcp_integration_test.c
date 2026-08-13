// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/cache.h"
#include "laghu/catalog.h"
#include "laghu/csp.h"
#include "laghu/css.h"
#include "laghu/html.h"
#include "laghu/instrumentation.h"
#include "laghu/javascript.h"
#include "laghu/lcp.h"
#include "laghu/rum.h"
#include "laghu/types.h"
#include "test_fixture.h"

static void test_lcp_prioritization(laghu_rum_engine *rum) {
  static const unsigned char evidence_html[] =
      "<html><body><nav><img src=/logo.png width=40 height=40></nav>"
      "<img src=/hero.jpg width=800 height=600><img src=/later.jpg>"
      "</body></html>";
  static const unsigned char selected_html[] =
      "<html><body><nav><img src=/logo.png width=40 height=40 loading=lazy>"
      "</nav><img src=/hero.jpg width=800 height=600 loading=lazy>"
      "<img src=/later.jpg loading=lazy></body></html>";
  static const unsigned char responsive_html[] =
      "<html><body><nav><img src=/logo.png width=40 height=40 loading=lazy>"
      "</nav><img src=.laghu/image/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaa width=800 height=600 "
      "srcset=\".laghu/image/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaa 800w, .laghu/image/bbbbbbbbbbbbbbbbbbbbbbbbbbbb"
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb 1200w\" sizes=100vw "
      "loading=lazy><img src=/later.jpg loading=lazy></body></html>";
  static const char template_key[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  laghu_rum_instrumentation_record record = {0};
  laghu_lcp_result result;
  char digest[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int bucket;
  laghu_csp_policy csp;

  laghu_csp_policy_init(&csp, "https://example.test");
  assert(
      laghu_lcp_inventory_record((laghu_buffer){evidence_html, sizeof(evidence_html) - 1U}, "/index.html", "https://example.test", &record, digest));
  assert(record.media_count == 3U && record.media_kind[0] == 0U && record.media_kind[1] == 1U && strlen(digest) == 64U);
  assert(laghu_runtime_prioritize_lcp(rum, (laghu_buffer){evidence_html, sizeof(evidence_html) - 1U},
                                      (laghu_buffer){selected_html, sizeof(selected_html) - 1U}, "/index.html", "https://example.test", NULL, 100U,
                                      60U, 640U, true, true, &csp, &result));
  assert(result.decision == LAGHU_LCP_DECISION_HEURISTIC && result.applied && result.rewritten);
  assert(strstr((const char *)result.data, "src=/hero.jpg") != NULL);
  assert(strstr((const char *)result.data, "fetchpriority=\"high\"") != NULL);
  assert(strstr((const char *)result.data, "src=/hero.jpg width=800 height=600 loading=lazy") == NULL);
  assert(strcmp(result.link_header, "</hero.jpg>; rel=preload; as=image") == 0);
  laghu_lcp_result_release(&result);
  assert(laghu_runtime_prioritize_learned_lcp(rum, (laghu_buffer){evidence_html, sizeof(evidence_html) - 1U},
                                              (laghu_buffer){selected_html, sizeof(selected_html) - 1U}, "/index.html", "https://example.test",
                                              template_key, 100U, 60U, 640U, true, true, &csp, &result));
  assert(result.decision == LAGHU_LCP_DECISION_STALE && !result.applied && !result.rewritten);
  laghu_lcp_result_release(&result);
  assert(laghu_runtime_prioritize_lcp(rum, (laghu_buffer){evidence_html, sizeof(evidence_html) - 1U},
                                      (laghu_buffer){responsive_html, sizeof(responsive_html) - 1U}, "/index.html", "https://example.test", NULL,
                                      100U, 60U, 640U, true, true, &csp, &result));
  assert(result.decision == LAGHU_LCP_DECISION_UNRESOLVED && !result.applied);
  laghu_lcp_result_release(&result);

  record.version = LAGHU_INSTRUMENTATION_VERSION;
  strcpy(record.template_key, template_key);
  record.updated_at = 100U;
  for (bucket = 0U; bucket < 2U; ++bucket) {
    record.lcp_observations[bucket] = 3U;
    record.lcp_candidates[bucket][1] = 3U;
    record.lcp_resources[bucket][1][0] = 3U;
  }
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, template_key, 100U, &record, sizeof(record), NULL));
  assert(laghu_runtime_prioritize_lcp(rum, (laghu_buffer){evidence_html, sizeof(evidence_html) - 1U},
                                      (laghu_buffer){selected_html, sizeof(selected_html) - 1U}, "/index.html", "https://example.test", template_key,
                                      101U, 60U, 640U, true, true, &csp, &result));
  assert(result.decision == LAGHU_LCP_DECISION_LEARNED && result.applied && result.observations == 6U && result.profile_ready[0] &&
         result.profile_ready[1]);
  laghu_lcp_result_release(&result);
  record.lcp_candidates[1][1] = 0U;
  record.lcp_candidates[1][2] = 3U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, template_key, 102U, &record, sizeof(record), NULL));
  assert(laghu_runtime_prioritize_lcp(rum, (laghu_buffer){evidence_html, sizeof(evidence_html) - 1U},
                                      (laghu_buffer){selected_html, sizeof(selected_html) - 1U}, "/index.html", "https://example.test", template_key,
                                      103U, 60U, 640U, true, true, &csp, &result));
  assert(result.decision == LAGHU_LCP_DECISION_HEURISTIC);
  laghu_lcp_result_release(&result);
  {
    static const unsigned char picture_html[] =
        "<html><body><picture><source srcset=\"/hero-1.webp 1x, "
        "/hero-2.webp 2x\" sizes=\"100vw\"><img src=/hero.jpg "
        "width=800 height=600 loading=lazy></picture></body></html>";
    static const char picture_key[] = "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    memset(&record, 0, sizeof(record));
    assert(laghu_lcp_inventory_record((laghu_buffer){picture_html, sizeof(picture_html) - 1U}, "/picture", "https://example.test", &record, digest));
    assert(record.media_count == 1U && record.media_resource_count[0] == 3U);
    record.version = LAGHU_INSTRUMENTATION_VERSION;
    strcpy(record.template_key, picture_key);
    record.updated_at = 200U;
    for (bucket = 0U; bucket < 2U; ++bucket) {
      record.lcp_observations[bucket] = 3U;
      record.lcp_candidates[bucket][0] = 3U;
      record.lcp_resources[bucket][0][1] = 3U;
    }
    assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, picture_key, 200U, &record, sizeof(record), NULL));
    assert(laghu_runtime_prioritize_lcp(rum, (laghu_buffer){picture_html, sizeof(picture_html) - 1U},
                                        (laghu_buffer){picture_html, sizeof(picture_html) - 1U}, "/picture", "https://example.test", picture_key,
                                        201U, 60U, 640U, true, true, &csp, &result));
    assert(result.decision == LAGHU_LCP_DECISION_LEARNED && result.applied);
    assert(strstr(result.link_header, "</hero-2.webp>") != NULL &&
           strstr(result.link_header, "imagesrcset=\"/hero-1.webp 1x, /hero-2.webp 2x\"") != NULL);
    assert(strstr((const char *)result.data, "fetchpriority=\"high\"") != NULL);
    laghu_lcp_result_release(&result);
  }
  {
    static const unsigned char video_html[] = "<html><body><video poster=/poster.jpg></video></body></html>";
    static const char video_key[] = "2123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    memset(&record, 0, sizeof(record));
    assert(laghu_lcp_inventory_record((laghu_buffer){video_html, sizeof(video_html) - 1U}, "/video", "https://example.test", &record, digest));
    record.version = LAGHU_INSTRUMENTATION_VERSION;
    strcpy(record.template_key, video_key);
    record.updated_at = 300U;
    for (bucket = 0U; bucket < 2U; ++bucket) {
      record.lcp_observations[bucket] = 3U;
      record.lcp_candidates[bucket][0] = 3U;
      record.lcp_resources[bucket][0][0] = 3U;
    }
    assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, video_key, 300U, &record, sizeof(record), NULL));
    assert(laghu_runtime_prioritize_lcp(rum, (laghu_buffer){video_html, sizeof(video_html) - 1U}, (laghu_buffer){video_html, sizeof(video_html) - 1U},
                                        "/video", "https://example.test", video_key, 301U, 60U, 640U, true, true, &csp, &result));
    assert(result.decision == LAGHU_LCP_DECISION_LEARNED && result.applied && !result.rewritten &&
           strcmp(result.link_header, "</poster.jpg>; rel=preload; as=image") == 0);
    laghu_lcp_result_release(&result);
  }
  laghu_csp_policy_init(&csp, "https://example.test");
  assert(laghu_csp_policy_add(&csp, "img-src 'none'", sizeof("img-src 'none'") - 1U));
  assert(laghu_runtime_prioritize_lcp(rum, (laghu_buffer){evidence_html, sizeof(evidence_html) - 1U},
                                      (laghu_buffer){selected_html, sizeof(selected_html) - 1U}, "/index.html", "https://example.test", NULL, 400U,
                                      60U, 640U, true, true, &csp, &result));
  assert(result.decision == LAGHU_LCP_DECISION_CONFLICT && !result.applied && result.link_header == NULL);
  laghu_lcp_result_release(&result);
}

static void test_image_markup_and_headers(laghu_rum_engine *rum, const char *temporary, const char *policy_key) {
  static const unsigned char html[] = "<html><body><img src=\"/image.png\" width=\"320\"></body></html>";
  static const unsigned char inline_html[] =
      "<img src=\"/image.png\" width=\"320\"><img src=\"/image.png\" "
      "width=\"320\">";
  static const unsigned char tiny_png[] = {0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU};
  char policy_payload[LAGHU_RUNTIME_KEY_SIZE];
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char no_webp_index_key[LAGHU_RUNTIME_KEY_SIZE];
  char first_key[LAGHU_RUNTIME_KEY_SIZE];
  char second_key[LAGHU_RUNTIME_KEY_SIZE];
  laghu_runtime_cache_entry entry;
  laghu_catalog_record catalog = {0};
  laghu_runtime_html_result page;
  static laghu_csp_policy deny_data_csp;

  assert(laghu_sha256_hex((laghu_buffer){(const unsigned char *)"runtime payload", sizeof("runtime payload") - 1U}, policy_payload));
  assert(strcmp(policy_payload, policy_key) == 0);
  assert(laghu_runtime_index_key("/image.png", "etag", policy_key, true, false, 0U, 0U, index_key));
  assert(laghu_runtime_index_key("/image.png", "etag", policy_key, false, false, 0U, 0U, no_webp_index_key));
  laghu_csp_policy_init(&deny_data_csp, "https://example.test");
  assert(laghu_csp_policy_add(&deny_data_csp, "img-src 'none'", sizeof("img-src 'none'") - 1U));
  assert(laghu_sha256_hex((laghu_buffer){tiny_png, sizeof(tiny_png)}, first_key));
  assert(laghu_sha256_hex((laghu_buffer){(const unsigned char *)"second", 6U}, second_key));
  assert(laghu_runtime_cache_publish(temporary, no_webp_index_key, first_key, "page", "image/png", "test-backend",
                                     (laghu_buffer){tiny_png, sizeof(tiny_png)}, &entry));
  assert(laghu_runtime_cache_publish(temporary, index_key, second_key, "page", "image/png", "test-backend",
                                     (laghu_buffer){(const unsigned char *)"runtime payload", sizeof("runtime payload") - 1U}, &entry));
  catalog.version = LAGHU_CATALOG_VERSION;
  strcpy(catalog.normalized_url, "/image.png");
  strcpy(catalog.source_hash, index_key);
  strcpy(catalog.policy_key, policy_key);
  catalog.capability_mask = 0x55aaU;
  catalog.natural_width = 1000U;
  catalog.natural_height = 500U;
  catalog.variant_count = 2U;
  catalog.updated_at = 2000U;
  catalog.last_accessed_at = 2000U;
  catalog.variants[0].width = 320U;
  catalog.variants[0].height = 160U;
  strcpy(catalog.variants[0].variant_key, first_key);
  catalog.variants[0].original_length = 1000U;
  catalog.variants[0].variant_length = sizeof(tiny_png);
  catalog.variants[0].ready = true;
  catalog.variants[1].width = 640U;
  catalog.variants[1].height = 320U;
  strcpy(catalog.variants[1].variant_key, second_key);
  catalog.variants[1].original_length = 1000U;
  catalog.variants[1].variant_length = sizeof("runtime payload") - 1U;
  catalog.variants[1].ready = true;
  assert(laghu_catalog_publish_url(temporary, &catalog));
  {
    static const unsigned char hinted_html[] =
        "<html><head><meta http-equiv=Content-Language content=en-CA>"
        "<link rel=stylesheet href=/app.css></head><body>"
        "<img src=/image.png fetchpriority=high>"
        "<script src=https://cdn.example.test/app.js></script></body></html>";
    static const unsigned char css[] = "body{color:#000}";
    laghu_stylesheet_record stylesheet;
    laghu_runtime_html_result hints;
    char css_key[LAGHU_RUNTIME_KEY_SIZE];
    assert(laghu_sha256_hex((laghu_buffer){css, sizeof(css) - 1U}, css_key));
    assert(laghu_runtime_cache_publish(temporary, css_key, css_key, css_key, "text/css", "test-css", (laghu_buffer){css, sizeof(css) - 1U}, &entry));
    memset(&stylesheet, 0, sizeof(stylesheet));
    stylesheet.version = LAGHU_STYLESHEET_CATALOG_VERSION;
    strcpy(stylesheet.normalized_url, "/app.css");
    strcpy(stylesheet.source_hash, css_key);
    strcpy(stylesheet.source_key, css_key);
    strcpy(stylesheet.derived_key, css_key);
    strcpy(stylesheet.dependency_key, css_key);
    strcpy(stylesheet.policy_key, policy_key);
    stylesheet.capability_mask = 0x55aaU;
    stylesheet.parser_version = 1U;
    stylesheet.inline_limit = 2048U;
    stylesheet.outline_threshold = 8192U;
    stylesheet.source_length = sizeof(css) - 1U;
    stylesheet.derived_length = sizeof(css) - 1U;
    stylesheet.updated_at = 2000U;
    stylesheet.ready = true;
    assert(laghu_stylesheet_publish(temporary, &stylesheet));
    assert(laghu_runtime_finalize_html_headers(
        temporary, (laghu_buffer){hinted_html, sizeof(hinted_html) - 1U}, "/index.html", "https://example.test", policy_key, 0x55aaU, 2001U, 604800U,
        LAGHU_HTML_PLAN_CONVERT_META_TAGS | LAGHU_HTML_PLAN_RESOURCE_HINTS, NULL, NULL, 2048U, 8192U, false, &hints));
    assert(hints.dependencies_pending && !hints.rewritten && hints.link_header_count == 0U);
    laghu_runtime_html_result_release(&hints);
    assert(laghu_runtime_finalize_html_headers(
        temporary, (laghu_buffer){hinted_html, sizeof(hinted_html) - 1U}, "/index.html", "https://example.test", policy_key, 0x55aaU, 2001U, 604800U,
        LAGHU_HTML_PLAN_CONVERT_META_TAGS | LAGHU_HTML_PLAN_RESOURCE_HINTS, NULL, NULL, 2048U, 8192U, false, &hints));
    assert(hints.rewritten && hints.set_content_language);
    assert(strcmp(hints.content_language, "en-CA") == 0);
    assert(hints.link_header_count == 4U);
    assert(strstr(hints.link_headers[0], "/.laghu/css/") != NULL);
    assert(strstr(hints.link_headers[1], "/.laghu/image/") != NULL);
    assert(strcmp(hints.link_headers[2], "<https://cdn.example.test>; rel=preconnect") == 0);
    assert(strcmp(hints.link_headers[3], "<https://cdn.example.test>; rel=dns-prefetch") == 0);
    assert(strstr((const char *)hints.data, "Content-Language") == NULL);
    laghu_runtime_html_result_release(&hints);
    assert(laghu_runtime_finalize_html_headers(temporary, (laghu_buffer){hinted_html, sizeof(hinted_html) - 1U}, "/index.html",
                                               "https://example.test", policy_key, 0x55aaU, 2001U, 604800U, LAGHU_HTML_PLAN_CONVERT_META_TAGS,
                                               "fr-CA", NULL, 2048U, 8192U, true, &hints));
    assert(hints.invalid && !hints.rewritten && !hints.set_content_language && hints.link_header_count == 0U);
    laghu_runtime_html_result_release(&hints);
    {
      static const unsigned char deduplicated[] =
          "<html><head><meta http-equiv=content-language content=en>"
          "<link rel=preconnect href=https://cdn.example.test></head>"
          "<body><script src=https://cdn.example.test/a.js></script></body>"
          "</html>";
      assert(laghu_runtime_finalize_html_headers(
          temporary, (laghu_buffer){deduplicated, sizeof(deduplicated) - 1U}, "/index.html", "https://example.test", policy_key, 0x55aaU, 2001U,
          604800U, LAGHU_HTML_PLAN_CONVERT_META_TAGS | LAGHU_HTML_PLAN_RESOURCE_HINTS, NULL, NULL, 2048U, 8192U, true, &hints));
      assert(hints.rewritten && hints.set_content_language && hints.link_header_count == 0U);
      laghu_runtime_html_result_release(&hints);
      assert(laghu_runtime_finalize_html_headers(
          temporary, (laghu_buffer){deduplicated, sizeof(deduplicated) - 1U}, "/index.html", "https://example.test", policy_key, 0x55aaU, 2001U,
          604800U, LAGHU_HTML_PLAN_CONVERT_META_TAGS | LAGHU_HTML_PLAN_RESOURCE_HINTS, NULL, "malformed", 2048U, 8192U, true, &hints));
      assert(hints.rewritten && hints.set_content_language && hints.link_header_count == 0U);
      laghu_runtime_html_result_release(&hints);
    }
    {
      static const unsigned char bounded[] =
          "<html><head><link rel=preconnect "
          "href=https://reserved.example.test></head><body>"
          "<script src=https://cdn-1.example.test/a.js?token=secret#part>"
          "</script><script src=https://CDN-1.example.test:443/repeat.js>"
          "</script><script src=https://example.test/same.js></script>"
          "<script src=https://user:pass@credentials.example.test/a.js>"
          "</script><script src=https://cdn-2.example.test/a.js></script>"
          "<script src=https://cdn-3.example.test/a.js></script>"
          "<script src=https://cdn-4.example.test/a.js></script>"
          "<script src=https://cdn-5.example.test/a.js></script>"
          "<script src=https://cdn-6.example.test/a.js></script>"
          "<script src=https://cdn-7.example.test/a.js></script>"
          "<script src=https://cdn-8.example.test/a.js></script>"
          "<script src=https://cdn-9.example.test/a.js></script></body></html>";
      assert(laghu_runtime_finalize_html_headers(temporary, (laghu_buffer){bounded, sizeof(bounded) - 1U}, "/index.html", "https://example.test",
                                                 policy_key, 0x55aaU, 2001U, 604800U, LAGHU_HTML_PLAN_RESOURCE_HINTS, NULL, NULL, 2048U, 8192U, true,
                                                 &hints));
      assert(!hints.invalid && !hints.rewritten && hints.link_header_count == 12U);
      assert(hints.length == sizeof(bounded) - 1U && memcmp(hints.data, bounded, sizeof(bounded) - 1U) == 0);
      assert(strcmp(hints.link_headers[0], "<https://cdn-1.example.test>; rel=preconnect") == 0);
      assert(strcmp(hints.link_headers[1], "<https://cdn-1.example.test>; rel=dns-prefetch") == 0);
      assert(strcmp(hints.link_headers[6], "<https://cdn-4.example.test>; rel=preconnect") == 0);
      assert(strcmp(hints.link_headers[7], "<https://cdn-4.example.test>; rel=dns-prefetch") == 0);
      assert(strcmp(hints.link_headers[8], "<https://cdn-5.example.test>; rel=dns-prefetch") == 0);
      assert(strcmp(hints.link_headers[11], "<https://cdn-8.example.test>; rel=dns-prefetch") == 0);
      assert(strstr(hints.link_headers[0], "token") == NULL && strstr(hints.link_headers[0], "?") == NULL &&
             strstr(hints.link_headers[0], "#") == NULL && strstr(hints.link_headers[0], "credentials") == NULL &&
             strstr(hints.link_headers[0], "example.test/same") == NULL);
      laghu_runtime_html_result_release(&hints);
    }
  }
  assert(laghu_runtime_rewrite_html(rum, temporary, (laghu_buffer){html, sizeof(html) - 1U}, "/index.html", "https://example.test", policy_key,
                                    0x55aaU, 2001U, 604800U,
                                    LAGHU_IMAGE_INSERT_DIMENSIONS | LAGHU_IMAGE_RESPONSIVE | LAGHU_IMAGE_RESPONSIVE_ZOOM | LAGHU_IMAGE_LAZYLOAD,
                                    false, false, false, false, 0U, NULL, false, 2048U, 2048U, 8192U, 0U, 100U, &page));
  assert(page.rewritten && !page.dependencies_pending);
  assert(strstr((const char *)page.data, "/.laghu/image/") != NULL);
  assert(strstr((const char *)page.data, " 320w") != NULL);
  assert(strstr((const char *)page.data, " 640w") != NULL);
  laghu_runtime_html_result_release(&page);
  assert(laghu_runtime_rewrite_html(rum, temporary, (laghu_buffer){inline_html, sizeof(inline_html) - 1U}, "/index.html", "https://example.test",
                                    policy_key, 0x55aaU, 2001U, 604800U, LAGHU_IMAGE_INLINE | LAGHU_IMAGE_DEDUP_INLINE, true, false, false, false, 0U,
                                    NULL, false, 2048U, 2048U, 8192U, 0U, 100U, &page));
  assert(page.rewritten);
  assert(strstr((const char *)page.data, "data:image/png;base64,") != NULL);
  assert(strstr((const char *)page.data, "/.laghu/image/") != NULL);
  laghu_runtime_html_result_release(&page);
  assert(laghu_runtime_rewrite_html(rum, temporary, (laghu_buffer){inline_html, sizeof(inline_html) - 1U}, "/index.html", "https://example.test",
                                    policy_key, 0x55aaU, 2001U, 604800U, LAGHU_IMAGE_INLINE | LAGHU_IMAGE_DEDUP_INLINE, true, false, false, false, 0U,
                                    &deny_data_csp, false, 2048U, 2048U, 8192U, 0U, 100U, &page));
  assert(page.rewritten);
  assert(strstr((const char *)page.data, "data:image/") == NULL);
  laghu_runtime_html_result_release(&page);
}

static void test_instrumentation_beacons(const laghu_test_workspace *workspace, laghu_rum_engine *rum_engine, const char *policy_key) {
  static const char valid[] = "# exact observation rules\nhost cdn.example.test /assets/\n";
  static const char invalid_host[] = "host 127.0.0.1 /assets/\n";
  static const char duplicate[] =
      "host cdn.example.test /assets/\n"
      "host cdn.example.test /assets/js/\n";
  char observation_path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_javascript_observation_set providers;
  laghu_runtime_html_result rum_page;
  laghu_instrumentation_beacon rum;
  char script_key[LAGHU_RUNTIME_KEY_SIZE];
  char json[2048U];
  const char *marker;

  assert(laghu_test_workspace_path(workspace, "javascript-observation.conf", observation_path, sizeof(observation_path)));
  assert(laghu_test_workspace_write(workspace, "javascript-observation.conf", (const unsigned char *)valid, sizeof(valid) - 1U));
  assert(laghu_javascript_observations_load(observation_path, &providers, NULL, 0U));
  assert(providers.count == 1U && providers.digest[0] != '\0');
  assert(laghu_runtime_add_instrumentation(rum_engine, workspace->path, &providers,
                                           (laghu_buffer){(const unsigned char *)"<html><body><script src=\"/app.js\"></script>"
                                                                                 "<script src=\"https://cdn.example.test/assets/a.js\">"
                                                                                 "</script></body></html>",
                                                          sizeof("<html><body><script src=\"/app.js\"></script>"
                                                                 "<script src=\"https://cdn.example.test/assets/a.js\">"
                                                                 "</script></body></html>") -
                                                              1U},
                                           "/rum", "https://example.test", policy_key, 3000U, 604800U, 10U, NULL, &rum_page));
  assert(rum_page.rewritten);
  marker = strstr((const char *)rum_page.data, "data-laghu-template=\"");
  assert(marker != NULL);
  marker += sizeof("data-laghu-template=\"") - 1U;
  memset(&rum, 0, sizeof(rum));
  memcpy(rum.template_key, marker, LAGHU_SHA256_HEX_LENGTH);
  rum.template_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
  assert(
      laghu_sha256_hex((laghu_buffer){(const unsigned char *)"https://example.test/app.js", sizeof("https://example.test/app.js") - 1U}, script_key));
  assert(snprintf(json, sizeof(json),
                  "{\"version\":1,\"template\":\"%s\",\"bucket\":1,"
                  "\"lcp_ms\":2200,\"inp_ms\":180,\"cls_milli\":80,"
                  "\"dcl_ms\":900,\"load_ms\":1200,\"errors\":0,"
                  "\"rejections\":0,\"candidates\":[{\"key\":\"%s\","
                  "\"before_dcl\":1,\"long_tasks\":0}]}",
                  rum.template_key, script_key) > 0);
  assert(laghu_runtime_parse_instrumentation_beacon((laghu_buffer){(const unsigned char *)json, strlen(json)}, &rum));
  assert(rum.candidate_count == 1U && rum.lcp_ms == 2200U);
  assert(laghu_instrumentation_apply_beacon(rum_engine, workspace->path, 3001U, 604800U, &rum));
  assert(snprintf(json, sizeof(json),
                  "{\"version\":2,\"template\":\"%s\",\"bucket\":0,"
                  "\"scheme\":1,\"lcp_ms\":1200,\"inp_ms\":80,"
                  "\"cls_milli\":20,\"dcl_ms\":700,\"load_ms\":900,"
                  "\"errors\":0,\"rejections\":0,\"candidates\":[],"
                  "\"lcp_kind\":0,\"lcp_ordinal\":0,"
                  "\"lcp_resource\":\"\"}",
                  rum.template_key) > 0);
  assert(laghu_runtime_parse_instrumentation_beacon((laghu_buffer){(const unsigned char *)json, strlen(json)}, &rum));
  assert(rum.version == 2U && rum.color_scheme_bucket == 1U && rum.lcp_kind == 0U);
  assert(laghu_instrumentation_apply_beacon(rum_engine, workspace->path, 3002U, 604800U, &rum));
  assert(strstr(laghu_runtime_instrumentation_script(), "largest-contentful-paint") != NULL);
  laghu_runtime_html_result_release(&rum_page);
  assert(laghu_test_workspace_write(workspace, "javascript-observation.conf", (const unsigned char *)invalid_host, sizeof(invalid_host) - 1U));
  assert(!laghu_javascript_observations_load(observation_path, &providers, NULL, 0U));
  assert(laghu_test_workspace_write(workspace, "javascript-observation.conf", (const unsigned char *)duplicate, sizeof(duplicate) - 1U));
  assert(!laghu_javascript_observations_load(observation_path, &providers, NULL, 0U));
}

int main(void) {
  static const unsigned char payload[] = "runtime payload";
  laghu_test_workspace workspace;
  laghu_rum_options options;
  laghu_rum_engine *rum;
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];

  assert(laghu_test_workspace_create(&workspace));
  assert(laghu_sha256_hex((laghu_buffer){payload, sizeof(payload) - 1U}, policy_key));
  laghu_rum_options_init(&options);
  rum = laghu_rum_engine_create(&options, NULL, 0U);
  assert(rum != NULL);
  test_lcp_prioritization(rum);
  test_image_markup_and_headers(rum, workspace.path, policy_key);
  test_instrumentation_beacons(&workspace, rum, policy_key);
  laghu_rum_engine_destroy(rum);
  assert(laghu_test_workspace_remove(&workspace));
  puts("laghu_runtime_instrumentation_lcp_integration_test: all tests passed");
  return 0;
}
