// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef _WIN32
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "laghu/runtime.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

static void assert_head_rewrite(const char *input, bool normalize, bool move,
                                bool cross, const char *expected, bool added,
                                bool combined, bool moved) {
  laghu_runtime_head_result result;
  laghu_html_planner_mask plan =
      (normalize ? LAGHU_HTML_PLAN_ADD_COMBINE_HEAD : 0U) |
      (move ? LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD : 0U) |
      (cross ? LAGHU_HTML_PLAN_MOVE_CSS_ABOVE_SCRIPTS : 0U);
  assert(laghu_runtime_plan_html_document(
      (laghu_buffer){(const unsigned char *)input, strlen(input)}, plan,
      &result));
  if (expected == NULL) {
    assert(!result.rewritten && result.data == NULL);
  } else {
    if (!result.rewritten || result.length != strlen(expected) ||
        memcmp(result.data, expected,
               result.length < strlen(expected) ? result.length
                                                : strlen(expected)) != 0) {
      fprintf(stderr, "head input: %s\nexpected: %s\nactual: %.*s\n", input,
              expected, (int)result.length,
              result.data == NULL ? (const unsigned char *)"" : result.data);
    }
    assert(result.rewritten && result.length == strlen(expected));
    assert(memcmp(result.data, expected, result.length) == 0);
  }
  assert(result.added_head == added);
  assert(result.combined_heads == combined);
  assert(result.moved_css == moved);
  laghu_runtime_head_result_release(&result);
}

static void assert_html_plan(const char *input, laghu_html_planner_mask plan,
                             const char *expected) {
  laghu_runtime_head_result result;
  if (!laghu_runtime_plan_html_document(
          (laghu_buffer){(const unsigned char *)input, strlen(input)}, plan,
          &result)) {
    fprintf(stderr, "HTML planner rejected: %s\n", input);
    assert(false);
  }
  if (expected == NULL) {
    assert(!result.rewritten && result.data == NULL);
  } else {
    if (!result.rewritten || result.length != strlen(expected) ||
        memcmp(result.data, expected,
               result.length < strlen(expected) ? result.length
                                                : strlen(expected)) != 0) {
      fprintf(stderr, "HTML input: %s\nexpected: %s\nactual: %.*s\n", input,
              expected, (int)result.length,
              result.data == NULL ? (const unsigned char *)"" : result.data);
    }
    assert(result.rewritten && result.lexical_changed);
    assert(result.length == strlen(expected));
    assert(memcmp(result.data, expected, result.length) == 0);
  }
  laghu_runtime_head_result_release(&result);
}

static void assert_html_plan_at(const char *input, const char *page_path,
                                const char *page_origin,
                                laghu_html_planner_mask plan,
                                const char *expected) {
  laghu_runtime_head_result result;
  assert(laghu_runtime_plan_html_document_at(
      (laghu_buffer){(const unsigned char *)input, strlen(input)}, page_path,
      page_origin, plan, &result));
  if (expected == NULL) {
    if (result.rewritten)
      fprintf(stderr, "Unexpected HTML URL rewrite: %s\nactual: %.*s\n", input,
              (int)result.length, result.data);
    assert(!result.rewritten && result.data == NULL);
  } else {
    if (!result.rewritten || result.length != strlen(expected) ||
        memcmp(result.data, expected,
               result.length < strlen(expected) ? result.length
                                                : strlen(expected)) != 0) {
      fprintf(stderr, "HTML URL input: %s\nexpected: %s\nactual: %.*s\n", input,
              expected, (int)result.length,
              result.data == NULL ? (const unsigned char *)"" : result.data);
    }
    assert(result.rewritten && result.lexical_changed);
    assert(result.length == strlen(expected));
    assert(memcmp(result.data, expected, result.length) == 0);
  }
  laghu_runtime_head_result_release(&result);
}

static void test_html_trim_urls(void) {
  const laghu_html_planner_mask trim = LAGHU_HTML_PLAN_TRIM_URLS;
  assert_html_plan_at(
      "<script src=\"https://example.com/shop/app.js?q=1#x\"></script>"
      "<img src=/shop/images/a%20b.png>"
      "<video poster=/shared/poster.jpg></video>",
      "/shop/page.html", "https://example.com", trim,
      "<script src=\"app.js?q=1#x\"></script>"
      "<img src=images/a%20b.png>"
      "<video poster=/shared/poster.jpg></video>");
  assert_html_plan_at("<audio src=/shop/shared/a.mp3></audio>",
                      "/shop/deep/page.html", "https://example.com", trim,
                      "<audio src=../shared/a.mp3></audio>");
  assert_html_plan_at(
      "<img src=/assets/hero.png>"
      "<base href=/assets/>",
      "/shop/page.html", "https://example.com", trim,
      "<img src=hero.png><base href=/assets/>");
  assert_html_plan_at(
      "<base href=https://cdn.example/assets/><img "
      "src=https://example.com/assets/a.png>",
      "/shop/page.html", "https://example.com", trim, NULL);
  assert_html_plan_at(
      "<link rel=\"alternate stylesheet\" href=/shop/a.css>"
      "<link rel=canonical href=/shop/page.html>"
      "<a href=/shop/next>next</a><form action=/submit></form>"
      "<iframe src=/frame></iframe><object data=/object></object>",
      "/shop/page.html", "https://example.com", trim,
      "<link rel=\"alternate stylesheet\" href=a.css>"
      "<link rel=canonical href=/shop/page.html>"
      "<a href=/shop/next>next</a><form action=/submit></form>"
      "<iframe src=/frame></iframe><object data=/object></object>");
  assert_html_plan_at(
      "<picture><source srcset=\"/shop/a.webp 1x, "
      "https://example.com/shop/a@2.webp 2x\"><img "
      "srcset=\"/shop/a.png 480w, /shared/a.png 960w\"></picture>",
      "/shop/page.html", "https://example.com", trim,
      "<picture><source srcset=\"a.webp 1x, a@2.webp 2x\"><img "
      "srcset=\"a.png 480w, /shared/a.png 960w\"></picture>");
  assert_html_plan_at(
      "<img srcset=\"data:image/png;base64,aaaa 1x, /shop/a.png 2x\">"
      "<script src=\"https://other.example/a.js\"></script>"
      "<img src=\"/shop/a&amp;b.png\">",
      "/shop/page.html", "https://example.com", trim, NULL);
  assert_html_plan_at("<script src=https://example.com:443/a.js></script>",
                      "/page.html", "https://example.com", trim,
                      "<script src=a.js></script>");
  assert_html_plan_at("<script src=https://example.com:8443/a.js></script>",
                      "/page.html", "https://example.com:8443", trim,
                      "<script src=a.js></script>");
  assert_html_plan_at(
      "<audio src=\"http://127.0.0.1:18081/asset.mp3?q=1#hero\"></audio>",
      "/minify-page.html", "http://127.0.0.1:18081", trim,
      "<audio src=\"asset.mp3?q=1#hero\"></audio>");
  assert_html_plan_at("<img src=https://[2001:db8::1]:8443/a.png>",
                      "/page.html", "https://[2001:db8::1]:8443", trim,
                      "<img src=a.png>");
  assert_html_plan_at(
      "<template><img src=/shop/a.png></template>"
      "<img src=/shop//a.png>",
      "/shop/page.html", "https://example.com", trim, NULL);
  assert_html_plan_at("<img src=/shop/a.png>", "/shop/page.html", NULL, trim,
                      NULL);
}

static void test_html_lexical_planner(void) {
  const laghu_html_planner_mask lexical = LAGHU_HTML_PLAN_LEXICAL;
  laghu_runtime_head_result result;

  assert_html_plan("<p>one \n\t two&nbsp;\xC2\xA0 three</p>", lexical,
                   "<p>one two&nbsp;\xC2\xA0 three</p>");
  assert_html_plan(
      "<div> a  b </div><pre> a  b </pre><textarea> a\n b </textarea>"
      "<script> a  b </script><style> a  b </style>"
      "<template><span> a  b </span></template>"
      "<template><template> a  b </template> c  d </template>"
      "<noscript> a  b </noscript><svg><text> a  b </text></svg>"
      "<math><mtext> a  b </mtext></math>"
      "<section contenteditable=\"true\"><span> a  b </span></section>",
      lexical,
      "<div> a b </div><pre> a  b </pre><textarea> a\n b </textarea>"
      "<script> a  b </script><style> a  b </style>"
      "<template><span> a  b </span></template>"
      "<template><template> a  b </template> c  d </template>"
      "<noscript> a  b </noscript><svg><text> a  b </text></svg>"
      "<math><mtext> a  b </mtext></math>"
      "<section contenteditable=true><span> a  b </span></section>");
  assert_html_plan(
      "<!--drop--><!--! keep--><!--keep ! marker--><!--[if IE]>keep<![endif]-->"
      "<!-- @license x --><!-- @preserve x --><!-- laghu:keep -->"
      "<!--# sourceMappingURL=x --><!--# sourceURL=x --><p>x</p>",
      LAGHU_HTML_PLAN_REMOVE_COMMENTS,
      "<!--! keep--><!--keep ! marker--><!--[if IE]>keep<![endif]-->"
      "<!-- @license x -->"
      "<!-- @preserve x --><!-- laghu:keep --><!--# sourceMappingURL=x -->"
      "<!--# sourceURL=x --><p>x</p>");
  assert_html_plan(
      "<input a=\"safe\" b='a&amp;b' c=\"a b\" d=\"\" e=\"a=b\" "
      "f=\"a`b\" g=\"a&lt;b\">",
      LAGHU_HTML_PLAN_REMOVE_QUOTES,
      "<input a=safe b=a&amp;b c=\"a b\" d=\"\" e=\"a=b\" "
      "f=\"a`b\" g=a&lt;b>");
  assert_html_plan(
      "<SCRIPT TYPE=\"TEXT/JAVASCRIPT\" src=\"/a.js\"></SCRIPT>"
      "<script type=\"module\"></script>"
      "<script type=\"text/javascript; charset=utf-8\"></script>"
      "<STYLE type='text/css'> x  y </STYLE>"
      "<link REL=\"stylesheet\" type=\"TEXT/CSS\" href=\"/a.css\">"
      "<link rel=\"alternate stylesheet\" type=\"text/css\">",
      LAGHU_HTML_PLAN_ELIDE_ATTRIBUTES,
      "<SCRIPT src=\"/a.js\"></SCRIPT><script type=\"module\"></script>"
      "<script type=\"text/javascript; charset=utf-8\"></script>"
      "<STYLE> x  y </STYLE><link REL=\"stylesheet\" href=\"/a.css\">"
      "<link rel=\"alternate stylesheet\" type=\"text/css\">");
  assert(!laghu_runtime_plan_html_document(
      (laghu_buffer){(const unsigned char *)"<div id=x ID=y></div>",
                     sizeof("<div id=x ID=y></div>") - 1U},
      lexical, &result));
  assert(!laghu_runtime_plan_html_document(
      (laghu_buffer){(const unsigned char *)"<div title='x></div>",
                     sizeof("<div title='x></div>") - 1U},
      lexical, &result));
}

