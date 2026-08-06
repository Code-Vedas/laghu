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

typedef struct {
  laghu_operational_registry *registry;
  unsigned int iterations;
} operational_thread_context;

#ifdef _WIN32
static DWORD WINAPI operational_thread(void *data) {
#else
static void *operational_thread(void *data) {
#endif
  operational_thread_context *context = data;
  unsigned int index;
  for (index = 0U; index < context->iterations; ++index)
    laghu_operational_registry_record(context->registry,
                                      LAGHU_OPERATIONAL_DECISION_ORIGINAL, 10U,
                                      10U, 1000U);
#ifdef _WIN32
  return 0U;
#else
  return NULL;
#endif
}

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

static void test_operational_registry(const char *cache_path) {
  laghu_operational_registry adapter, worker;
  laghu_operational_snapshot *snapshot = calloc(1U, sizeof(*snapshot));
  laghu_operational_readiness readiness;
  laghu_cache_stats cache = {0};
  laghu_transform_budget budget;
  char *output = calloc(LAGHU_OPERATIONAL_RENDER_SIZE, 1U);
  size_t length = 0U;
  uint64_t now = (uint64_t)time(NULL);
  operational_thread_context thread_context;
  unsigned int thread_index;
#ifdef _WIN32
  HANDLE threads[4];
#else
  pthread_t threads[4];
#endif
  assert(snapshot != NULL && output != NULL);
  laghu_operational_registry_init(&adapter);
  laghu_operational_registry_init(&worker);
  assert(laghu_operational_registry_open(
      &adapter, cache_path, LAGHU_OPERATIONAL_SURFACE_STANDALONE,
      LAGHU_OPERATIONAL_PROCESS_ADAPTER, true, now));
  assert(laghu_operational_registry_open(
      &worker, cache_path, LAGHU_OPERATIONAL_SURFACE_WORKER,
      LAGHU_OPERATIONAL_PROCESS_LIBVIPS, true, now));
  thread_context.registry = &adapter;
  thread_context.iterations = 10000U;
  for (thread_index = 0U; thread_index < 4U; ++thread_index) {
#ifdef _WIN32
    threads[thread_index] =
        CreateThread(NULL, 0U, operational_thread, &thread_context, 0U, NULL);
    assert(threads[thread_index] != NULL);
#else
    assert(pthread_create(&threads[thread_index], NULL, operational_thread,
                          &thread_context) == 0);
#endif
  }
  for (thread_index = 0U; thread_index < 4U; ++thread_index) {
#ifdef _WIN32
    assert(WaitForSingleObject(threads[thread_index], INFINITE) ==
           WAIT_OBJECT_0);
    CloseHandle(threads[thread_index]);
#else
    assert(pthread_join(threads[thread_index], NULL) == 0);
#endif
  }
  laghu_operational_registry_record(
      &adapter, LAGHU_OPERATIONAL_DECISION_OPTIMIZED, 1000U, 600U, 25000U);
  laghu_operational_registry_record(&adapter, LAGHU_OPERATIONAL_DECISION_BYPASS,
                                    200U, 200U, 5000001U);
  laghu_operational_registry_failure(&adapter,
                                     LAGHU_OPERATIONAL_FAILURE_TRANSFORM);
  cache.hits = 4U;
  cache.misses = 2U;
  cache.publications = 3U;
  cache.evictions = 1U;
  cache.bytes = 4096U;
  cache.files = 7U;
  cache.rejected_publications = 2U;
  cache.variant_occupancy = 3U;
  laghu_operational_registry_cache(&adapter, &cache);
  laghu_transform_budget_init(&budget, 32U * 1024U * 1024U, 50U, 16U);
  assert(laghu_transform_budget_reserve(&budget, 4096U));
  budget.rejection = LAGHU_BUDGET_REJECTION_DEADLINE;
  laghu_operational_registry_budget(&adapter, &budget, 50U);
  {
    unsigned int observations[4] = {3U, 4U, 0U, 0U};
    bool ready[4] = {true, true, false, false};
    laghu_operational_registry_lcp(&adapter, LAGHU_LCP_DECISION_LEARNED, true,
                                   observations, ready);
  }
  assert(laghu_operational_registry_heartbeat(&worker, now, true, 64U, 3U));
  laghu_operational_registry_worker_job(&worker, false, 1000U,
                                        LAGHU_OPERATIONAL_FAILURE_WORKER);
  assert(laghu_operational_registry_snapshot(&adapter, snapshot));
  {
    unsigned int slot;
    bool found = false;
    for (slot = 0U; slot < snapshot->slot_count; ++slot) {
      if (snapshot->slots[slot].active != 0U &&
          snapshot->slots[slot].surface ==
              LAGHU_OPERATIONAL_SURFACE_STANDALONE) {
        assert(snapshot->slots[slot].requests == 40002U);
        found = true;
        break;
      }
    }
    assert(found);
  }
  assert(laghu_operational_render_prometheus(
      snapshot, now, output, LAGHU_OPERATIONAL_RENDER_SIZE, &length));
  assert(length != 0U && strstr(output, "laghu_requests_total") != NULL);
  assert(strstr(output, "laghu_cache_bytes 4096") != NULL);
  assert(strstr(output, "laghu_cache_rejected_writes_total 2") != NULL);
  assert(strstr(output, "laghu_failures_total{subsystem=\"worker\"} 1") !=
         NULL);
  assert(strstr(output, "laghu_variant_occupancy 3") != NULL);
  assert(strstr(output, "laghu_lcp_decisions_total{decision=\"learned\"} 1") !=
         NULL);
  assert(strstr(output,
                "laghu_lcp_profile_ready{viewport=\"mobile\",theme=\"dark\"} "
                "1") != NULL);
  assert(strstr(output,
                "laghu_transform_rejections_total{reason=\"deadline\"} 1") !=
         NULL);
  assert(strstr(output, "laghu_response_bytes_total{kind=\"saved\"} 400") !=
         NULL);
  assert(strstr(output, cache_path) == NULL);
  assert(laghu_operational_readiness_evaluate(snapshot, now, true, true, false,
                                              &readiness));
  assert(readiness.workers_ready && !readiness.degraded &&
         readiness.configured_workers == 1U && readiness.healthy_workers == 1U);
  assert(laghu_operational_registry_heartbeat(&worker, now, false, 64U, 4U));
  assert(laghu_operational_registry_snapshot(&adapter, snapshot));
  assert(laghu_operational_readiness_evaluate(snapshot, now, true, true, false,
                                              &readiness));
  assert(readiness.workers_ready && readiness.degraded);
  assert(laghu_operational_readiness_evaluate(snapshot, now, true, true, true,
                                              &readiness));
  assert(!readiness.workers_ready && readiness.degraded);
  assert(laghu_operational_render_readiness(
      &readiness, true, output, LAGHU_OPERATIONAL_RENDER_SIZE, &length));
  assert(strstr(output, "\"status\":\"not_ready\"") != NULL);
  assert(strstr(output, "\"budgets\":\"ready\"") != NULL);
  laghu_operational_registry_close(&worker);
  laghu_operational_registry_close(&adapter);
  free(output);
  free(snapshot);
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

int main(void) {
  char temporary[LAGHU_RUNTIME_PATH_SIZE];
#ifdef _WIN32
  char base[LAGHU_RUNTIME_PATH_SIZE];
  assert(GetTempPathA(sizeof(base), base) > 0U);
  assert(GetTempFileNameA(base, "lgo", 0U, temporary) != 0U);
  assert(DeleteFileA(temporary));
  assert(_mkdir(temporary) == 0);
#else
  strcpy(temporary, "/tmp/laghu-operational-XXXXXX");
  assert(mkdtemp(temporary) != NULL);
#endif
  test_operational_registry(temporary);
  puts("laghu_runtime_operational_integration_test: all tests passed");
  return 0;
}
