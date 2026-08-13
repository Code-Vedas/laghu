// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/css.h"
#include "laghu/html.h"
#include "laghu/types.h"
#include "test_fixture.h"

static void assert_head_rewrite(const char *input, bool normalize, bool move, bool cross, const char *expected, bool added, bool combined,
                                bool moved) {
  laghu_runtime_head_result result;
  laghu_html_planner_mask plan = (normalize ? LAGHU_HTML_PLAN_ADD_COMBINE_HEAD : 0U) | (move ? LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD : 0U) |
                                 (cross ? LAGHU_HTML_PLAN_MOVE_CSS_ABOVE_SCRIPTS : 0U);
  assert(laghu_runtime_plan_html_document((laghu_buffer){(const unsigned char *)input, strlen(input)}, plan, &result));
  if (expected == NULL) {
    assert(!result.rewritten && result.data == NULL);
  } else {
    if (!result.rewritten || result.length != strlen(expected) ||
        memcmp(result.data, expected, result.length < strlen(expected) ? result.length : strlen(expected)) != 0) {
      fprintf(stderr, "head input: %s\nexpected: %s\nactual: %.*s\n", input, expected, (int)result.length,
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

static void assert_html_plan(const char *input, laghu_html_planner_mask plan, const char *expected) {
  laghu_runtime_head_result result;
  if (!laghu_runtime_plan_html_document((laghu_buffer){(const unsigned char *)input, strlen(input)}, plan, &result)) {
    fprintf(stderr, "HTML planner rejected: %s\n", input);
    assert(false);
  }
  if (expected == NULL) {
    assert(!result.rewritten && result.data == NULL);
  } else {
    if (!result.rewritten || result.length != strlen(expected) ||
        memcmp(result.data, expected, result.length < strlen(expected) ? result.length : strlen(expected)) != 0) {
      fprintf(stderr, "HTML input: %s\nexpected: %s\nactual: %.*s\n", input, expected, (int)result.length,
              result.data == NULL ? (const unsigned char *)"" : result.data);
    }
    assert(result.rewritten && result.lexical_changed);
    assert(result.length == strlen(expected));
    assert(memcmp(result.data, expected, result.length) == 0);
  }
  laghu_runtime_head_result_release(&result);
}

static void assert_html_plan_at(const char *input, const char *page_path, const char *page_origin, laghu_html_planner_mask plan,
                                const char *expected) {
  laghu_runtime_head_result result;
  assert(laghu_runtime_plan_html_document_at((laghu_buffer){(const unsigned char *)input, strlen(input)}, page_path, page_origin, plan, &result));
  if (expected == NULL) {
    if (result.rewritten) fprintf(stderr, "Unexpected HTML URL rewrite: %s\nactual: %.*s\n", input, (int)result.length, result.data);
    assert(!result.rewritten && result.data == NULL);
  } else {
    if (!result.rewritten || result.length != strlen(expected) ||
        memcmp(result.data, expected, result.length < strlen(expected) ? result.length : strlen(expected)) != 0) {
      fprintf(stderr, "HTML URL input: %s\nexpected: %s\nactual: %.*s\n", input, expected, (int)result.length,
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
  assert_html_plan_at("<audio src=/shop/shared/a.mp3></audio>", "/shop/deep/page.html", "https://example.com", trim,
                      "<audio src=../shared/a.mp3></audio>");
  assert_html_plan_at("<img src=/assets/hero.png><base href=/assets/>", "/shop/page.html", "https://example.com", trim,
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
  assert_html_plan_at("<script src=https://example.com:443/a.js></script>", "/page.html", "https://example.com", trim, "<script src=a.js></script>");
  assert_html_plan_at("<script src=https://example.com:8443/a.js></script>", "/page.html", "https://example.com:8443", trim,
                      "<script src=a.js></script>");
  assert_html_plan_at("<audio src=\"http://127.0.0.1:18081/asset.mp3?q=1#hero\"></audio>", "/minify-page.html", "http://127.0.0.1:18081", trim,
                      "<audio src=\"asset.mp3?q=1#hero\"></audio>");
  assert_html_plan_at("<img src=https://[2001:db8::1]:8443/a.png>", "/page.html", "https://[2001:db8::1]:8443", trim, "<img src=a.png>");
  assert_html_plan_at(
      "<template><img src=/shop/a.png></template>"
      "<img src=/shop//a.png>",
      "/shop/page.html", "https://example.com", trim, NULL);
  assert_html_plan_at("<img src=/shop/a.png>", "/shop/page.html", NULL, trim, NULL);
}

static void test_html_lexical_planner(void) {
  const laghu_html_planner_mask lexical = LAGHU_HTML_PLAN_LEXICAL;
  laghu_runtime_head_result result;

  assert_html_plan("<p>one \n\t two&nbsp;\xC2\xA0 three</p>", lexical, "<p>one two&nbsp;\xC2\xA0 three</p>");
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
  assert(!laghu_runtime_plan_html_document((laghu_buffer){(const unsigned char *)"<div id=x ID=y></div>", sizeof("<div id=x ID=y></div>") - 1U},
                                           lexical, &result));
  assert(!laghu_runtime_plan_html_document((laghu_buffer){(const unsigned char *)"<div title='x></div>", sizeof("<div title='x></div>") - 1U},
                                           lexical, &result));
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
  assert_head_rewrite("<html><head></head><body>x</body><head></head></html>", true, true, true, NULL, false, false, false);
  assert_head_rewrite("<!doctype html><html><body>x</body></html>", true, false, false, "<!doctype html><html><head></head><body>x</body></html>",
                      true, false, false);
  assert_head_rewrite("<!doctype html><body>x</body>", true, false, false, "<!doctype html><head></head><body>x</body>", true, false, false);
  assert_head_rewrite("<div>fragment</div>", true, true, true, NULL, false, false, false);
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
  assert(!laghu_runtime_plan_html_document((laghu_buffer){(const unsigned char *)"<html><head>", 12U},
                                           LAGHU_HTML_PLAN_ADD_COMBINE_HEAD | LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD, &result));
  assert(!laghu_runtime_plan_html_document(
      (laghu_buffer){(const unsigned char *)"<html><head><head></head></head></html>", sizeof("<html><head><head></head></head></html>") - 1U},
      LAGHU_HTML_PLAN_ADD_COMBINE_HEAD | LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD, &result));

  bounded = malloc((LAGHU_HTML_MAX_TOKENS + 1U) * 6U);
  assert(bounded != NULL);
  for (index = 0U; index <= LAGHU_HTML_MAX_TOKENS; ++index) {
    memcpy(bounded + offset, "<meta>", 6U);
    offset += 6U;
  }
  assert(!laghu_runtime_plan_html_document((laghu_buffer){bounded, offset}, LAGHU_HTML_PLAN_ADD_COMBINE_HEAD | LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD,
                                           &result));
  free(bounded);
}

static void test_head_planner_cache(const char *cache_path, const char *policy_key) {
  static const unsigned char html[] =
      "<html><head><script src=/app.js></script></head><body>"
      "<style media=all>.a{color:red}</style></body></html>";
  static const unsigned char changed[] =
      "<html><head><script src=/app.js></script></head><body>"
      "<style media=all>.a{color:blue}</style></body></html>";
  laghu_runtime_html_result result;
  char first_key[LAGHU_RUNTIME_KEY_SIZE];

  assert(laghu_runtime_rewrite_css_markup(
      cache_path, (laghu_buffer){html, sizeof(html) - 1U}, "/head.html", "https://example.test", policy_key, 0U, 42U, 604800U, false, false, false,
      LAGHU_HTML_PLAN_ADD_COMBINE_HEAD | LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD | LAGHU_HTML_PLAN_MOVE_CSS_ABOVE_SCRIPTS, true, true, 2048U, 8192U,
      &result));
  assert(!result.rewritten && result.dependencies_pending);
  strcpy(first_key, result.dependency_key);
  laghu_runtime_html_result_release(&result);
  assert(laghu_runtime_rewrite_css_markup(
      cache_path, (laghu_buffer){html, sizeof(html) - 1U}, "/head.html", "https://example.test", policy_key, 0U, 43U, 604800U, false, false, false,
      LAGHU_HTML_PLAN_ADD_COMBINE_HEAD | LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD | LAGHU_HTML_PLAN_MOVE_CSS_ABOVE_SCRIPTS, true, true, 2048U, 8192U,
      &result));
  assert(result.rewritten && !result.dependencies_pending);
  assert(result.length == sizeof(html) - 1U);
  assert(strstr((const char *)result.data, "<head><style media=all>") != NULL);
  assert(strstr((const char *)result.data, "</head><body></body>") != NULL);
  assert(strcmp(first_key, result.dependency_key) == 0);
  laghu_runtime_html_result_release(&result);
  assert(laghu_runtime_rewrite_css_markup(
      cache_path, (laghu_buffer){changed, sizeof(changed) - 1U}, "/head.html", "https://example.test", policy_key, 0U, 44U, 604800U, false, false,
      false, LAGHU_HTML_PLAN_ADD_COMBINE_HEAD | LAGHU_HTML_PLAN_MOVE_CSS_TO_HEAD | LAGHU_HTML_PLAN_MOVE_CSS_ABOVE_SCRIPTS, true, true, 2048U, 8192U,
      &result));
  assert(!result.rewritten && result.dependencies_pending);
  assert(strcmp(first_key, result.dependency_key) != 0);
  laghu_runtime_html_result_release(&result);
}

static void test_html_lexical_cache(const char *cache_path, const char *policy_key) {
  static const unsigned char html[] =
      "<html><body><!--drop--><p class=\"safe\">one   two</p>"
      "<script type=\"text/javascript\"> x  y </script>"
      "<pre> a  b </pre><!-- @license keep --></body></html>";
  laghu_runtime_html_result result;
  char dependency_key[LAGHU_RUNTIME_KEY_SIZE];

  assert(laghu_runtime_rewrite_css_markup(cache_path, (laghu_buffer){html, sizeof(html) - 1U}, "/minify.html", "https://example.test", policy_key, 0U,
                                          51U, 604800U, false, false, false, LAGHU_HTML_PLAN_LEXICAL, true, true, 2048U, 8192U, &result));
  assert(!result.rewritten && result.dependencies_pending);
  strcpy(dependency_key, result.dependency_key);
  laghu_runtime_html_result_release(&result);
  assert(laghu_runtime_rewrite_css_markup(cache_path, (laghu_buffer){html, sizeof(html) - 1U}, "/minify.html", "https://example.test", policy_key, 0U,
                                          52U, 604800U, false, false, false, LAGHU_HTML_PLAN_LEXICAL, true, true, 2048U, 8192U, &result));
  assert(result.rewritten && !result.dependencies_pending);
  assert(result.length < sizeof(html) - 1U);
  assert(strcmp(dependency_key, result.dependency_key) == 0);
  assert(strstr((const char *)result.data, "<!--drop-->") == NULL);
  assert(strstr((const char *)result.data, "<p class=safe>one two</p><script> x  y </script>") != NULL);
  assert(strstr((const char *)result.data, "<pre> a  b </pre><!-- @license keep -->") != NULL);
  laghu_runtime_html_result_release(&result);
}

int main(void) {
  static const unsigned char payload[] = "runtime payload";
  laghu_test_workspace workspace;
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];

  assert(laghu_test_workspace_create(&workspace));
  assert(laghu_sha256_hex((laghu_buffer){payload, sizeof(payload) - 1U}, policy_key));
  test_head_planner();
  test_html_lexical_planner();
  test_html_trim_urls();
  test_head_planner_cache(workspace.path, policy_key);
  test_html_lexical_cache(workspace.path, policy_key);
  assert(laghu_test_workspace_remove(&workspace));
  puts("laghu_runtime_html_integration_test: all tests passed");
  return 0;
}