static void publish_javascript_fixture(const char *cache_path, const char *url,
                                       const char *policy, const char *target,
                                       const char *source, const char *derived,
                                       uint32_t flags, uint64_t updated,
                                       char variant[LAGHU_RUNTIME_KEY_SIZE]) {
  unsigned char catalog[1908U] = {0};
  uint64_t magic = UINT64_C(0x4c414748554a5343);
  uint64_t source_length = strlen(source), derived_length = strlen(derived);
  uint32_t version = 3U, module = flags & 1U;
  char canonical[LAGHU_RUNTIME_PATH_SIZE + LAGHU_RUNTIME_KEY_SIZE +
                 LAGHU_JAVASCRIPT_TARGET_SIZE + 64U];
  char catalog_key[LAGHU_RUNTIME_KEY_SIZE], checksum[LAGHU_RUNTIME_KEY_SIZE];
  char path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_runtime_cache_entry entry;
  FILE *file;
  int length;
  assert(laghu_sha256_hex(
      (laghu_buffer){(const unsigned char *)derived, derived_length}, variant));
  assert(laghu_runtime_cache_publish(
      cache_path, variant, variant, variant, "application/javascript",
      "swc-test",
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
  length =
      snprintf(canonical, sizeof(canonical), "laghu-js-url-v1\n%s\n%s\n%s\n%s",
               url, policy, target, module != 0U ? "module" : "classic");
  assert(length > 0 && (size_t)length < sizeof(canonical));
  assert(laghu_sha256_hex(
      (laghu_buffer){(const unsigned char *)canonical, (size_t)length},
      catalog_key));
  length = snprintf(path, sizeof(path), "%s/javascript-%s.meta", cache_path,
                    catalog_key);
  assert(length > 0 && (size_t)length < sizeof(path));
  file = fopen(path, "wb");
  assert(file != NULL);
  assert(fwrite(catalog, 1U, sizeof(catalog), file) == sizeof(catalog));
  assert(fclose(file) == 0);
}

static void test_head_planner(void) {
  laghu_runtime_head_result result;
  unsigned char *bounded;
  size_t offset = 0U;
  unsigned int index;

  assert_head_rewrite(
      "<!doctype html><html><head><title>x</title></head>"
      "<body>x</body></html>",
      true, true, false, NULL, false, false, false);
  assert_head_rewrite(
      "<HTML><HEAD><meta></HEAD> \n<!--gap--><head><title>x"
      "</title></head><body></body></HTML>",
      true, false, false,
      "<HTML><HEAD><meta> \n<!--gap--><title>x</title></HEAD>"
      "<body></body></HTML>",
      false, true, false);
  assert_head_rewrite("<html><head></head><body>x</body><head></head></html>",
                      true, true, true, NULL, false, false, false);
  assert_head_rewrite("<!doctype html><html><body>x</body></html>", true, false,
                      false,
                      "<!doctype html><html><head></head><body>x</body></html>",
                      true, false, false);
  assert_head_rewrite("<!doctype html><body>x</body>", true, false, false,
                      "<!doctype html><head></head><body>x</body>", true, false,
                      false);
  assert_head_rewrite("<div>fragment</div>", true, true, true, NULL, false,
                      false, false);
  assert_head_rewrite(
      "<html><head><meta></head><body><link media='print' rel='stylesheet' "
      "href='/a.css'><style nonce=abc>.a{color:red}</style></body></html>",
      true, true, false,
      "<html><head><meta><link media='print' rel='stylesheet' "
      "href='/a.css'><style nonce=abc>.a{color:red}</style></head><body>"
      "</body></html>",
      false, false, true);
  assert_head_rewrite(
      "<html><head><script src='/app.js'></script></head><body>"
      "<style>.a{color:red}</style></body></html>",
      true, true, false, NULL, false, false, false);
  assert_head_rewrite(
      "<html><head><script src='/app.js'></script></head><body>"
      "<style>.a{color:red}</style></body></html>",
      true, true, true,
      "<html><head><style>.a{color:red}</style><script src='/app.js'>"
      "</script></head><body></body></html>",
      false, false, true);
  assert_head_rewrite(
      "<html><head><script type='application/ld+json'>{}</script></head>"
      "<body><link rel=stylesheet href=/a.css></body></html>",
      true, true, false,
      "<html><head><script type='application/ld+json'>{}</script>"
      "<link rel=stylesheet href=/a.css></head><body></body></html>",
      false, false, true);
  assert_head_rewrite(
      "<html><head><script type='application/json'>{\"tag\":\"<head>\"}"
      "</script></head><body><style>.x::before{content:'<head>'}</style>"
      "</body></html>",
      true, true, false,
      "<html><head><script type='application/json'>{\"tag\":\"<head>\"}"
      "</script><style>.x::before{content:'<head>'}</style></head><body>"
      "</body></html>",
      false, false, true);
  assert_head_rewrite(
      "<html><head></head><body><template><style>.x{}</style></template>"
      "<noscript><link rel=stylesheet href=/n.css></noscript>"
      "<link rel='alternate stylesheet' href=/a.css>"
      "<style scoped>.s{}</style><style onclick=x>.e{}</style></body></html>",
      true, true, true, NULL, false, false, false);
  assert(!laghu_runtime_plan_html_document(
      (laghu_buffer){(const unsigned char *)"<html><head>", 12U},
      LAGHU_HTML_PLAN_ADD_COMBINE_HEAD | LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD,
      &result));
  assert(!laghu_runtime_plan_html_document(
      (laghu_buffer){
          (const unsigned char *)"<html><head><head></head></head></html>",
          sizeof("<html><head><head></head></head></html>") - 1U},
      LAGHU_HTML_PLAN_ADD_COMBINE_HEAD | LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD,
      &result));

  bounded = malloc((LAGHU_HTML_MAX_TOKENS + 1U) * 6U);
  assert(bounded != NULL);
  for (index = 0U; index <= LAGHU_HTML_MAX_TOKENS; ++index) {
    memcpy(bounded + offset, "<meta>", 6U);
    offset += 6U;
  }
  assert(!laghu_runtime_plan_html_document(
      (laghu_buffer){bounded, offset},
      LAGHU_HTML_PLAN_ADD_COMBINE_HEAD | LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD,
      &result));
  free(bounded);
}

static void test_head_planner_cache(const char *cache_path,
                                    const char *policy_key) {
  static const unsigned char html[] =
      "<html><head><script src=/app.js></script></head><body>"
      "<style media=all>.a{color:red}</style></body></html>";
  static const unsigned char changed[] =
      "<html><head><script src=/app.js></script></head><body>"
      "<style media=all>.a{color:blue}</style></body></html>";
  laghu_runtime_html_result result;
  char first_key[LAGHU_RUNTIME_KEY_SIZE];

  assert(laghu_runtime_rewrite_css_markup(
      cache_path, (laghu_buffer){html, sizeof(html) - 1U}, "/head.html",
      "https://example.test", policy_key, 0U, 42U, 604800U, false, false, false,
      LAGHU_HTML_PLAN_ADD_COMBINE_HEAD | LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD |
          LAGHU_HTML_PLAN_MOVE_CSS_ABOVE_SCRIPTS,
      true, true, 2048U, 8192U, &result));
  assert(!result.rewritten && result.dependencies_pending);
  strcpy(first_key, result.dependency_key);
  laghu_runtime_html_result_release(&result);
  assert(laghu_runtime_rewrite_css_markup(
      cache_path, (laghu_buffer){html, sizeof(html) - 1U}, "/head.html",
      "https://example.test", policy_key, 0U, 43U, 604800U, false, false, false,
      LAGHU_HTML_PLAN_ADD_COMBINE_HEAD | LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD |
          LAGHU_HTML_PLAN_MOVE_CSS_ABOVE_SCRIPTS,
      true, true, 2048U, 8192U, &result));
  assert(result.rewritten && !result.dependencies_pending);
  assert(result.length == sizeof(html) - 1U);
  assert(strstr((const char *)result.data, "<head><style media=all>") != NULL);
  assert(strstr((const char *)result.data, "</head><body></body>") != NULL);
  assert(strcmp(first_key, result.dependency_key) == 0);
  laghu_runtime_html_result_release(&result);
  assert(laghu_runtime_rewrite_css_markup(
      cache_path, (laghu_buffer){changed, sizeof(changed) - 1U}, "/head.html",
      "https://example.test", policy_key, 0U, 44U, 604800U, false, false, false,
      LAGHU_HTML_PLAN_ADD_COMBINE_HEAD | LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD |
          LAGHU_HTML_PLAN_MOVE_CSS_ABOVE_SCRIPTS,
      true, true, 2048U, 8192U, &result));
  assert(!result.rewritten && result.dependencies_pending);
  assert(strcmp(first_key, result.dependency_key) != 0);
  laghu_runtime_html_result_release(&result);
}

static void test_rum_engine(const char *directory) {
  static const char key[] =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  static const unsigned char payload[] = {0U, 1U, 2U, 0xfeU, 0xffU};
  laghu_rum_options options;
  laghu_rum_engine *engine;
  laghu_rum_value value;
  unsigned char restored[sizeof(payload)];
  laghu_rum_image_record image = {0}, restored_image = {0};
  char snapshot[LAGHU_RUNTIME_PATH_SIZE];
  assert(snprintf(snapshot, sizeof(snapshot), "%s/rum.snapshot", directory) >
         0);
  laghu_rum_options_init(&options);
  options.snapshot_path = snapshot;
  options.sync_interval_seconds = 1U;
  engine = laghu_rum_engine_create(&options, NULL, 0U);
  assert(engine != NULL);
  assert(laghu_rum_engine_publish(engine, LAGHU_RUM_RECORD_DECISION, key, 10U,
                                  payload, sizeof(payload), NULL));
  assert(laghu_rum_engine_read(engine, LAGHU_RUM_RECORD_DECISION, key, 11U,
                               restored, sizeof(restored), &value));
  assert(value.length == sizeof(payload) &&
         memcmp(restored, payload, sizeof(payload)) == 0);
  assert(laghu_rum_engine_memory_used(engine) == sizeof(payload));
  image.version = 1U;
  strcpy(image.identity, key);
  image.updated_at = 10U;
  image.width = 321U;
  image.above_fold = true;
  assert(laghu_rum_engine_publish(engine, LAGHU_RUM_RECORD_IMAGE, key, 10U,
                                  &image, sizeof(image), NULL));
  laghu_rum_engine_destroy(engine);
  engine = laghu_rum_engine_create(&options, NULL, 0U);
  assert(engine != NULL);
  memset(restored, 0, sizeof(restored));
  assert(laghu_rum_engine_read(engine, LAGHU_RUM_RECORD_DECISION, key, 12U,
                               restored, sizeof(restored), &value));
  assert(memcmp(restored, payload, sizeof(payload)) == 0);
  assert(laghu_rum_engine_read(engine, LAGHU_RUM_RECORD_IMAGE, key, 12U,
                               &restored_image, sizeof(restored_image),
                               &value));
  assert(restored_image.width == 321U && restored_image.above_fold &&
         !strcmp(restored_image.identity, key));
  laghu_rum_engine_destroy(engine);
  {
    laghu_rum_engine *peer_one, *peer_two;
    laghu_rum_image_record one = {0}, two = {0};
    laghu_rum_instrumentation_record rum_one = {0}, rum_two = {0}, rum_total;
    one.version = two.version = 1U;
    strcpy(one.identity, key);
    strcpy(two.identity, key);
    one.updated_at = 20U;
    two.updated_at = 21U;
    one.width = 400U;
    two.height = 300U;
    two.above_fold = true;
    peer_one = laghu_rum_engine_create(&options, NULL, 0U);
    peer_two = laghu_rum_engine_create(&options, NULL, 0U);
    assert(peer_one != NULL && peer_two != NULL);
    assert(laghu_rum_engine_publish(peer_one, LAGHU_RUM_RECORD_IMAGE, key, 20U,
                                    &one, sizeof(one), NULL));
    assert(laghu_rum_engine_publish(peer_two, LAGHU_RUM_RECORD_IMAGE, key, 21U,
                                    &two, sizeof(two), NULL));
    rum_one.version = rum_two.version = LAGHU_INSTRUMENTATION_VERSION;
    strcpy(rum_one.template_key, key);
    strcpy(rum_two.template_key, key);
    strcpy(rum_one.provider_digest, key);
    strcpy(rum_two.provider_digest, key);
    strcpy(rum_one.policy_key, key);
    strcpy(rum_two.policy_key, key);
    rum_one.updated_at = 20U;
    rum_two.updated_at = 21U;
    rum_one.observations[0] = 2U;
    rum_two.observations[0] = 3U;
    assert(laghu_rum_engine_publish(peer_one, LAGHU_RUM_RECORD_INSTRUMENTATION,
                                    key, 20U, &rum_one, sizeof(rum_one), NULL));
    assert(laghu_rum_engine_publish(peer_two, LAGHU_RUM_RECORD_INSTRUMENTATION,
                                    key, 21U, &rum_two, sizeof(rum_two), NULL));
    laghu_rum_engine_destroy(peer_one);
    laghu_rum_engine_destroy(peer_two);
    engine = laghu_rum_engine_create(&options, NULL, 0U);
    assert(engine != NULL &&
           laghu_rum_engine_read(engine, LAGHU_RUM_RECORD_IMAGE, key, 22U,
                                 &restored_image, sizeof(restored_image),
                                 &value));
    assert(restored_image.width == 400U && restored_image.height == 300U &&
           restored_image.above_fold);
    assert(laghu_rum_engine_read(engine, LAGHU_RUM_RECORD_INSTRUMENTATION, key,
                                 22U, &rum_total, sizeof(rum_total), &value));
    assert(rum_total.observations[0] == 5U);
    laghu_rum_engine_destroy(engine);
    engine = laghu_rum_engine_create(&options, NULL, 0U);
    assert(engine != NULL &&
           laghu_rum_engine_read(engine, LAGHU_RUM_RECORD_INSTRUMENTATION, key,
                                 23U, &rum_total, sizeof(rum_total), &value));
    assert(rum_total.observations[0] == 5U);
    laghu_rum_engine_destroy(engine);
  }
  laghu_rum_options_init(&options);
  options.store_uri = "memory:";
  options.snapshot_path = NULL;
  options.ttl_seconds = 1U;
  engine = laghu_rum_engine_create(&options, NULL, 0U);
  assert(engine != NULL);
  assert(laghu_rum_engine_publish(engine, LAGHU_RUM_RECORD_DECISION, key, 20U,
                                  payload, sizeof(payload), NULL));
  assert(!laghu_rum_engine_read(engine, LAGHU_RUM_RECORD_DECISION, key, 22U,
                                restored, sizeof(restored), &value));
  laghu_rum_engine_destroy(engine);
  options.store_uri = "memcached://127.0.0.1:11211";
  assert(laghu_rum_engine_create(&options, NULL, 0U) == NULL);
  {
    char rum_error[160U];
    options.store_uri = "redis://secret@example.test:6379/0?prefix=laghu:";
    assert(laghu_rum_engine_create(&options, rum_error, sizeof(rum_error)) ==
           NULL);
    assert(strstr(rum_error, "secret") == NULL);
    options.store_uri = "rediss://example.test:6380/0?unknown=value";
    assert(laghu_rum_engine_create(&options, rum_error, sizeof(rum_error)) ==
           NULL);
  }
  {
    laghu_rum_image_record aggregate = {0}, delta = {0};
    aggregate.version = delta.version = 1U;
    strcpy(aggregate.identity, key);
    strcpy(delta.identity, key);
    aggregate.width = 100U;
    delta.width = 200U;
    delta.above_fold = true;
    assert(laghu_rum_record_merge(LAGHU_RUM_RECORD_IMAGE, &aggregate, &delta,
                                  sizeof(aggregate)));
    assert(aggregate.width == 200U && aggregate.above_fold);
  }
}

static void test_rum_redis(void) {
  static const char key[] =
      "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  const char *uri = getenv("LAGHU_TEST_REDIS_URI");
  const char *library = getenv("LAGHU_TEST_HIREDIS_LIBRARY");
  laghu_rum_options options;
  laghu_rum_engine *one, *two, *reader;
  laghu_rum_image_record first = {0}, second = {0}, aggregate = {0};
  laghu_rum_instrumentation_record rum_first = {0}, rum_second = {0},
                                   rum_aggregate = {0};
  laghu_critical_css_record css_first = {0}, css_second = {0},
                            css_aggregate = {0};
  static const unsigned char decision[] = "ready-v1";
  unsigned char decision_result[sizeof(decision)] = {0};
  laghu_rum_value value;
  if (uri == NULL || *uri == '\0') return;
  laghu_rum_options_init(&options);
  options.store_uri = uri;
  options.client_library = library;
  options.required = true;
  options.sync_interval_seconds = 1U;
  one = laghu_rum_engine_create(&options, NULL, 0U);
  two = laghu_rum_engine_create(&options, NULL, 0U);
  assert(one != NULL && two != NULL);
  first.version = second.version = 1U;
  strcpy(first.identity, key);
  strcpy(second.identity, key);
  first.updated_at = 100U;
  first.width = 640U;
  second.updated_at = 101U;
  second.height = 480U;
  second.above_fold = true;
  assert(laghu_rum_engine_publish(one, LAGHU_RUM_RECORD_IMAGE, key, 100U,
                                  &first, sizeof(first), NULL));
  assert(laghu_rum_engine_publish(two, LAGHU_RUM_RECORD_IMAGE, key, 101U,
                                  &second, sizeof(second), NULL));
  rum_first.version = rum_second.version = LAGHU_INSTRUMENTATION_VERSION;
  strcpy(rum_first.template_key, key);
  strcpy(rum_second.template_key, key);
  strcpy(rum_first.provider_digest, key);
  strcpy(rum_second.provider_digest, key);
  strcpy(rum_first.policy_key, key);
  strcpy(rum_second.policy_key, key);
  rum_first.updated_at = 100U;
  rum_second.updated_at = 101U;
  rum_first.observations[1] = UINT64_MAX - 1U;
  rum_second.observations[1] = 5U;
  assert(laghu_rum_engine_publish(one, LAGHU_RUM_RECORD_INSTRUMENTATION, key,
                                  100U, &rum_first, sizeof(rum_first), NULL));
  assert(laghu_rum_engine_publish(two, LAGHU_RUM_RECORD_INSTRUMENTATION, key,
                                  101U, &rum_second, sizeof(rum_second), NULL));
  css_first.version = css_second.version = LAGHU_CRITICAL_CSS_VERSION;
  strcpy(css_first.template_key, key);
  strcpy(css_second.template_key, key);
  strcpy(css_first.stylesheet_url, "/assets/main.css");
  strcpy(css_second.stylesheet_url, "/assets/main.css");
  strcpy(css_first.stylesheet_key, key);
  strcpy(css_second.stylesheet_key, key);
  strcpy(css_first.policy_key, key);
  strcpy(css_second.policy_key, key);
  css_first.updated_at = 100U;
  css_second.updated_at = 101U;
  css_first.generation = UINT32_MAX - 1U;
  css_second.generation = 3U;
  css_first.observation_count[0] = UINT16_MAX - 1U;
  css_second.observation_count[0] = 2U;
  css_first.critical_rules[0][0] = 0x01U;
  css_second.critical_rules[0][0] = 0x80U;
  assert(laghu_rum_engine_publish(one, LAGHU_RUM_RECORD_CRITICAL_CSS, key, 100U,
                                  &css_first, sizeof(css_first), NULL));
  assert(laghu_rum_engine_publish(two, LAGHU_RUM_RECORD_CRITICAL_CSS, key, 101U,
                                  &css_second, sizeof(css_second), NULL));
  assert(laghu_rum_engine_publish(one, LAGHU_RUM_RECORD_DECISION, key, 100U,
                                  decision, sizeof(decision), NULL));
  laghu_rum_engine_destroy(one);
  laghu_rum_engine_destroy(two);
  reader = laghu_rum_engine_create(&options, NULL, 0U);
  assert(reader != NULL &&
         laghu_rum_engine_read(reader, LAGHU_RUM_RECORD_IMAGE, key, 102U,
                               &aggregate, sizeof(aggregate), &value));
  assert(aggregate.width == 640U && aggregate.height == 480U &&
         aggregate.above_fold);
  assert(laghu_rum_engine_read(reader, LAGHU_RUM_RECORD_INSTRUMENTATION, key,
                               102U, &rum_aggregate, sizeof(rum_aggregate),
                               &value));
  assert(rum_aggregate.observations[1] == UINT64_MAX);
  assert(laghu_rum_engine_read(reader, LAGHU_RUM_RECORD_CRITICAL_CSS, key, 102U,
                               &css_aggregate, sizeof(css_aggregate), &value));
  assert(css_aggregate.generation == UINT32_MAX &&
         css_aggregate.observation_count[0] == UINT16_MAX &&
         css_aggregate.critical_rules[0][0] == 0x81U);
  assert(laghu_rum_engine_read(reader, LAGHU_RUM_RECORD_DECISION, key, 102U,
                               decision_result, sizeof(decision_result),
                               &value));
  assert(memcmp(decision_result, decision, sizeof(decision)) == 0);
  laghu_rum_engine_destroy(reader);
}

static void test_html_lexical_cache(const char *cache_path,
                                    const char *policy_key) {
  static const unsigned char html[] =
      "<html><body><!--drop--><p class=\"safe\">one   two</p>"
      "<script type=\"text/javascript\"> x  y </script>"
      "<pre> a  b </pre><!-- @license keep --></body></html>";
  laghu_runtime_html_result result;
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];

  assert(laghu_runtime_rewrite_css_markup(
      cache_path, (laghu_buffer){html, sizeof(html) - 1U}, "/minify.html",
      "https://example.test", policy_key, 0U, 51U, 604800U, false, false, false,
      LAGHU_HTML_PLAN_LEXICAL, true, true, 2048U, 8192U, &result));
  assert(!result.rewritten && result.dependencies_pending);
  strcpy(dependency_key, result.dependency_key);
  laghu_runtime_html_result_release(&result);
  assert(laghu_runtime_rewrite_css_markup(
      cache_path, (laghu_buffer){html, sizeof(html) - 1U}, "/minify.html",
      "https://example.test", policy_key, 0U, 52U, 604800U, false, false, false,
      LAGHU_HTML_PLAN_LEXICAL, true, true, 2048U, 8192U, &result));
  assert(result.rewritten && !result.dependencies_pending);
  assert(result.length < sizeof(html) - 1U);
  assert(strcmp(dependency_key, result.dependency_key) == 0);
  assert(strstr((const char *)result.data, "<!--drop-->") == NULL);
  assert(strstr((const char *)result.data,
                "<p class=safe>one two</p><script> x  y </script>") != NULL);
  assert(strstr((const char *)result.data,
                "<pre> a  b </pre><!-- @license keep -->") != NULL);
  laghu_runtime_html_result_release(&result);
}

int main(void) {
  static const unsigned char payload[] = "runtime payload";
  char temporary[LAGHU_RUNTIME_PATH_SIZE];
  char queue_path[LAGHU_RUNTIME_PATH_SIZE];
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char no_webp_index_key[LAGHU_RUNTIME_KEY_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned char received[64];
  unsigned char cached[64];
  laghu_runtime_queue producer = {0};
  laghu_runtime_queue consumer = {0};
  laghu_runtime_job submitted;
  laghu_runtime_job taken;
  laghu_runtime_cache_entry entry;
  laghu_catalog_record catalog = {0};
  laghu_catalog_record loaded;
  char catalog_key[LAGHU_RUNTIME_KEY_SIZE];
  char providers_path[LAGHU_RUNTIME_PATH_SIZE];
  char javascript_target[LAGHU_JAVASCRIPT_TARGET_SIZE];
  laghu_rum_engine *rum_engine;

  laghu_runtime_queue_init(&producer);
  laghu_runtime_queue_init(&consumer);
  test_head_planner();
  test_html_lexical_planner();
  test_html_trim_urls();
  {
    laghu_asset_policy asset;
    char rewritten[LAGHU_RUNTIME_PATH_SIZE];
    char object_key[LAGHU_RUNTIME_PATH_SIZE];
    char error[128];
    laghu_asset_policy_init(&asset);
    strcpy(asset.source_domain, "https://origin.example.test");
    strcpy(asset.public_domain, "https://cdn.example.test");
    strcpy(asset.source_prefix, "/assets");
    strcpy(asset.public_prefix, "/immutable");
    strcpy(asset.mime_types, "image/png, application/pdf");
    assert(laghu_asset_policy_validate(&asset, error, sizeof(error)));
    assert(laghu_asset_object_key(
        &asset, "https://origin.example.test/assets/logo.png?x=1#preview",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        object_key));
    assert(strcmp(object_key,
                  "/immutable/"
                  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789ab"
                  "cdef/logo.png") == 0);
    assert(laghu_asset_url_rewrite(
        &asset, "https://origin.example.test/assets/logo.png?x=1#preview",
        "image/png",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        rewritten, sizeof(rewritten)));
    assert(strcmp(rewritten,
                  "https://cdn.example.test/immutable/"
                  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789ab"
                  "cdef/logo.png?x=1#preview") == 0);
    {
      laghu_asset_record record = {0};
      strcpy(record.source_url,
             "https://origin.example.test/assets/logo.png?x=1#preview");
      strcpy(record.content_type, "image/png");
      strcpy(
          record.content_hash,
          "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
      strcpy(record.object_key,
             "/immutable/"
             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef/"
             "logo.png");
      record.body_length = 1U;
      assert(!laghu_asset_record_rewrite(&asset, &record, rewritten,
                                         sizeof(rewritten)));
      record.state = LAGHU_ASSET_READY;
      assert(laghu_asset_record_rewrite(&asset, &record, rewritten,
                                        sizeof(rewritten)));
      record.object_key[0] = '\0';
      assert(!laghu_asset_record_rewrite(&asset, &record, rewritten,
                                         sizeof(rewritten)));
    }
    asset.preserve_query = false;
    assert(laghu_asset_url_rewrite(
        &asset, "https://origin.example.test/assets/logo.png?x=1#preview",
        "image/png",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        rewritten, sizeof(rewritten)));
    assert(strstr(rewritten, "?x=1") == NULL &&
           strstr(rewritten, "#preview") != NULL);
    strcpy(asset.public_domain, "https://user@cdn.example.test");
    assert(!laghu_asset_policy_validate(&asset, error, sizeof(error)));
  }
#ifdef _WIN32
  {
    char base[LAGHU_RUNTIME_PATH_SIZE];
    char unique[LAGHU_RUNTIME_PATH_SIZE];
    assert(GetTempPathA(sizeof(base), base) > 0U);
    assert(GetTempFileNameA(base, "lgr", 0U, unique) != 0U);
    assert(DeleteFileA(unique));
    assert(snprintf(temporary, sizeof(temporary), "%s", unique) > 0);
    assert(_mkdir(temporary) == 0);
  }
#else
  strcpy(temporary, "/tmp/laghu-runtime-XXXXXX");
  assert(mkdtemp(temporary) != NULL);
#endif
  {
    laghu_cache_backend *backend = calloc(1U, sizeof(*backend));
    laghu_cache_limits limits;
    char uri[LAGHU_RUNTIME_PATH_SIZE];
    char parsed[LAGHU_RUNTIME_PATH_SIZE];
#ifdef _WIN32
    char canonical[LAGHU_RUNTIME_PATH_SIZE];
    size_t cursor;
    assert(snprintf(canonical, sizeof(canonical), "%s", temporary) > 0);
    for (cursor = 0U; canonical[cursor] != '\0'; ++cursor)
      if (canonical[cursor] == '\\') canonical[cursor] = '/';
    assert(snprintf(uri, sizeof(uri), "file:///%s", canonical) > 0);
#else
    assert(snprintf(uri, sizeof(uri), "file://%s", temporary) > 0);
#endif
    assert(backend != NULL);
    assert(laghu_cache_backend_uri_parse(uri, parsed, sizeof(parsed)));
    assert(strcmp(parsed, temporary) == 0);
    assert(!laghu_cache_backend_uri_parse("memcached://127.0.0.1", parsed,
                                          sizeof(parsed)));
    assert(!laghu_cache_backend_uri_parse("file://server/cache", parsed,
                                          sizeof(parsed)));
    assert(!laghu_cache_backend_uri_parse("file:///tmp/cache?secret=x", parsed,
                                          sizeof(parsed)));
    assert(!laghu_cache_backend_uri_parse("file:///tmp/%2fcache", parsed,
                                          sizeof(parsed)));
    laghu_cache_limits_init(&limits);
    assert(laghu_cache_backend_open(backend, uri, &limits));
    assert(strcmp(backend->path, temporary) == 0);
    laghu_cache_backend_close(backend);
    assert(laghu_cache_backend_open_path(backend, temporary, &limits));
    laghu_cache_backend_close(backend);
    free(backend);
  }
  {
    laghu_asset_config asset_config;
    laghu_asset_record asset_record = {0}, loaded_record = {0}, taken_record;
    char config_path[LAGHU_RUNTIME_PATH_SIZE];
    char catalog_path[LAGHU_RUNTIME_PATH_SIZE];
    char asset_queue_path[LAGHU_RUNTIME_PATH_SIZE];
    char catalog_key[LAGHU_RUNTIME_KEY_SIZE];
    char job_path[LAGHU_RUNTIME_PATH_SIZE];
    char error[256];
    unsigned char *taken_body = NULL;
    size_t taken_length = 0U;
    static const unsigned char body[] = "asset-body";
    FILE *config_file;
    assert(snprintf(config_path, sizeof(config_path), "%s/assets.conf",
                    temporary) > 0);
    assert(snprintf(catalog_path, sizeof(catalog_path), "%s/asset-catalog",
                    temporary) > 0);
    assert(snprintf(asset_queue_path, sizeof(asset_queue_path),
                    "%s/asset-queue", temporary) > 0);
    config_file = fopen(config_path, "wb");
    assert(config_file != NULL);
    assert(fprintf(config_file,
                   "version=1\nsource_domain=https://origin.example.test\n"
                   "public_domain=https://cdn.example.test\n"
                   "source_prefix=/assets\npublic_prefix=/immutable\n"
                   "mime_types=image/png,application/pdf\n"
                   "mode=rewrite_only\n"
                   "catalog_path=%s\nqueue_path=%s\n"
                   "endpoint=https://s3.example.test\nregion=test-1\n"
                   "bucket=assets\naccess_key_env=TEST_ACCESS_KEY\n"
                   "secret_key_env=TEST_SECRET_KEY\n",
                   catalog_path, asset_queue_path) > 0);
    assert(fclose(config_file) == 0);
    assert(laghu_asset_config_load(config_path, &asset_config, error,
                                   sizeof(error)));
    strcpy(asset_record.source_url,
           "https://origin.example.test/assets/logo.png?version=1");
    strcpy(asset_record.source_validator, "etag-1");
    strcpy(asset_record.content_hash,
           "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    strcpy(asset_record.policy_digest, asset_config.digest);
    strcpy(asset_record.provider_digest, asset_config.digest);
    strcpy(asset_record.content_type, "image/png");
    assert(laghu_asset_object_key(&asset_config.policy, asset_record.source_url,
                                  asset_record.content_hash,
                                  asset_record.object_key));
    asset_record.body_length = sizeof(body) - 1U;
    asset_record.state = LAGHU_ASSET_PENDING;
    assert(laghu_asset_catalog_key(&asset_record, catalog_key));
    assert(laghu_asset_catalog_publish(catalog_path, &asset_record));
    assert(
        laghu_asset_catalog_lookup(catalog_path, catalog_key, &loaded_record));
    assert(strcmp(loaded_record.source_url, asset_record.source_url) == 0);
    asset_record.state = LAGHU_ASSET_READY;
    asset_record.updated_at = (uint64_t)time(NULL);
    assert(laghu_asset_catalog_publish(catalog_path, &asset_record));
    {
      static const unsigned char document[] =
          "<img src=\"https://origin.example.test/assets/logo.png?version=1\">";
      unsigned char *rewritten_document = NULL;
      size_t rewritten_length = 0U;
      assert(laghu_asset_rewrite_document(
          &asset_config, (laghu_buffer){document, sizeof(document) - 1U},
          &rewritten_document, &rewritten_length));
      assert(rewritten_document != NULL &&
             strstr((const char *)rewritten_document,
                    "https://cdn.example.test/immutable/") != NULL);
      free(rewritten_document);
      rewritten_document = NULL;
      {
        static const unsigned char root_document[] =
            "body{background:url('/assets/logo.png?version=1')}";
        assert(laghu_asset_rewrite_document_at(
            &asset_config,
            (laghu_buffer){root_document, sizeof(root_document) - 1U},
            "/styles/site.css", &rewritten_document, &rewritten_length));
        assert(rewritten_document != NULL &&
               strstr((const char *)rewritten_document,
                      "https://cdn.example.test/immutable/") != NULL);
        free(rewritten_document);
      }
    }
    asset_record.state = LAGHU_ASSET_PENDING;
    assert(laghu_asset_job_publish(&asset_config, &asset_record,
                                   (laghu_buffer){body, sizeof(body) - 1U}));
    assert(laghu_asset_job_take(&asset_config, &taken_record, &taken_body,
                                &taken_length, job_path));
    assert(taken_length == sizeof(body) - 1U &&
           memcmp(taken_body, body, taken_length) == 0);
    free(taken_body);
    assert(laghu_asset_job_complete(job_path));
    memset(&taken_record, 0, sizeof(taken_record));
    memset(&asset_record, 0, sizeof(asset_record));
    strcpy(asset_record.source_url,
           "https://origin.example.test/assets/fallback.png");
    strcpy(asset_record.policy_digest, asset_config.digest);
    strcpy(asset_record.provider_digest, asset_config.digest);
    assert(laghu_asset_job_publish(&asset_config, &asset_record,
                                   (laghu_buffer){NULL, 0U}));
    assert(laghu_asset_job_take(&asset_config, &taken_record, &taken_body,
                                &taken_length, job_path));
    assert(taken_body == NULL && taken_length == 0U &&
           strcmp(taken_record.source_url, asset_record.source_url) == 0);
    assert(laghu_asset_job_complete(job_path));
    assert(laghu_asset_retry_after(&asset_config.policy, 1U, 100U) == 102U);
  }
  test_rum_engine(temporary);
  test_rum_redis();
  {
    static const char config[] =
        "# administrator approvals\n"
        "defer /assets/analytics.js\n"
        "defer /assets/checkout.js template=/checkout/\n";
    laghu_javascript_defer_set defer;
    laghu_rum_instrumentation_record baseline = {0}, current = {0};
    char path[LAGHU_RUNTIME_PATH_SIZE], error[128];
    FILE *file;
    assert(snprintf(path, sizeof(path), "%s/javascript-defer.conf", temporary) >
           0);
    file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(config, 1U, sizeof(config) - 1U, file) ==
           sizeof(config) - 1U);
    assert(fclose(file) == 0);
    assert(laghu_javascript_defer_load(path, &defer, error, sizeof(error)));
    assert(defer.count == 2U && strlen(defer.digest) == 64U);
    assert(laghu_javascript_defer_approved(&defer, "/assets/analytics.js",
                                           "/anywhere/"));
    assert(laghu_javascript_defer_approved(&defer, "/assets/checkout.js",
                                           "/checkout/"));
    assert(!laghu_javascript_defer_approved(&defer, "/assets/checkout.js",
                                            "/account/"));
    baseline.version = current.version = LAGHU_INSTRUMENTATION_VERSION;
    baseline.script_count = current.script_count = 1U;
    baseline.observations[1] = 100U;
    baseline.script_observations[1][0] = 90U;
    assert(laghu_javascript_defer_recommended(&baseline, 1U, 0U));
    current.observations[1] = 50U;
    baseline.errors[1] = 1U;
    current.errors[1] = 2U;
    assert(
        laghu_javascript_defer_rollback_recommended(&baseline, &current, 1U));
  }
  {
    laghu_rum_options rum_options;
    laghu_rum_options_init(&rum_options);
    rum_engine = laghu_rum_engine_create(&rum_options, NULL, 0U);
    assert(rum_engine != NULL);
  }
  {
    static const char config[] =
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
    static const unsigned char hostile_css[] =
        "@font-face{src:url(https://example.test/a.woff2)}";
    laghu_font_provider_set providers;
    const laghu_font_provider *google;
    const laghu_font_provider *fontsource;
    char error[128];
    FILE *file;
    assert(snprintf(providers_path, sizeof(providers_path),
                    "%s/font-providers.conf", temporary) > 0);
    file = fopen(providers_path, "wb");
    assert(file != NULL);
    assert(fwrite(config, 1U, sizeof(config) - 1U, file) ==
           sizeof(config) - 1U);
    assert(fclose(file) == 0);
    assert(laghu_font_providers_load(providers_path, &providers, error,
                                     sizeof(error)));
    assert(providers.count == 2U);
    google = laghu_font_provider_match(
        &providers,
        "https://fonts.googleapis.com/css2?family=Roboto&display=swap");
    fontsource = laghu_font_provider_match(
        &providers,
        "https://cdn.jsdelivr.net/fontsource/css/open-sans@5.2.7/index.css");
    assert(google != NULL && strcmp(google->id, "google_fonts") == 0);
    assert(laghu_font_provider_url_allowed(
        &providers.providers[1],
        "https://cdn.jsdelivr.net/fontsource/css/open-sans@5.2.7/index.css",
        false, false));
    assert(fontsource != NULL && strcmp(fontsource->id, "fontsource_cdn") == 0);
    assert(laghu_font_provider_match(
               &providers, "https://fonts.googleapis.com/evil") == NULL);
    assert(laghu_font_css_validate(
        google, (laghu_buffer){google_css, sizeof(google_css) - 1U}));
    assert(!laghu_font_css_validate(
        google, (laghu_buffer){hostile_css, sizeof(hostile_css) - 1U}));
    {
      static const unsigned char page[] =
          "<html><head><link rel=stylesheet href=\"https://fonts.googleapis."
          "com/css2?family=Roboto\"></head><body></body></html>";
      char font_queue_path[LAGHU_RUNTIME_PATH_SIZE];
      laghu_runtime_queue font_producer;
      laghu_runtime_queue font_consumer;
      laghu_runtime_job font_job;
      laghu_runtime_html_result font_page;
      laghu_runtime_cache_entry css_entry;
      laghu_font_stylesheet_record font_record = {0};
      unsigned char ignored[1];
      char css_key[LAGHU_RUNTIME_KEY_SIZE];
      laghu_runtime_queue_init(&font_producer);
      laghu_runtime_queue_init(&font_consumer);
      assert(snprintf(font_queue_path, sizeof(font_queue_path),
                      "%s/fonts.queue", temporary) > 0);
      assert(
          laghu_runtime_queue_create(&font_producer, font_queue_path, 2U, 1U));
      assert(laghu_runtime_queue_open(&font_consumer, font_queue_path));
      assert(laghu_runtime_rewrite_font_css(
          &font_producer, temporary, &providers,
          (laghu_buffer){page, sizeof(page) - 1U}, 100U, true, true, 2048U,
          &font_page));
      assert(!font_page.rewritten && font_page.dependencies_pending);
      assert(laghu_runtime_queue_try_take(&font_consumer, &font_job, ignored,
                                          sizeof(ignored)));
      assert(font_job.kind == LAGHU_RUNTIME_JOB_FONT_CSS);
      assert(strcmp(font_job.provider_id, "google_fonts") == 0);
      assert(laghu_sha256_hex(
          (laghu_buffer){google_css, sizeof(google_css) - 1U}, css_key));
      assert(laghu_runtime_cache_publish(
          temporary, css_key, css_key, css_key, "text/css", "test-font-fetch",
          (laghu_buffer){google_css, sizeof(google_css) - 1U}, &css_entry));
      strcpy(font_record.provider_id, google->id);
      strcpy(font_record.provider_digest, google->digest);
      strcpy(font_record.normalized_url,
             "https://fonts.googleapis.com/css2?family=Roboto");
      strcpy(font_record.variant_key, css_key);
      font_record.css_length = sizeof(google_css) - 1U;
      font_record.fetched_at = 100U;
      font_record.ttl_seconds = 604800U;
      font_record.ready = true;
      assert(laghu_font_stylesheet_publish(temporary, &font_record));
      assert(laghu_runtime_rewrite_font_css(
          &font_producer, temporary, &providers,
          (laghu_buffer){page, sizeof(page) - 1U}, 101U, true, true, 2048U,
          &font_page));
      assert(font_page.rewritten && !font_page.dependencies_pending);
      assert(strstr((const char *)font_page.data, "<style>@font-face") != NULL);
      assert(strstr((const char *)font_page.data, "fonts.googleapis.com") ==
             NULL);
      laghu_runtime_html_result_release(&font_page);
      laghu_runtime_queue_close(&font_consumer);
      laghu_runtime_queue_close(&font_producer);
    }
  }
  assert(snprintf(queue_path, sizeof(queue_path), "%s/jobs.queue", temporary) >
         0);
  assert(laghu_sha256_hex((laghu_buffer){payload, sizeof(payload) - 1U},
                          policy_key));
  test_head_planner_cache(temporary, policy_key);
  test_html_lexical_cache(temporary, policy_key);
  assert(laghu_runtime_index_key("/image.png", "etag", policy_key, true,
                                 index_key));
  assert(laghu_runtime_index_key("/image.png", "etag", policy_key, false,
                                 no_webp_index_key));
  assert(strcmp(index_key, no_webp_index_key) != 0);
  {
    laghu_cache_backend *backend = calloc(1U, sizeof(*backend));
    laghu_cache_limits limits;
    laghu_cache_stats stats;
    laghu_runtime_cache_entry backend_entry;
    char backend_path[LAGHU_RUNTIME_PATH_SIZE];
    laghu_cache_limits_init(&limits);
    limits.metadata_size = 16384U;
    limits.size_limit = sizeof(payload) - 1U;
    limits.inode_limit = 16U;
    assert(snprintf(backend_path, sizeof(backend_path), "%s/backend-cache",
                    temporary) > 0);
    assert(backend != NULL && laghu_runtime_directory_ensure(backend_path));
    assert(laghu_cache_backend_open_path(backend, backend_path, &limits));
    assert(laghu_cache_backend_publish(
        backend, index_key, policy_key, "etag", "text/plain", "test",
        (laghu_buffer){payload, sizeof(payload) - 1U}, &backend_entry));
    assert(
        laghu_cache_backend_lookup(backend, index_key, "etag", &backend_entry));
    assert(laghu_cache_backend_read(backend, &backend_entry, cached,
                                    sizeof(cached)));
    assert(laghu_cache_backend_health(backend, &stats));
    assert(stats.bytes == sizeof(payload) - 1U && stats.files == 3U &&
           stats.hits == 1U && stats.publications == 1U &&
           stats.cache_generation == 1U);
    {
      char metadata_path[LAGHU_RUNTIME_PATH_SIZE];
      unsigned char *metadata = malloc(16384U);
      FILE *file;
      size_t length;
      size_t cursor;
      bool payload_found = false;
      assert(snprintf(metadata_path, sizeof(metadata_path), "%s.laghu-metadata",
                      backend_path) > 0);
      file = fopen(metadata_path, "rb");
      assert(file != NULL && metadata != NULL);
      length = fread(metadata, 1U, 16384U, file);
      assert(fclose(file) == 0 && length == 16384U);
      for (cursor = 0U; cursor + sizeof(payload) - 1U <= length; ++cursor)
        if (memcmp(metadata + cursor, payload, sizeof(payload) - 1U) == 0)
          payload_found = true;
      assert(!payload_found);
      free(metadata);
    }
    assert(!laghu_cache_backend_publish(
        backend, no_webp_index_key, no_webp_index_key, "etag-2", "text/plain",
        "test", (laghu_buffer){payload, sizeof(payload) - 1U}, &backend_entry));
    assert(laghu_cache_backend_health(backend, &stats));
    assert(stats.rejected_publications == 1U);
    assert(laghu_cache_backend_maintain(backend, 100U));
    assert(!laghu_cache_backend_lookup(backend, index_key, "etag",
                                       &backend_entry));
    assert(laghu_cache_backend_health(backend, &stats));
    assert(stats.bytes == 0U && stats.files == 0U && stats.evictions == 1U);
    {
      char normalized[LAGHU_RUNTIME_PATH_SIZE];
      char source_hash[LAGHU_RUNTIME_KEY_SIZE];
      bool control = false;
      uint64_t matched = 0U, generation = 0U;
      assert(laghu_cache_source_normalize("/image.png?v=1&laghu=purge#ignored",
                                          normalized, sizeof(normalized),
                                          &control));
      assert(control && strcmp(normalized, "/image.png?v=1") == 0);
      assert(!laghu_cache_source_normalize("/image.png?laghu=purge&laghu=purge",
                                           normalized, sizeof(normalized),
                                           &control));
      assert(laghu_cache_source_hash("/image.png?v=1", source_hash));
      assert(laghu_cache_backend_associate(backend, index_key, source_hash));
      assert(laghu_cache_backend_publish(
          backend, index_key, policy_key, "etag", "text/plain", "test",
          (laghu_buffer){payload, sizeof(payload) - 1U}, &backend_entry));
      assert(laghu_cache_backend_purge_url(backend, "/image.png?v=1", 200U,
                                           &matched) ==
             LAGHU_CACHE_PURGE_ACCEPTED);
      assert(matched == 1U);
      assert(!laghu_cache_backend_lookup(backend, index_key, "etag",
                                         &backend_entry));
      assert(laghu_cache_backend_flush(backend, 7U, 201U, &generation));
      assert(generation == 7U);
      assert(!laghu_cache_backend_flush(backend, 6U, 202U, &generation));
      assert(laghu_cache_backend_health(backend, &stats));
      assert(stats.url_purges == 1U && stats.full_purges == 1U &&
             stats.invalidated_artifacts >= 1U && stats.cache_generation == 7U);
    }
    {
      char metadata_path[LAGHU_RUNTIME_PATH_SIZE];
      uint64_t zero = 0U;
      FILE *file;
      assert(snprintf(metadata_path, sizeof(metadata_path), "%s.laghu-metadata",
                      backend_path) > 0);
      file = fopen(metadata_path, "r+b");
      assert(file != NULL && fwrite(&zero, sizeof(zero), 1U, file) == 1U);
      assert(fclose(file) == 0);
      assert(laghu_cache_backend_health(backend, &stats));
      assert(stats.corrupt_removals == 1U && stats.rebuilding == false);
    }
    laghu_cache_backend_close(backend);
    ++limits.inode_limit;
    assert(!laghu_cache_backend_open_path(backend, backend_path, &limits));
    free(backend);
  }
  assert(laghu_runtime_queue_create(&producer, queue_path, 1U, 64U));
  assert(laghu_runtime_queue_set_backend(&producer, 0x55aaU, "test-vips"));
  assert(laghu_runtime_queue_heartbeat(&producer, 123456U));
  assert(laghu_runtime_queue_open(&consumer, queue_path));
  assert(consumer.capabilities == 0x55aaU);
  assert(consumer.worker_heartbeat == 123456U);
  assert(strcmp(consumer.backend_id, "test-vips") == 0);
  assert(laghu_runtime_queue_heartbeat(&producer, 123457U));
  assert(laghu_runtime_queue_refresh(&consumer));
  assert(consumer.worker_heartbeat == 123457U);
  memset(&submitted, 0, sizeof(submitted));
  strcpy(submitted.index_key, index_key);
  strcpy(submitted.request_path, "/image.png");
  strcpy(submitted.validator, "etag");
  strcpy(submitted.content_type, "image/png");
  strcpy(submitted.policy_key, policy_key);
  submitted.quality = 82U;
  submitted.metadata_limit = 10000U;
  submitted.metadata_ttl = 604800U;
  submitted.target_count = 2U;
  submitted.target_width[0] = 320U;
  submitted.target_width[1] = 640U;
  submitted.resize_filter[0] = UINT64_C(1) << 12;
  submitted.resize_filter[1] = UINT64_C(1) << 12;
  submitted.payload = (laghu_buffer){payload, sizeof(payload) - 1U};
  assert(laghu_runtime_queue_try_publish(&producer, &submitted));
  assert(!laghu_runtime_queue_try_publish(&producer, &submitted));
  assert(laghu_runtime_queue_try_take(&consumer, &taken, received,
                                      sizeof(received)));
  assert(taken.payload.length == sizeof(payload) - 1U);
  assert(memcmp(taken.payload.data, payload, taken.payload.length) == 0);
  assert(taken.target_count == 2U && taken.target_width[0] == 320U &&
         taken.target_width[1] == 640U);
  assert(taken.metadata_limit == 10000U && taken.metadata_ttl == 604800U);
  memset(&submitted, 0, sizeof(submitted));
  submitted.kind = LAGHU_RUNTIME_JOB_SPRITE;
  strcpy(submitted.index_key, index_key);
  strcpy(submitted.policy_key, policy_key);
  submitted.sprite_count = 2U;
  strcpy(submitted.sprite_variant_keys[0], index_key);
  strcpy(submitted.sprite_variant_keys[1], no_webp_index_key);
  submitted.sprite_width[0] = 20U;
  submitted.sprite_width[1] = 30U;
  assert(laghu_runtime_queue_try_publish(&producer, &submitted));
  assert(laghu_runtime_queue_try_take(&consumer, &taken, received,
                                      sizeof(received)));
  assert(taken.kind == LAGHU_RUNTIME_JOB_SPRITE && taken.sprite_count == 2U);
  assert(taken.payload.length == 0U && taken.sprite_width[1] == 30U);
  assert(strcmp(taken.sprite_variant_keys[1], no_webp_index_key) == 0);
  submitted.sprite_count = 1U;
  assert(!laghu_runtime_queue_try_publish(&producer, &submitted));
  submitted.sprite_count = 2U;
  strcpy(submitted.sprite_variant_keys[1], "not-a-content-key");
  assert(!laghu_runtime_queue_try_publish(&producer, &submitted));
  assert(laghu_javascript_target_normalize(
      "  Defaults   AND supports ES6-module and not dead  ",
      javascript_target));
  assert(strcmp(javascript_target,
                "defaults and supports es6-module and not dead") == 0);
  assert(!laghu_javascript_target_normalize("extends ../browser",
                                            javascript_target));
  {
    laghu_runtime_javascript_result javascript;
    assert(laghu_runtime_rewrite_javascript(
        &producer, temporary, (laghu_buffer){payload, sizeof(payload) - 1U},
        "/application.js", policy_key, "last 2 chrome versions", false, true,
        &javascript));
    assert(!javascript.rewritten && javascript.published);
    assert(laghu_runtime_queue_try_take(&consumer, &taken, received,
                                        sizeof(received)));
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
    static const unsigned char optimized[] =
        "function publicName(n){return n+1}";
    laghu_runtime_html_result javascript_page;
    laghu_runtime_cache_entry javascript_entry;
    char variant[LAGHU_RUNTIME_KEY_SIZE];
    assert(laghu_runtime_rewrite_javascript_html(
        &producer, temporary, (laghu_buffer){html, sizeof(html) - 1U},
        "/inline", policy_key, "last 2 chrome versions", NULL, 100U, 60U, NULL,
        NULL, NULL, 1U, NULL, false, false, false, false, false, false, 2048U,
        8192U, &javascript_page));
    assert(!javascript_page.rewritten && javascript_page.dependencies_pending);
    laghu_runtime_html_result_release(&javascript_page);
    assert(laghu_runtime_queue_try_take(&consumer, &taken, received,
                                        sizeof(received)));
    assert(taken.kind == LAGHU_RUNTIME_JOB_JAVASCRIPT);
    assert(laghu_sha256_hex((laghu_buffer){optimized, sizeof(optimized) - 1U},
                            variant));
    assert(laghu_runtime_cache_publish(
        temporary, taken.index_key, variant, taken.validator,
        "application/javascript", "swc-test",
        (laghu_buffer){optimized, sizeof(optimized) - 1U}, &javascript_entry));
    assert(laghu_runtime_rewrite_javascript_html(
        &producer, temporary, (laghu_buffer){html, sizeof(html) - 1U},
        "/inline", policy_key, "last 2 chrome versions", NULL, 101U, 60U, NULL,
        NULL, NULL, 1U, NULL, false, false, false, false, false, false, 2048U,
        8192U, &javascript_page));
    assert(javascript_page.rewritten &&
           javascript_page.length < sizeof(html) - 1U);
    assert(strstr((const char *)javascript_page.data,
                  "function publicName(n){return n+1}") != NULL);
    laghu_runtime_html_result_release(&javascript_page);
  }
  {
    static const char source[] =
        "console.log('one'); console.log('two'); console.log('three');";
    static const char derived[] = "console.log(1),console.log(2)";
    static const unsigned char html[] =
        "<html><body><script src=\"/small.js\"></script><p>padding padding "
        "padding</p></body></html>";
    laghu_runtime_html_result page;
    char fixture_variant[LAGHU_RUNTIME_KEY_SIZE];
    publish_javascript_fixture(temporary, "/small.js", policy_key,
                               "last 2 chrome versions", source, derived, 2U,
                               100U, fixture_variant);
    assert(laghu_runtime_rewrite_javascript_html(
        &producer, temporary, (laghu_buffer){html, sizeof(html) - 1U},
        "/inline-external", policy_key, "last 2 chrome versions", NULL, 101U,
        60U, NULL, NULL, NULL, 1U, NULL, false, false, false, true, false,
        false, 2048U, 8192U, &page));
    assert(page.rewritten);
    assert(strstr((const char *)page.data, "src=") == NULL);
    assert(strstr((const char *)page.data, derived) != NULL);
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
    const char *route, *map_route;
    char bundle_key[LAGHU_RUNTIME_KEY_SIZE];
    char map_key[LAGHU_RUNTIME_KEY_SIZE];
    char fixture_variant[LAGHU_RUNTIME_KEY_SIZE];
    assert(laghu_runtime_cache_publish(
        temporary,
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "application/json", "swc-test",
        (laghu_buffer){(const unsigned char *)"{\"version\":3}", 13U},
        &map_entry));
    assert(laghu_runtime_cache_publish(
        temporary,
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "application/json", "swc-test",
        (laghu_buffer){(const unsigned char *)"{\"version\":3}", 13U},
        &map_entry));
    publish_javascript_fixture(temporary, "/assets/application-one-entry.js",
                               policy_key, "last 2 chrome versions",
                               first_source, first, 2U | 4U | 8U, 100U,
                               fixture_variant);
    publish_javascript_fixture(temporary, "/assets/application-two-entry.js",
                               policy_key, "last 2 chrome versions",
                               second_source, second, 2U | 4U | 8U, 100U,
                               fixture_variant);
    assert(laghu_runtime_rewrite_javascript_html(
        &producer, temporary, (laghu_buffer){html, sizeof(html) - 1U},
        "/combine", policy_key, "last 2 chrome versions", NULL, 101U, 60U, NULL,
        NULL, NULL, 1U, NULL, false, false, true, false, false, true, 2048U,
        8192U, &page));
    assert(page.rewritten);
    assert(strstr((const char *)page.data, "/.laghu/js/") != NULL);
    assert(strstr((const char *)page.data,
                  "data-laghu-combine=\"application-main\"") != NULL);
    route = strstr((const char *)page.data, "/.laghu/js/");
    assert(route != NULL);
    memcpy(bundle_key, route + 11U, LAGHU_SHA256_HEX_LENGTH);
    bundle_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
    assert(laghu_runtime_cache_lookup_variant(temporary, bundle_key,
                                              &bundle_entry));
    assert(bundle_entry.length < sizeof(bundle));
    assert(
        laghu_runtime_cache_read(&bundle_entry, bundle, bundle_entry.length));
    bundle[bundle_entry.length] = '\0';
    map_route = strstr((const char *)bundle, "sourceMappingURL=/.laghu/js/");
    assert(map_route != NULL);
    memcpy(map_key, map_route + sizeof("sourceMappingURL=/.laghu/js/") - 1U,
           LAGHU_SHA256_HEX_LENGTH);
    map_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
    assert(laghu_runtime_cache_lookup_variant(temporary, map_key, &map_entry));
    assert(map_entry.length < sizeof(map));
    assert(laghu_runtime_cache_read(&map_entry, map, map_entry.length));
    map[map_entry.length] = '\0';
    assert(strstr((const char *)map, "\"version\":3") != NULL);
    assert(strstr((const char *)map, "\"sections\":[") != NULL);
    assert(strstr((const char *)map, "aaaaaaaaaaaaaaaa") != NULL);
    assert(strstr((const char *)map, "bbbbbbbbbbbbbbbb") != NULL);
    laghu_runtime_html_result_release(&page);
  }
  {
    static const char source[] =
        "console.log('approved deferred script with enough source bytes');";
    static const char derived[] = "console.log('deferred')";
    static const unsigned char html[] =
        "<html><body><script "
        "src=\"/assets/deferred.js\"></script></body></html>";
    static const char template_key[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    laghu_javascript_defer_set defer = {0};
    laghu_rum_instrumentation_record record = {0};
    laghu_runtime_html_result page;
    char absolute[] = "https://example.test/assets/deferred.js";
    char fixture_variant[LAGHU_RUNTIME_KEY_SIZE];
    publish_javascript_fixture(temporary, "/assets/deferred.js", policy_key,
                               "last 2 chrome versions", source, derived, 16U,
                               100U, fixture_variant);
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
    assert(laghu_sha256_hex(
        (laghu_buffer){(const unsigned char *)absolute, strlen(absolute)},
        record.script_keys[0]));
    record.observations[1] = 100U;
    record.script_observations[1][0] = 90U;
    assert(laghu_rum_engine_publish(
        rum_engine, LAGHU_RUM_RECORD_INSTRUMENTATION, template_key, 100U,
        &record, sizeof(record), NULL));
    assert(laghu_runtime_rewrite_javascript_html(
        &producer, temporary, (laghu_buffer){html, sizeof(html) - 1U}, "/page",
        policy_key, "last 2 chrome versions", NULL, 101U, 60U, rum_engine,
        template_key, "https://example.test", 1U, &defer, true, true, false,
        false, false, false, 2048U, 8192U, &page));
    assert(page.rewritten &&
           strstr((const char *)page.data,
                  "src=\"/assets/deferred.js\" defer") != NULL);
    laghu_runtime_html_result_release(&page);
  }
  assert(laghu_catalog_key("/image.png", index_key, policy_key, 0x55aaU,
                           catalog_key));
  catalog.version = LAGHU_CATALOG_VERSION;
  strcpy(catalog.normalized_url, "/image.png");
  strcpy(catalog.source_hash, index_key);
  strcpy(catalog.policy_key, policy_key);
  catalog.capability_mask = 0x55aaU;
  catalog.natural_width = 800U;
  catalog.natural_height = 600U;
  catalog.variant_count = 2U;
  catalog.variants[0].width = 320U;
  catalog.variants[0].ready = true;
  catalog.updated_at = 1000U;
  catalog.last_accessed_at = 1000U;
  assert(laghu_catalog_publish(temporary, catalog_key, &catalog));
  assert(laghu_catalog_lookup(temporary, catalog_key, 1001U, 604800U, &loaded));
  assert(loaded.natural_width == 800U && loaded.variant_count == 2U);
  {
    static const unsigned char json[] =
        "{\"url\":\"/image.png\",\"width\":240,\"height\":180,"
        "\"viewport_width\":390,\"dpr_hundredths\":200,"
        "\"above_fold\":true,\"mobile\":true}";
    laghu_image_beacon_record beacon;
    assert(laghu_runtime_parse_image_beacon(
        (laghu_buffer){json, sizeof(json) - 1U}, &beacon));
    assert(beacon.mobile && beacon.above_fold && beacon.width == 240U);
    assert(laghu_catalog_apply_beacon(rum_engine, temporary, policy_key,
                                      0x55aaU, 1002U, 604800U, &beacon));
    assert(laghu_catalog_lookup_url(temporary, "/image.png", policy_key,
                                    0x55aaU, 1003U, 604800U, &loaded));
    {
      char identity[LAGHU_RUNTIME_KEY_SIZE];
      laghu_rum_image_record learning;
      laghu_rum_value value;
      assert(laghu_catalog_url_identity("/image.png", policy_key, 0x55aaU,
                                        identity));
      assert(laghu_rum_engine_read(rum_engine, LAGHU_RUM_RECORD_IMAGE, identity,
                                   1003U, &learning, sizeof(learning), &value));
      assert(value.length == sizeof(learning));
      assert(learning.mobile_width == 240U && learning.above_fold);
    }
    {
      static const unsigned char invalid[] = "{\"url\":\"https://x\"}";
      assert(!laghu_runtime_parse_image_beacon(
          (laghu_buffer){invalid, sizeof(invalid) - 1U}, &beacon));
    }
  }
  assert(
      !laghu_catalog_lookup(temporary, catalog_key, 700000U, 604800U, &loaded));
  {
    char lock_path[LAGHU_RUNTIME_PATH_SIZE];
    FILE *lock;
    memset(&catalog, 0, sizeof(catalog));
    catalog.version = LAGHU_CATALOG_VERSION;
    strcpy(catalog.normalized_url, "/locked.png");
    strcpy(catalog.source_hash, index_key);
    strcpy(catalog.policy_key, policy_key);
    catalog.capability_mask = 0x55aaU;
    catalog.updated_at = 800000U;
    catalog.last_accessed_at = 800000U;
    assert(laghu_catalog_key(catalog.normalized_url, catalog.source_hash,
                             catalog.policy_key, catalog.capability_mask,
                             catalog_key));
    assert(snprintf(lock_path, sizeof(lock_path), "%s/catalog/%s.meta.lock",
                    temporary, catalog_key) > 0);
    lock = fopen(lock_path, "wb");
    assert(lock != NULL && fclose(lock) == 0);
    assert(!laghu_catalog_publish(temporary, catalog_key, &catalog));
    assert(remove(lock_path) == 0);
    assert(laghu_catalog_publish(temporary, catalog_key, &catalog));

    strcpy(catalog.normalized_url, "/newest.png");
    catalog.updated_at = 800100U;
    catalog.last_accessed_at = 800100U;
    assert(laghu_catalog_publish_url(temporary, &catalog));
    assert(laghu_catalog_prune(temporary, 800101U, 1U, 604800U));
    assert(!laghu_catalog_lookup_url(temporary, "/locked.png", policy_key,
                                     0x55aaU, 800101U, 604800U, &loaded));
    assert(laghu_catalog_lookup_url(temporary, "/newest.png", policy_key,
                                    0x55aaU, 800101U, 604800U, &loaded));
  }
  assert(laghu_runtime_cache_publish(
      temporary, index_key, policy_key, "etag", "image/png", "test-backend",
      (laghu_buffer){payload, sizeof(payload) - 1U}, &entry));
  {
    static const unsigned char css[] =
        "/*! keep */\n.hero { color: red; margin: 0  0; } /* remove */\n";
    laghu_runtime_css_result stylesheet;
    assert(laghu_runtime_rewrite_css(
        NULL, temporary, (laghu_buffer){css, sizeof(css) - 1U}, "/site.css",
        "https://example.test", policy_key, 0x55aaU, 900000U, 604800U, true,
        false, 2048U, 8192U, &stylesheet));
    assert(!stylesheet.rewritten && stylesheet.published);
    laghu_runtime_css_result_release(&stylesheet);
    assert(laghu_runtime_rewrite_css(
        NULL, temporary, (laghu_buffer){css, sizeof(css) - 1U}, "/site.css",
        "https://example.test", policy_key, 0x55aaU, 900001U, 604800U, true,
        false, 2048U, 8192U, &stylesheet));
    assert(stylesheet.rewritten && !stylesheet.published);
    assert(strstr((const char *)stylesheet.data, "/*! keep */") != NULL);
    assert(strstr((const char *)stylesheet.data, "remove") == NULL);
    laghu_runtime_css_result_release(&stylesheet);
    {
      static const unsigned char leaf_css[] =
          ".leaf { color: blue; margin: 0  0  0  0; } /* leaf remove */";
      static const unsigned char root_css[] =
          "@import \"/leaf.css\" print; .root { color: red; padding: 0  0; }";
      laghu_runtime_css_result imported;
      assert(laghu_runtime_rewrite_css(
          NULL, temporary, (laghu_buffer){leaf_css, sizeof(leaf_css) - 1U},
          "/leaf.css", "https://example.test", policy_key, 0x55aaU, 900010U,
          604800U, true, false, 2048U, 8192U, &imported));
      assert(imported.published && !imported.rewritten);
      laghu_runtime_css_result_release(&imported);
      assert(laghu_runtime_rewrite_css(
          NULL, temporary, (laghu_buffer){leaf_css, sizeof(leaf_css) - 1U},
          "/leaf.css", "https://example.test", policy_key, 0x55aaU, 900011U,
          604800U, true, false, 2048U, 8192U, &imported));
      assert(imported.rewritten);
      laghu_runtime_css_result_release(&imported);
      assert(laghu_runtime_rewrite_css(
          NULL, temporary, (laghu_buffer){root_css, sizeof(root_css) - 1U},
          "/root.css", "https://example.test", policy_key, 0x55aaU, 900012U,
          604800U, true, true, 2048U, 8192U, &imported));
      assert(imported.published && !imported.rewritten);
      laghu_runtime_css_result_release(&imported);
      assert(laghu_runtime_rewrite_css(
          NULL, temporary, (laghu_buffer){root_css, sizeof(root_css) - 1U},
          "/root.css", "https://example.test", policy_key, 0x55aaU, 900013U,
          604800U, true, true, 2048U, 8192U, &imported));
      assert(imported.rewritten &&
             strstr((const char *)imported.data, "@import") == NULL);
      assert(strstr((const char *)imported.data, "@media print{") != NULL);
      assert(strstr((const char *)imported.data, ".leaf") != NULL);
      laghu_runtime_css_result_release(&imported);

      {
        static const unsigned char cycle_a[] = "@import '/cycle-b.css';";
        static const unsigned char cycle_b[] = "@import '/cycle-a.css';";
        assert(laghu_runtime_rewrite_css(
            NULL, temporary, (laghu_buffer){cycle_a, sizeof(cycle_a) - 1U},
            "/cycle-a.css", "https://example.test", policy_key, 0x55aaU,
            900014U, 604800U, true, true, 2048U, 8192U, &imported));
        assert(imported.dependencies_pending && !imported.rewritten);
        laghu_runtime_css_result_release(&imported);
        assert(laghu_runtime_rewrite_css(
            NULL, temporary, (laghu_buffer){cycle_b, sizeof(cycle_b) - 1U},
            "/cycle-b.css", "https://example.test", policy_key, 0x55aaU,
            900015U, 604800U, true, true, 2048U, 8192U, &imported));
        assert(!imported.rewritten && !imported.dependencies_pending);
        laghu_runtime_css_result_release(&imported);
      }
    }
    {
      static const unsigned char linked[] =
          "<html><head><link rel=\"stylesheet\" href=\"/site.css\"></head>"
          "<body></body></html>";
      laghu_runtime_html_result markup;
      assert(laghu_runtime_rewrite_css_markup(
          temporary, (laghu_buffer){linked, sizeof(linked) - 1U}, "/index.html",
          "https://example.test", policy_key, 0x55aaU, 900002U, 604800U, true,
          false, false, 0U, true, true, 2048U, 8192U, &markup));
      assert(!markup.rewritten && markup.dependencies_pending);
      laghu_runtime_html_result_release(&markup);
      assert(laghu_runtime_rewrite_css_markup(
          temporary, (laghu_buffer){linked, sizeof(linked) - 1U}, "/index.html",
          "https://example.test", policy_key, 0x55aaU, 900003U, 604800U, true,
          false, false, 0U, true, true, 2048U, 8192U, &markup));
      assert(markup.rewritten && !markup.dependencies_pending);
      assert(strstr((const char *)markup.data, "<style>") != NULL);
      assert(strstr((const char *)markup.data, "href=") == NULL);
      laghu_runtime_html_result_release(&markup);
      assert(laghu_runtime_rewrite_css_markup(
          temporary, (laghu_buffer){linked, sizeof(linked) - 1U}, "/index.html",
          "https://example.test", policy_key, 0x55aaU, 900003U, 604800U, true,
          false, false, 0U, false, true, 2048U, 8192U, &markup));
      assert(!markup.rewritten && !markup.dependencies_pending);
      laghu_runtime_html_result_release(&markup);
    }
    {
      static const unsigned char long_leaf_css[] =
          ".linked { color: green; margin: 0  0  0  0; }";
      char long_path[256U];
      char import_html[512U];
      laghu_runtime_css_result sheet;
      laghu_runtime_html_result markup;
      size_t index;
      int length;
      long_path[0] = '/';
      for (index = 1U; index < 190U; ++index) {
        long_path[index] = 'a';
      }
      strcpy(long_path + 190U, ".css");
      assert(laghu_runtime_rewrite_css(
          NULL, temporary,
          (laghu_buffer){long_leaf_css, sizeof(long_leaf_css) - 1U}, long_path,
          "https://example.test", policy_key, 0x55aaU, 900020U, 604800U, true,
          false, 2048U, 8192U, &sheet));
      laghu_runtime_css_result_release(&sheet);
      assert(laghu_runtime_rewrite_css(
          NULL, temporary,
          (laghu_buffer){long_leaf_css, sizeof(long_leaf_css) - 1U}, long_path,
          "https://example.test", policy_key, 0x55aaU, 900021U, 604800U, true,
          false, 2048U, 8192U, &sheet));
      assert(sheet.rewritten);
      laghu_runtime_css_result_release(&sheet);
      length = snprintf(import_html, sizeof(import_html),
                        "<style>@import \"%s\" print;</style>", long_path);
      assert(length > 0 && (size_t)length < sizeof(import_html));
      assert(laghu_runtime_rewrite_css_markup(
          temporary,
          (laghu_buffer){(const unsigned char *)import_html, (size_t)length},
          "/index.html", "https://example.test", policy_key, 0x55aaU, 900022U,
          604800U, false, true, false, 0U, false, true, 2048U, 8192U, &markup));
      assert(!markup.rewritten && markup.dependencies_pending);
      laghu_runtime_html_result_release(&markup);
      assert(laghu_runtime_rewrite_css_markup(
          temporary,
          (laghu_buffer){(const unsigned char *)import_html, (size_t)length},
          "/index.html", "https://example.test", policy_key, 0x55aaU, 900023U,
          604800U, false, true, false, 0U, false, true, 2048U, 8192U, &markup));
      assert(markup.rewritten && strstr((const char *)markup.data,
                                        "<link rel=\"stylesheet\"") != NULL);
      assert(strstr((const char *)markup.data, "media=\"print\"") != NULL);
      assert(strstr((const char *)markup.data, "@import") == NULL);
      laghu_runtime_html_result_release(&markup);
    }
    {
      unsigned char *outlined = malloc(10000U);
      laghu_runtime_html_result markup;
      size_t offset = 0U;
      assert(outlined != NULL);
      memcpy(outlined + offset, "<style>", 7U);
      offset += 7U;
      while (offset + 32U < 9980U) {
        memcpy(outlined + offset, ".x { color: red; margin: 0 0; } ", 32U);
        offset += 32U;
      }
      memcpy(outlined + offset, "</style>", 8U);
      offset += 8U;
      assert(laghu_runtime_rewrite_css_markup(
          temporary, (laghu_buffer){outlined, offset}, "/outline.html",
          "https://example.test", policy_key, 0x55aaU, 900003U, 604800U, false,
          true, false, 0U, false, true, 2048U, 8192U, &markup));
      assert(!markup.rewritten && markup.dependencies_pending);
      laghu_runtime_html_result_release(&markup);
      assert(laghu_runtime_rewrite_css_markup(
          temporary, (laghu_buffer){outlined, offset}, "/outline.html",
          "https://example.test", policy_key, 0x55aaU, 900004U, 604800U, false,
          true, false, 0U, false, true, 2048U, 8192U, &markup));
      assert(!markup.rewritten && markup.dependencies_pending);
      laghu_runtime_html_result_release(&markup);
      assert(laghu_runtime_rewrite_css_markup(
          temporary, (laghu_buffer){outlined, offset}, "/outline.html",
          "https://example.test", policy_key, 0x55aaU, 900005U, 604800U, false,
          true, false, 0U, false, true, 2048U, 8192U, &markup));
      assert(markup.rewritten && !markup.dependencies_pending);
      assert(strstr((const char *)markup.data, "/.laghu/css/") != NULL);
      laghu_runtime_html_result_release(&markup);
      free(outlined);
    }
    {
      static const unsigned char first_css[] =
          ".first { color: red; padding: 0  0  0  0; }\n";
      static const unsigned char second_css[] =
          ".second { color: blue; margin: 0  0  0  0; }\n";
      static const unsigned char combined_html[] =
          "<html><head><link rel=\"stylesheet\" href=\"/first.css\"> \n"
          "<link rel=\"stylesheet\" href=\"/second.css\"></head></html>";
      static const unsigned char split_html[] =
          "<link rel=\"stylesheet\" href=\"/first.css\"><!-- split -->"
          "<link rel=\"stylesheet\" href=\"/second.css\">";
      static const unsigned char media_html[] =
          "<link rel=\"stylesheet\" href=\"/first.css\" media=\"print\">"
          "<link rel=\"stylesheet\" href=\"/second.css\" media=\"screen\">";
      laghu_runtime_css_result sheet;
      laghu_runtime_css_combine_result combined;
      laghu_runtime_cache_entry combined_entry;
      unsigned char combined_body[256U];
      char first_key[LAGHU_RUNTIME_KEY_SIZE];
      assert(laghu_runtime_rewrite_css(
          NULL, temporary, (laghu_buffer){first_css, sizeof(first_css) - 1U},
          "/first.css", "https://example.test", policy_key, 0x55aaU, 910000U,
          604800U, true, false, 2048U, 8192U, &sheet));
      laghu_runtime_css_result_release(&sheet);
      assert(laghu_runtime_rewrite_css(
          NULL, temporary, (laghu_buffer){first_css, sizeof(first_css) - 1U},
          "/first.css", "https://example.test", policy_key, 0x55aaU, 910001U,
          604800U, true, false, 2048U, 8192U, &sheet));
      laghu_runtime_css_result_release(&sheet);
      assert(laghu_runtime_rewrite_css(
          NULL, temporary, (laghu_buffer){second_css, sizeof(second_css) - 1U},
          "/second.css", "https://example.test", policy_key, 0x55aaU, 910000U,
          604800U, true, false, 2048U, 8192U, &sheet));
      laghu_runtime_css_result_release(&sheet);
      assert(laghu_runtime_rewrite_css(
          NULL, temporary, (laghu_buffer){second_css, sizeof(second_css) - 1U},
          "/second.css", "https://example.test", policy_key, 0x55aaU, 910001U,
          604800U, true, false, 2048U, 8192U, &sheet));
      laghu_runtime_css_result_release(&sheet);
      assert(laghu_runtime_combine_css_markup(
          temporary, (laghu_buffer){combined_html, sizeof(combined_html) - 1U},
          "/index.html", "https://example.test", policy_key, 0x55aaU, 910002U,
          604800U, 2048U, 8192U, &combined));
      assert(combined.rewritten && !combined.dependencies_pending);
      assert(strstr((const char *)combined.data, "/.laghu/css/") != NULL);
      {
        const char *route = strstr((const char *)combined.data, "/.laghu/css/");
        assert(route != NULL);
        memcpy(first_key, route + sizeof("/.laghu/css/") - 1U,
               LAGHU_SHA256_HEX_LENGTH);
        first_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
      }
      assert(laghu_runtime_cache_lookup_variant(temporary, first_key,
                                                &combined_entry));
      assert(combined_entry.length < sizeof(combined_body));
      assert(laghu_runtime_cache_read(&combined_entry, combined_body,
                                      sizeof(combined_body)));
      combined_body[combined_entry.length] = '\0';
      assert(strstr((const char *)combined_body, ".first") <
             strstr((const char *)combined_body, ".second"));
      laghu_runtime_css_combine_result_release(&combined);
      {
        static const unsigned char changed_css[] =
            ".first { color: green; padding: 0  0  0  0; }\n";
        char changed_key[LAGHU_RUNTIME_KEY_SIZE];
        assert(laghu_runtime_rewrite_css(
            NULL, temporary,
            (laghu_buffer){changed_css, sizeof(changed_css) - 1U}, "/first.css",
            "https://example.test", policy_key, 0x55aaU, 910004U, 604800U, true,
            false, 2048U, 8192U, &sheet));
        laghu_runtime_css_result_release(&sheet);
        assert(laghu_runtime_rewrite_css(
            NULL, temporary,
            (laghu_buffer){changed_css, sizeof(changed_css) - 1U}, "/first.css",
            "https://example.test", policy_key, 0x55aaU, 910005U, 604800U, true,
            false, 2048U, 8192U, &sheet));
        laghu_runtime_css_result_release(&sheet);
        assert(laghu_runtime_combine_css_markup(
            temporary,
            (laghu_buffer){combined_html, sizeof(combined_html) - 1U},
            "/index.html", "https://example.test", policy_key, 0x55aaU, 910006U,
            604800U, 2048U, 8192U, &combined));
        assert(combined.rewritten);
        {
          const char *route =
              strstr((const char *)combined.data, "/.laghu/css/");
          assert(route != NULL);
          memcpy(changed_key, route + sizeof("/.laghu/css/") - 1U,
                 LAGHU_SHA256_HEX_LENGTH);
          changed_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
        }
        assert(strcmp(first_key, changed_key) != 0);
        laghu_runtime_css_combine_result_release(&combined);
      }
      assert(laghu_runtime_combine_css_markup(
          temporary, (laghu_buffer){split_html, sizeof(split_html) - 1U},
          "/index.html", "https://example.test", policy_key, 0x55aaU, 910003U,
          604800U, 2048U, 8192U, &combined));
      assert(!combined.rewritten && !combined.dependencies_pending);
      laghu_runtime_css_combine_result_release(&combined);
      assert(laghu_runtime_combine_css_markup(
          temporary, (laghu_buffer){media_html, sizeof(media_html) - 1U},
          "/index.html", "https://example.test", policy_key, 0x55aaU, 910003U,
          604800U, 2048U, 8192U, &combined));
      assert(!combined.rewritten && !combined.dependencies_pending);
      laghu_runtime_css_combine_result_release(&combined);
      assert(
          laghu_runtime_csp_allows_self_styles(NULL, "https://example.test"));
      assert(laghu_runtime_csp_allows_self_styles(
          "default-src 'none'; style-src 'self'", "https://example.test"));
      assert(laghu_runtime_csp_allows_self_styles(
          "style-src-elem https://example.test; style-src 'none'",
          "https://example.test"));
      assert(!laghu_runtime_csp_allows_self_styles(
          "style-src-elem 'none'; style-src 'self'", "https://example.test"));
      assert(!laghu_runtime_csp_allows_self_styles("default-src 'none'",
                                                   "https://example.test"));
      assert(laghu_runtime_csp_allows_self_scripts(
          "default-src 'none'; script-src 'self'; style-src 'none'",
          "https://example.test"));
      assert(!laghu_runtime_csp_allows_self_scripts(
          "default-src 'self'; script-src 'none'; style-src 'self'",
          "https://example.test"));
    }
  }
  {
    static const unsigned char html[] =
        "<html><body><img src=\"/image.png\" width=\"320\"></body></html>";
    static const unsigned char inline_html[] =
        "<img src=\"/image.png\" width=\"320\"><img src=\"/image.png\" "
        "width=\"320\">";
    static const unsigned char tiny_png[] = {0x89U, 'P',   'N',   'G',
                                             0x0dU, 0x0aU, 0x1aU, 0x0aU};
    char first_key[LAGHU_RUNTIME_KEY_SIZE];
    char second_key[LAGHU_RUNTIME_KEY_SIZE];
    laghu_runtime_html_result page;
    assert(laghu_sha256_hex((laghu_buffer){tiny_png, sizeof(tiny_png)},
                            first_key));
    assert(laghu_sha256_hex((laghu_buffer){(const unsigned char *)"second", 6U},
                            second_key));
    assert(laghu_runtime_cache_publish(
        temporary, no_webp_index_key, first_key, "page", "image/png",
        "test-backend", (laghu_buffer){tiny_png, sizeof(tiny_png)}, &entry));
    assert(laghu_runtime_cache_publish(
        temporary, catalog_key, second_key, "page", "image/png", "test-backend",
        (laghu_buffer){payload, sizeof(payload) - 1U}, &entry));
    memset(&catalog, 0, sizeof(catalog));
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
    catalog.variants[1].variant_length = sizeof(payload) - 1U;
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
      assert(laghu_runtime_cache_publish(
          temporary, css_key, css_key, css_key, "text/css", "test-css",
          (laghu_buffer){css, sizeof(css) - 1U}, &entry));
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
          temporary, (laghu_buffer){hinted_html, sizeof(hinted_html) - 1U},
          "/index.html", "https://example.test", policy_key, 0x55aaU, 2001U,
          604800U,
          LAGHU_HTML_PLAN_CONVERT_META_TAGS | LAGHU_HTML_PLAN_RESOURCE_HINTS,
          NULL, NULL, 2048U, 8192U, false, &hints));
      assert(hints.dependencies_pending && !hints.rewritten &&
             hints.link_header_count == 0U);
      laghu_runtime_html_result_release(&hints);
      assert(laghu_runtime_finalize_html_headers(
          temporary, (laghu_buffer){hinted_html, sizeof(hinted_html) - 1U},
          "/index.html", "https://example.test", policy_key, 0x55aaU, 2001U,
          604800U,
          LAGHU_HTML_PLAN_CONVERT_META_TAGS | LAGHU_HTML_PLAN_RESOURCE_HINTS,
          NULL, NULL, 2048U, 8192U, false, &hints));
      assert(hints.rewritten && hints.set_content_language);
      assert(strcmp(hints.content_language, "en-CA") == 0);
      assert(hints.link_header_count == 3U);
      assert(strstr(hints.link_headers[0], "/.laghu/css/") != NULL);
      assert(strstr(hints.link_headers[1], "/.laghu/image/") != NULL);
      assert(strstr(hints.link_headers[2], "https://cdn.example.test") != NULL);
      assert(strstr((const char *)hints.data, "Content-Language") == NULL);
      laghu_runtime_html_result_release(&hints);
      assert(laghu_runtime_finalize_html_headers(
          temporary, (laghu_buffer){hinted_html, sizeof(hinted_html) - 1U},
          "/index.html", "https://example.test", policy_key, 0x55aaU, 2001U,
          604800U, LAGHU_HTML_PLAN_CONVERT_META_TAGS, "fr-CA", NULL, 2048U,
          8192U, true, &hints));
      assert(hints.invalid && !hints.rewritten && !hints.set_content_language &&
             hints.link_header_count == 0U);
      laghu_runtime_html_result_release(&hints);
      {
        static const unsigned char deduplicated[] =
            "<html><head><meta http-equiv=content-language content=en>"
            "<link rel=preconnect href=https://cdn.example.test></head>"
            "<body><script src=https://cdn.example.test/a.js></script></body>"
            "</html>";
        assert(laghu_runtime_finalize_html_headers(
            temporary, (laghu_buffer){deduplicated, sizeof(deduplicated) - 1U},
            "/index.html", "https://example.test", policy_key, 0x55aaU, 2001U,
            604800U,
            LAGHU_HTML_PLAN_CONVERT_META_TAGS | LAGHU_HTML_PLAN_RESOURCE_HINTS,
            NULL, NULL, 2048U, 8192U, true, &hints));
        assert(hints.rewritten && hints.set_content_language &&
               hints.link_header_count == 0U);
        laghu_runtime_html_result_release(&hints);
        assert(laghu_runtime_finalize_html_headers(
            temporary, (laghu_buffer){deduplicated, sizeof(deduplicated) - 1U},
            "/index.html", "https://example.test", policy_key, 0x55aaU, 2001U,
            604800U,
            LAGHU_HTML_PLAN_CONVERT_META_TAGS | LAGHU_HTML_PLAN_RESOURCE_HINTS,
            NULL, "malformed", 2048U, 8192U, true, &hints));
        assert(hints.rewritten && hints.set_content_language &&
               hints.link_header_count == 0U);
        laghu_runtime_html_result_release(&hints);
      }
    }
    assert(laghu_runtime_rewrite_html(
        rum_engine, temporary, (laghu_buffer){html, sizeof(html) - 1U},
        "/index.html", "https://example.test", policy_key, 0x55aaU, 2001U,
        604800U,
        LAGHU_IMAGE_INSERT_DIMENSIONS | LAGHU_IMAGE_RESPONSIVE |
            LAGHU_IMAGE_RESPONSIVE_ZOOM | LAGHU_IMAGE_LAZYLOAD,
        false, false, false, false, 0U, false, false, true, false, 2048U, 2048U,
        8192U, 0U, 100U, &page));
    assert(page.rewritten && !page.dependencies_pending);
    assert(strstr((const char *)page.data, "/.laghu/image/") != NULL);
    assert(strstr((const char *)page.data, " 320w") != NULL);
    assert(strstr((const char *)page.data, " 640w") != NULL);
    laghu_runtime_html_result_release(&page);
    assert(laghu_runtime_rewrite_html(
        rum_engine, temporary,
        (laghu_buffer){inline_html, sizeof(inline_html) - 1U}, "/index.html",
        "https://example.test", policy_key, 0x55aaU, 2001U, 604800U,
        LAGHU_IMAGE_INLINE | LAGHU_IMAGE_DEDUP_INLINE, true, false, false,
        false, 0U, true, true, true, false, 2048U, 2048U, 8192U, 0U, 100U,
        &page));
    assert(page.rewritten);
    assert(strstr((const char *)page.data, "data:image/png;base64,") != NULL);
    assert(strstr((const char *)page.data, "/.laghu/image/") != NULL);
    laghu_runtime_html_result_release(&page);
    assert(laghu_runtime_rewrite_html(
        rum_engine, temporary,
        (laghu_buffer){inline_html, sizeof(inline_html) - 1U}, "/index.html",
        "https://example.test", policy_key, 0x55aaU, 2001U, 604800U,
        LAGHU_IMAGE_INLINE | LAGHU_IMAGE_DEDUP_INLINE, true, false, false,
        false, 0U, false, true, true, false, 2048U, 2048U, 8192U, 0U, 100U,
        &page));
    assert(page.rewritten);
    assert(strstr((const char *)page.data, "data:image/") == NULL);
    laghu_runtime_html_result_release(&page);
  }
  assert(!laghu_runtime_cache_publish(
      NULL, index_key, policy_key, "etag", "image/png", "test-backend",
      (laghu_buffer){payload, sizeof(payload) - 1U}, &entry));
  {
    static const unsigned char css[] =
        ".hero{color:red}.footer{color:blue}@font-face{font-family:x;src:url(x."
        "woff2)}";
    static const unsigned char critical_html[] =
        "<html><head><link rel=\"stylesheet\" href=\"/critical.css\"></head>"
        "<body><div class=hero>hero</div></body></html>";
    laghu_stylesheet_record stylesheet = {0};
    laghu_runtime_cache_entry css_entry;
    laghu_runtime_html_result critical_page;
    laghu_critical_css_beacon observation = {0};
    const char *marker;
    char css_key[LAGHU_RUNTIME_KEY_SIZE];
    unsigned int observation_index;
    assert(laghu_sha256_hex((laghu_buffer){css, sizeof(css) - 1U}, css_key));
    assert(laghu_runtime_cache_publish(
        temporary, css_key, css_key, css_key, "text/css", "critical-test",
        (laghu_buffer){css, sizeof(css) - 1U}, &css_entry));
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
    assert(laghu_stylesheet_publish(temporary, &stylesheet));
    assert(laghu_runtime_prioritize_critical_css(
        rum_engine, temporary,
        (laghu_buffer){critical_html, sizeof(critical_html) - 1U}, "/critical",
        "https://example.test", policy_key, 0x55aaU, 2001U, 604800U, 2048U,
        8192U, 1024U, true, true, true, true, &critical_page));
    assert(!critical_page.rewritten);
    laghu_runtime_html_result_release(&critical_page);
    assert(laghu_runtime_prioritize_critical_css(
        rum_engine, temporary,
        (laghu_buffer){critical_html, sizeof(critical_html) - 1U}, "/critical",
        "https://example.test", policy_key, 0x55aaU, 2001U, 604800U, 2048U,
        8192U, 1024U, true, true, true, true, &critical_page));
    assert(critical_page.rewritten);
    marker = strstr((const char *)critical_page.data, "data-laghu-critical=\"");
    assert(marker != NULL);
    marker += sizeof("data-laghu-critical=\"") - 1U;
    memcpy(observation.template_key, marker, LAGHU_SHA256_HEX_LENGTH);
    observation.template_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
    observation.viewport_bucket = 1U;
    observation.rule_count = 1U;
    observation.rules[0] = 0U;
    laghu_runtime_html_result_release(&critical_page);
    for (observation_index = 0U; observation_index < 3U; ++observation_index)
      assert(laghu_critical_css_apply_beacon(rum_engine, temporary, policy_key,
                                             2002U + observation_index, 604800U,
                                             &observation));
    assert(laghu_runtime_prioritize_critical_css(
        rum_engine, temporary,
        (laghu_buffer){critical_html, sizeof(critical_html) - 1U}, "/critical",
        "https://example.test", policy_key, 0x55aaU, 2005U, 604800U, 2048U,
        8192U, 1024U, true, true, true, true, &critical_page));
    assert(critical_page.rewritten);
    assert(strstr((const char *)critical_page.data,
                  "<style>.hero{color:red}@font-face") != NULL);
    assert(strstr((const char *)critical_page.data, "</style></head><body>") !=
           NULL);
    assert(strstr((const char *)critical_page.data, "/.laghu/css/") != NULL);
    laghu_runtime_html_result_release(&critical_page);
    {
      static const unsigned char json[] =
          "{\"template\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
          "aaaaaaaaaaaaaaaa\",\"bucket\":0,\"rules\":[0,7]}";
      laghu_critical_css_beacon parsed;
      assert(laghu_runtime_parse_critical_css_beacon(
          (laghu_buffer){json, sizeof(json) - 1U}, &parsed));
      assert(parsed.viewport_bucket == 0U && parsed.rule_count == 2U &&
             parsed.rules[1] == 7U);
    }
  }
  {
    char observation_path[LAGHU_RUNTIME_PATH_SIZE];
    laghu_javascript_observation_set providers;
    laghu_runtime_html_result rum_page;
    laghu_instrumentation_beacon rum;
    char script_key[LAGHU_RUNTIME_KEY_SIZE];
    char json[2048U];
    const char *marker;
    FILE *configuration;
    assert(snprintf(observation_path, sizeof(observation_path), "%s/%s",
                    temporary, "javascript-observation.conf") > 0);
    configuration = fopen(observation_path, "wb");
    assert(configuration != NULL);
    assert(fputs("# exact observation rules\nhost cdn.example.test /assets/\n",
                 configuration) >= 0);
    assert(fclose(configuration) == 0);
    assert(laghu_javascript_observations_load(observation_path, &providers,
                                              NULL, 0U));
    assert(providers.count == 1U && providers.digest[0] != '\0');
    assert(laghu_runtime_add_instrumentation(
        rum_engine, temporary, &providers,
        (laghu_buffer){
            (const unsigned char
                 *)"<html><body><script src=\"/app.js\"></script>"
                   "<script src=\"https://cdn.example.test/assets/a.js\">"
                   "</script></body></html>",
            sizeof("<html><body><script src=\"/app.js\"></script>"
                   "<script src=\"https://cdn.example.test/assets/a.js\">"
                   "</script></body></html>") -
                1U},
        "/rum", "https://example.test", policy_key, 3000U, 604800U, 10U, true,
        &rum_page));
    assert(rum_page.rewritten);
    marker = strstr((const char *)rum_page.data, "data-laghu-template=\"");
    assert(marker != NULL);
    marker += sizeof("data-laghu-template=\"") - 1U;
    memset(&rum, 0, sizeof(rum));
    memcpy(rum.template_key, marker, LAGHU_SHA256_HEX_LENGTH);
    rum.template_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
    assert(laghu_sha256_hex(
        (laghu_buffer){(const unsigned char *)"https://example.test/app.js",
                       sizeof("https://example.test/app.js") - 1U},
        script_key));
    assert(snprintf(json, sizeof(json),
                    "{\"version\":1,\"template\":\"%s\",\"bucket\":1,"
                    "\"lcp_ms\":2200,\"inp_ms\":180,\"cls_milli\":80,"
                    "\"dcl_ms\":900,\"load_ms\":1200,\"errors\":0,"
                    "\"rejections\":0,\"candidates\":[{\"key\":\"%s\","
                    "\"before_dcl\":1,\"long_tasks\":0}]}",
                    rum.template_key, script_key) > 0);
    assert(laghu_runtime_parse_instrumentation_beacon(
        (laghu_buffer){(const unsigned char *)json, strlen(json)}, &rum));
    assert(rum.candidate_count == 1U && rum.lcp_ms == 2200U);
    assert(laghu_instrumentation_apply_beacon(rum_engine, temporary, 3001U,
                                              604800U, &rum));
    assert(strstr(laghu_runtime_instrumentation_script(),
                  "largest-contentful-paint") != NULL);
    laghu_runtime_html_result_release(&rum_page);
    configuration = fopen(observation_path, "wb");
    assert(configuration != NULL);
    assert(fputs("host 127.0.0.1 /assets/\n", configuration) >= 0);
    assert(fclose(configuration) == 0);
    assert(!laghu_javascript_observations_load(observation_path, &providers,
                                               NULL, 0U));
    configuration = fopen(observation_path, "wb");
    assert(configuration != NULL);
    assert(fputs("host cdn.example.test /assets/\n"
                 "host cdn.example.test /assets/js/\n",
                 configuration) >= 0);
    assert(fclose(configuration) == 0);
    assert(!laghu_javascript_observations_load(observation_path, &providers,
                                               NULL, 0U));
  }
  assert(!laghu_runtime_cache_lookup(temporary, index_key, NULL, &entry));
  assert(laghu_runtime_cache_lookup(temporary, index_key, "etag", &entry));
  assert(laghu_runtime_cache_lookup_variant(temporary, policy_key, &entry));
  assert(laghu_runtime_cache_read(&entry, cached, sizeof(cached)));
  assert(memcmp(cached, payload, sizeof(payload) - 1U) == 0);
  assert(!laghu_runtime_cache_lookup(temporary, index_key, "changed", &entry));
  {
    unsigned char corrupted = (unsigned char)(payload[0] ^ 0xffU);
    FILE *file = fopen(entry.variant_path, "r+b");
    assert(file != NULL);
    assert(fwrite(&corrupted, 1U, 1U, file) == 1U);
    assert(fclose(file) == 0);
  }
  assert(laghu_runtime_cache_lookup(temporary, index_key, "etag", &entry));
  assert(!laghu_runtime_cache_read(&entry, cached, sizeof(cached)));
  {
    FILE *file = fopen(entry.variant_path, "wb");
    assert(file != NULL);
    assert(fwrite(payload, 1U, 1U, file) == 1U);
    assert(fclose(file) == 0);
  }
  assert(!laghu_runtime_cache_lookup(temporary, index_key, "etag", &entry));
  {
    laghu_source_policy *source = malloc(sizeof(*source));
    laghu_source_policy *loaded = malloc(sizeof(*loaded));
    unsigned char *source_body = NULL;
    size_t source_length = 0U;
    char type[LAGHU_RUNTIME_TYPE_SIZE], validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
    char mapping[LAGHU_RUNTIME_KEY_SIZE], source_path[LAGHU_RUNTIME_PATH_SIZE];
    char registry_queue[LAGHU_RUNTIME_PATH_SIZE];
    FILE *file;
    assert(source != NULL && loaded != NULL);
    laghu_source_policy_init(source);
    assert(laghu_source_mode_parse("mapped", false, &source->mode));
    assert(!laghu_source_mode_parse("native", false, &source->mode));
    source->mode = LAGHU_SOURCE_FILE_MAPPED;
    assert(laghu_source_mapping_add(
        source, "https://origin.example.test/assets/", temporary));
    assert(!laghu_source_mapping_add(
        source, "https://origin.example.test/assets/", temporary));
    assert(laghu_source_policy_validate(source, false, NULL, 0U));
    assert(snprintf(source_path, sizeof(source_path), "%s/source.png",
                    temporary) > 0);
    file = fopen(source_path, "wb");
    assert(file != NULL);
    assert(fwrite("png-body", sizeof("png-body") - 1U, 1U, file) == 1U);
    assert(fclose(file) == 0);
    assert(snprintf(registry_queue, sizeof(registry_queue), "%s/source.queue",
                    temporary) > 0);
    assert(laghu_source_registry_publish(registry_queue, source));
    assert(laghu_source_registry_load(registry_queue, loaded));
    assert(laghu_source_file_load(
               loaded, "https://origin.example.test/assets/source.png",
               &source_body, &source_length, type, validator,
               mapping) == LAGHU_SOURCE_LOAD_READY);
    assert(source_length == sizeof("png-body") - 1U &&
           memcmp(source_body, "png-body", source_length) == 0 &&
           strcmp(type, "image/png") == 0 && validator[0] != '\0' &&
           mapping[0] != '\0');
    free(source_body);
    assert(laghu_source_file_load(
               loaded, "https://origin.example.test/assets/../source.png",
               &source_body, &source_length, type, validator,
               mapping) == LAGHU_SOURCE_LOAD_UNSAFE);
#ifndef _WIN32
    {
      char link_path[LAGHU_RUNTIME_PATH_SIZE];
      assert(snprintf(link_path, sizeof(link_path), "%s/link.png", temporary) >
             0);
      assert(symlink(source_path, link_path) == 0);
      assert(laghu_source_file_load(
                 loaded, "https://origin.example.test/assets/link.png",
                 &source_body, &source_length, type, validator,
                 mapping) == LAGHU_SOURCE_LOAD_UNSAFE);
    }
#endif
    free(loaded);
    free(source);
  }
  {
    struct sockaddr_in address4;
    struct sockaddr_in6 address6;
    memset(&address4, 0, sizeof(address4));
    address4.sin_family = AF_INET;
    assert(inet_pton(AF_INET, "10.1.2.3", &address4.sin_addr) == 1);
    assert(!laghu_source_address_public((struct sockaddr *)&address4));
    assert(inet_pton(AF_INET, "203.0.113.1", &address4.sin_addr) == 1);
    assert(!laghu_source_address_public((struct sockaddr *)&address4));
    assert(inet_pton(AF_INET, "8.8.8.8", &address4.sin_addr) == 1);
    assert(laghu_source_address_public((struct sockaddr *)&address4));
    memset(&address6, 0, sizeof(address6));
    address6.sin6_family = AF_INET6;
    assert(inet_pton(AF_INET6, "::ffff:127.0.0.1", &address6.sin6_addr) == 1);
    assert(!laghu_source_address_public((struct sockaddr *)&address6));
    assert(inet_pton(AF_INET6, "2001:db8::1", &address6.sin6_addr) == 1);
    assert(!laghu_source_address_public((struct sockaddr *)&address6));
    assert(inet_pton(AF_INET6, "2606:4700:4700::1111", &address6.sin6_addr) ==
           1);
    assert(laghu_source_address_public((struct sockaddr *)&address6));
  }
  laghu_runtime_queue_close(&consumer);
  laghu_runtime_queue_close(&producer);
  laghu_rum_engine_destroy(rum_engine);
  puts("laghu_runtime_test: all tests passed");
  return 0;
}
