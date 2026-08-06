// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef _WIN32
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

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
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "laghu/assets.h"
#include "laghu/budget.h"
#include "laghu/cache.h"
#include "laghu/catalog.h"
#include "laghu/csp.h"
#include "laghu/css.h"
#include "laghu/fonts.h"
#include "laghu/html.h"
#include "laghu/instrumentation.h"
#include "laghu/javascript.h"
#include "laghu/lcp.h"
#include "laghu/operational.h"
#include "laghu/queue.h"
#include "laghu/rum.h"
#include "laghu/source.h"
#include "laghu/types.h"

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
  static const char template_key[] =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  laghu_rum_instrumentation_record record = {0};
  laghu_lcp_result result;
  char digest[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int bucket;
  laghu_csp_policy csp;
  laghu_csp_policy_init(&csp, "https://example.test");
  assert(laghu_lcp_inventory_record(
      (laghu_buffer){evidence_html, sizeof(evidence_html) - 1U}, "/index.html",
      "https://example.test", &record, digest));
  assert(record.media_count == 3U && record.media_kind[0] == 0U &&
         record.media_kind[1] == 1U && strlen(digest) == 64U);
  assert(laghu_runtime_prioritize_lcp(
      rum, (laghu_buffer){evidence_html, sizeof(evidence_html) - 1U},
      (laghu_buffer){selected_html, sizeof(selected_html) - 1U}, "/index.html",
      "https://example.test", NULL, 100U, 60U, 640U, true, true, &csp,
      &result));
  assert(result.decision == LAGHU_LCP_DECISION_HEURISTIC && result.applied &&
         result.rewritten);
  assert(strstr((const char *)result.data, "src=/hero.jpg") != NULL);
  assert(strstr((const char *)result.data, "fetchpriority=\"high\"") != NULL);
  assert(strstr((const char *)result.data,
                "src=/hero.jpg width=800 height=600 loading=lazy") == NULL);
  assert(strcmp(result.link_header, "</hero.jpg>; rel=preload; as=image") == 0);
  laghu_lcp_result_release(&result);
  assert(laghu_runtime_prioritize_lcp(
      rum, (laghu_buffer){evidence_html, sizeof(evidence_html) - 1U},
      (laghu_buffer){responsive_html, sizeof(responsive_html) - 1U},
      "/index.html", "https://example.test", NULL, 100U, 60U, 640U, true, true,
      &csp, &result));
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
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION,
                                  template_key, 100U, &record, sizeof(record),
                                  NULL));
  assert(laghu_runtime_prioritize_lcp(
      rum, (laghu_buffer){evidence_html, sizeof(evidence_html) - 1U},
      (laghu_buffer){selected_html, sizeof(selected_html) - 1U}, "/index.html",
      "https://example.test", template_key, 101U, 60U, 640U, true, true, &csp,
      &result));
  assert(result.decision == LAGHU_LCP_DECISION_LEARNED && result.applied &&
         result.observations == 6U && result.profile_ready[0] &&
         result.profile_ready[1]);
  laghu_lcp_result_release(&result);
  record.lcp_candidates[1][1] = 0U;
  record.lcp_candidates[1][2] = 3U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION,
                                  template_key, 102U, &record, sizeof(record),
                                  NULL));
  assert(laghu_runtime_prioritize_lcp(
      rum, (laghu_buffer){evidence_html, sizeof(evidence_html) - 1U},
      (laghu_buffer){selected_html, sizeof(selected_html) - 1U}, "/index.html",
      "https://example.test", template_key, 103U, 60U, 640U, true, true, &csp,
      &result));
  assert(result.decision == LAGHU_LCP_DECISION_HEURISTIC);
  laghu_lcp_result_release(&result);

  {
    static const unsigned char picture_html[] =
        "<html><body><picture><source srcset=\"/hero-1.webp 1x, "
        "/hero-2.webp 2x\" sizes=\"100vw\"><img src=/hero.jpg "
        "width=800 height=600 loading=lazy></picture></body></html>";
    static const char picture_key[] =
        "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    memset(&record, 0, sizeof(record));
    assert(laghu_lcp_inventory_record(
        (laghu_buffer){picture_html, sizeof(picture_html) - 1U}, "/picture",
        "https://example.test", &record, digest));
    assert(record.media_count == 1U && record.media_resource_count[0] == 3U);
    record.version = LAGHU_INSTRUMENTATION_VERSION;
    strcpy(record.template_key, picture_key);
    record.updated_at = 200U;
    for (bucket = 0U; bucket < 2U; ++bucket) {
      record.lcp_observations[bucket] = 3U;
      record.lcp_candidates[bucket][0] = 3U;
      record.lcp_resources[bucket][0][1] = 3U;
    }
    assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION,
                                    picture_key, 200U, &record, sizeof(record),
                                    NULL));
    assert(laghu_runtime_prioritize_lcp(
        rum, (laghu_buffer){picture_html, sizeof(picture_html) - 1U},
        (laghu_buffer){picture_html, sizeof(picture_html) - 1U}, "/picture",
        "https://example.test", picture_key, 201U, 60U, 640U, true, true, &csp,
        &result));
    assert(result.decision == LAGHU_LCP_DECISION_LEARNED && result.applied);
    assert(strstr(result.link_header, "</hero-2.webp>") != NULL &&
           strstr(result.link_header,
                  "imagesrcset=\"/hero-1.webp 1x, "
                  "/hero-2.webp 2x\"") != NULL);
    assert(strstr((const char *)result.data, "fetchpriority=\"high\"") != NULL);
    laghu_lcp_result_release(&result);
  }

  {
    static const unsigned char video_html[] =
        "<html><body><video poster=/poster.jpg></video></body></html>";
    static const char video_key[] =
        "2123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    memset(&record, 0, sizeof(record));
    assert(laghu_lcp_inventory_record(
        (laghu_buffer){video_html, sizeof(video_html) - 1U}, "/video",
        "https://example.test", &record, digest));
    record.version = LAGHU_INSTRUMENTATION_VERSION;
    strcpy(record.template_key, video_key);
    record.updated_at = 300U;
    for (bucket = 0U; bucket < 2U; ++bucket) {
      record.lcp_observations[bucket] = 3U;
      record.lcp_candidates[bucket][0] = 3U;
      record.lcp_resources[bucket][0][0] = 3U;
    }
    assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION,
                                    video_key, 300U, &record, sizeof(record),
                                    NULL));
    assert(laghu_runtime_prioritize_lcp(
        rum, (laghu_buffer){video_html, sizeof(video_html) - 1U},
        (laghu_buffer){video_html, sizeof(video_html) - 1U}, "/video",
        "https://example.test", video_key, 301U, 60U, 640U, true, true, &csp,
        &result));
    assert(result.decision == LAGHU_LCP_DECISION_LEARNED && result.applied &&
           !result.rewritten &&
           strcmp(result.link_header, "</poster.jpg>; rel=preload; as=image") ==
               0);
    laghu_lcp_result_release(&result);
  }

  laghu_csp_policy_init(&csp, "https://example.test");
  assert(laghu_csp_policy_add(&csp, "img-src 'none'",
                              sizeof("img-src 'none'") - 1U));
  assert(laghu_runtime_prioritize_lcp(
      rum, (laghu_buffer){evidence_html, sizeof(evidence_html) - 1U},
      (laghu_buffer){selected_html, sizeof(selected_html) - 1U}, "/index.html",
      "https://example.test", NULL, 400U, 60U, 640U, true, true, &csp,
      &result));
  assert(result.decision == LAGHU_LCP_DECISION_CONFLICT && !result.applied &&
         result.link_header == NULL);
  laghu_lcp_result_release(&result);
}

int main(void) {
  laghu_rum_options options;
  laghu_rum_engine *rum;
  laghu_rum_options_init(&options);
  rum = laghu_rum_engine_create(&options, NULL, 0U);
  assert(rum != NULL);
  test_lcp_prioritization(rum);
  laghu_rum_engine_destroy(rum);
  puts("laghu_runtime_lcp_integration_test: all tests passed");
  return 0;
}
