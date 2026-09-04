// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "laghu/profile.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void laghu_profile_test_fail(const char *expression, unsigned int line) {
  (void)fprintf(stderr, "profile check failed at line %u: %s\n", line, expression);
  abort();
}

#undef assert
#define assert(expression) ((expression) ? (void)0 : laghu_profile_test_fail(#expression, __LINE__))

static bool issue_receipt(laghu_rum_engine *rum, const char *template_key, const char *snapshot_key, const char *receipt, uint64_t now,
                          unsigned int ttl_seconds) {
  return laghu_runtime_issue_chrome_analysis_receipt(rum, template_key, snapshot_key, receipt, now, ttl_seconds, 1500U);
}

int main(void) {
  static const char key[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  laghu_rum_options options;
  laghu_rum_engine *rum;
  laghu_rum_instrumentation_record record = {0};
  laghu_template_profile profile;

  laghu_rum_options_init(&options);
  options.store_uri = "memory:";
  options.ttl_seconds = 600U;
  rum = laghu_rum_engine_create(&options, NULL, 0U);
  assert(rum != NULL);
  assert(laghu_template_profile_decide(rum, key, 100U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_NONE && !profile.apply);

  record.version = LAGHU_INSTRUMENTATION_VERSION;
  strcpy(record.template_key, key);
  record.updated_at = 100U;
  record.observations[1] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES - 1U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 100U, &record, sizeof(record), NULL));
  assert(laghu_template_profile_decide(rum, key, 101U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_NONE && !profile.apply);

  record.observations[1] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES;
  record.histograms[1][0][2] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES;
  record.histograms[1][1][2] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES;
  record.histograms[1][2][2] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES;
  record.updated_at = 102U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 102U, &record, sizeof(record), NULL));
  assert(laghu_template_profile_decide(rum, key, 103U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_LEARNED && profile.apply && profile.lcp_over_budget && profile.inp_over_budget &&
         profile.cls_over_budget && profile.observations == LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES);

  memset(record.histograms, 0, sizeof(record.histograms));
  record.histograms[1][0][1] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES;
  record.histograms[1][1][1] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES;
  record.histograms[1][2][1] = LAGHU_TEMPLATE_PROFILE_MIN_SAMPLES;
  record.updated_at = 104U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 104U, &record, sizeof(record), NULL));
  assert(laghu_template_profile_decide(rum, key, 105U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_LEARNED && !profile.apply && !profile.lcp_over_budget && !profile.inp_over_budget &&
         !profile.cls_over_budget);

  record.errors[1] = 1U;
  record.updated_at = 106U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 106U, &record, sizeof(record), NULL));
  assert(laghu_template_profile_decide(rum, key, 107U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_REGRESSION && !profile.apply);

  record.errors[1] = 0U;
  record.updated_at = 108U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 108U, &record, sizeof(record), NULL));
  assert(laghu_template_profile_decide(rum, key, 169U, 60U, 1U, &profile));
  assert(profile.decision == LAGHU_TEMPLATE_PROFILE_STALE && !profile.apply);

  /* Chrome contributes only a receipt-bound LCP candidate; it must not
   * fabricate an observation or accept a report that page HTML could preseed. */
  memset(&record, 0, sizeof(record));
  record.version = LAGHU_INSTRUMENTATION_VERSION;
  strcpy(record.template_key, key);
  record.updated_at = 200U;
  record.media_count = 1U;
  assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 200U, &record, sizeof(record), NULL));
  {
    static const char receipt[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const char second_receipt[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    static const char expired_receipt[] = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    static const char snapshot[] = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    static const char wrong_snapshot[] = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    static const char other_template[] = "9999999999999999999999999999999999999999999999999999999999999999";
    char report[512U];
    int length;
    assert(issue_receipt(rum, key, snapshot, receipt, 201U, 60U));
    assert(laghu_rum_engine_read(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 201U, &record, sizeof(record), NULL));
    assert(record.updated_at == 200U && strcmp(record.chrome_analysis_receipts[0].value, receipt) == 0 &&
           strcmp(record.chrome_analysis_receipts[0].snapshot_key, snapshot) == 0 && !record.chrome_analysis_receipts[0].consumed);
    length = snprintf(report, sizeof(report),
                      "{\"version\":1,\"width\":1440,\"lcp_ordinal\":0,\"network_requests_blocked\":0,\"template\":\"%s\","
                      "\"receipt\":\"%s\",\"snapshot\":\"%s\"}",
                      key, receipt, snapshot);
    assert(length > 0 && (size_t)length < sizeof(report));
    assert(laghu_runtime_apply_chrome_analysis(rum, (laghu_buffer){(const unsigned char *)report, (size_t)length}, 201U, 60U));
    /* Replay, duplicate fields, non-canonical text, an observed victim
     * receipt with another snapshot, and malformed reports all fail closed. */
    assert(!laghu_runtime_apply_chrome_analysis(rum, (laghu_buffer){(const unsigned char *)report, (size_t)length}, 202U, 60U));
    assert(!laghu_runtime_apply_chrome_analysis(
        rum,
        (laghu_buffer){(const unsigned char *)"{\"version\":1,\"width\":1440,\"lcp_ordinal\":0,\"network_requests_blocked\":0,"
                                              "\"template\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
                                              "\"template\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}",
                       sizeof("{\"version\":1,\"width\":1440,\"lcp_ordinal\":0,\"network_requests_blocked\":0,"
                              "\"template\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
                              "\"template\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}") -
                           1U},
        202U, 60U));
    assert(!laghu_runtime_apply_chrome_analysis(rum, (laghu_buffer){(const unsigned char *)"<!-- <script>report</script> -->", 31U}, 202U, 60U));
    assert(issue_receipt(rum, key, snapshot, second_receipt, 202U, 60U));
    {
      laghu_rum_instrumentation_record other = {0};
      other.version = LAGHU_INSTRUMENTATION_VERSION;
      strcpy(other.template_key, other_template);
      other.updated_at = 202U;
      other.media_count = 1U;
      assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, other_template, 202U, &other, sizeof(other), NULL));
      length = snprintf(report, sizeof(report),
                        "{\"version\":1,\"width\":1440,\"lcp_ordinal\":0,\"network_requests_blocked\":0,\"template\":\"%s\","
                        "\"receipt\":\"%s\",\"snapshot\":\"%s\"}",
                        other_template, second_receipt, snapshot);
      assert(length > 0 && (size_t)length < sizeof(report));
      assert(!laghu_runtime_apply_chrome_analysis(rum, (laghu_buffer){(const unsigned char *)report, (size_t)length}, 202U, 60U));
    }
    length = snprintf(report, sizeof(report),
                      "{\"version\":1,\"width\":1440,\"lcp_ordinal\":0,\"network_requests_blocked\":0,\"template\":\"%s\","
                      "\"receipt\":\"%s\",\"snapshot\":\"%s\"}",
                      key, second_receipt, wrong_snapshot);
    assert(length > 0 && (size_t)length < sizeof(report));
    assert(!laghu_runtime_apply_chrome_analysis(rum, (laghu_buffer){(const unsigned char *)report, (size_t)length}, 202U, 60U));
    length = snprintf(report, sizeof(report),
                      "{\"version\":1,\"width\":1440,\"lcp_ordinal\":0,\"network_requests_blocked\":0,\"template\":\"%s\","
                      "\"receipt\":\"%s\",\"snapshot\":\"%s\"}",
                      key, second_receipt, snapshot);
    assert(length > 0 && (size_t)length < sizeof(report));
    assert(laghu_runtime_apply_chrome_analysis(rum, (laghu_buffer){(const unsigned char *)report, (size_t)length}, 202U, 60U));
    assert(issue_receipt(rum, key, snapshot, expired_receipt, 202U, 20U));
    length = snprintf(report, sizeof(report),
                      "{\"version\":1,\"width\":1440,\"lcp_ordinal\":0,\"network_requests_blocked\":0,\"template\":\"%s\","
                      "\"receipt\":\"%s\",\"snapshot\":\"%s\"}",
                      key, expired_receipt, snapshot);
    assert(length > 0 && (size_t)length < sizeof(report));
    assert(!laghu_runtime_apply_chrome_analysis(rum, (laghu_buffer){(const unsigned char *)report, (size_t)length}, 223U, 60U));
  }
  assert(laghu_rum_engine_read(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 204U, &record, sizeof(record), NULL));
  assert(record.lcp_observations[2] == 2U && record.lcp_candidates[2][0] == 2U && record.observations[1] == 0U);

  /* Receipt lifetime never extends its source template. A late optional job
   * must leave enough time for Chrome, two periodic RUM sync legs, and the
   * importer lifecycle. This engine uses the default five-second cadence. */
  {
    static const char boundary_template[] = "6666666666666666666666666666666666666666666666666666666666666666";
    static const char boundary_snapshot[] = "5555555555555555555555555555555555555555555555555555555555555555";
    static const char boundary_receipt[] = "4444444444444444444444444444444444444444444444444444444444444444";
    static const char late_receipt[] = "3333333333333333333333333333333333333333333333333333333333333333";
    laghu_rum_instrumentation_record boundary = {0};
    boundary.version = LAGHU_INSTRUMENTATION_VERSION;
    strcpy(boundary.template_key, boundary_template);
    boundary.updated_at = 400U;
    boundary.media_count = 1U;
    assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, boundary_template, 400U, &boundary, sizeof(boundary), NULL));
    assert(issue_receipt(rum, boundary_template, boundary_snapshot, boundary_receipt, 407U, 20U));
    assert(laghu_rum_engine_read(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, boundary_template, 407U, &boundary, sizeof(boundary), NULL));
    assert(boundary.updated_at == 400U && boundary.chrome_analysis_receipts[0].expires_at == 420U);
    assert(!issue_receipt(rum, boundary_template, boundary_snapshot, late_receipt, 408U, 20U));
  }

  /* The configured RUM store can expire before image metadata. Issuance must
   * use that smaller lifetime rather than claiming a report can replicate. */
  {
    static const char short_template[] = "1212121212121212121212121212121212121212121212121212121212121212";
    static const char short_snapshot[] = "3434343434343434343434343434343434343434343434343434343434343434";
    static const char short_receipt[] = "5656565656565656565656565656565656565656565656565656565656565656";
    static const char too_late_receipt[] = "7878787878787878787878787878787878787878787878787878787878787878";
    laghu_rum_options short_options;
    laghu_rum_engine *short_rum;
    laghu_rum_instrumentation_record short_record = {0};
    laghu_rum_options_init(&short_options);
    short_options.store_uri = "memory:";
    short_options.ttl_seconds = 6U;
    short_options.sync_interval_seconds = 1U;
    short_rum = laghu_rum_engine_create(&short_options, NULL, 0U);
    assert(short_rum != NULL);
    short_record.version = LAGHU_INSTRUMENTATION_VERSION;
    strcpy(short_record.template_key, short_template);
    short_record.updated_at = 700U;
    short_record.media_count = 1U;
    assert(laghu_rum_engine_publish(short_rum, LAGHU_RUM_RECORD_INSTRUMENTATION, short_template, 700U, &short_record, sizeof(short_record), NULL));
    assert(issue_receipt(short_rum, short_template, short_snapshot, short_receipt, 700U, 60U));
    assert(laghu_rum_engine_read(short_rum, LAGHU_RUM_RECORD_INSTRUMENTATION, short_template, 700U, &short_record, sizeof(short_record), NULL));
    assert(short_record.chrome_analysis_receipts[0].expires_at == 706U);
    assert(!issue_receipt(short_rum, short_template, short_snapshot, too_late_receipt, 702U, 60U));
    laghu_rum_engine_destroy(short_rum);
  }

  /* The ledger is deliberately bounded: eight concurrent optional jobs are
   * tracked, a ninth is refused without changing RUM, and a failed publish
   * can release its slot. */
  {
    static const char limit_template[] = "8888888888888888888888888888888888888888888888888888888888888888";
    static const char snapshot[] = "7777777777777777777777777777777777777777777777777777777777777777";
    laghu_rum_instrumentation_record limit = {0};
    char receipt[LAGHU_RUNTIME_KEY_SIZE];
    unsigned int index;
    limit.version = LAGHU_INSTRUMENTATION_VERSION;
    strcpy(limit.template_key, limit_template);
    limit.updated_at = 300U;
    limit.media_count = 1U;
    assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, limit_template, 300U, &limit, sizeof(limit), NULL));
    for (index = 0U; index < LAGHU_CHROME_ANALYSIS_MAX_RECEIPTS; ++index) {
      memset(receipt, (int)('0' + index), LAGHU_SHA256_HEX_LENGTH);
      receipt[LAGHU_SHA256_HEX_LENGTH] = '\0';
      assert(issue_receipt(rum, limit_template, snapshot, receipt, 300U, 60U));
    }
    memset(receipt, 'f', LAGHU_SHA256_HEX_LENGTH);
    receipt[LAGHU_SHA256_HEX_LENGTH] = '\0';
    assert(!issue_receipt(rum, limit_template, snapshot, receipt, 300U, 60U));
    memset(receipt, '0', LAGHU_SHA256_HEX_LENGTH);
    receipt[LAGHU_SHA256_HEX_LENGTH] = '\0';
    assert(laghu_runtime_revoke_chrome_analysis_receipt(rum, limit_template, receipt, 300U));
    memset(receipt, 'f', LAGHU_SHA256_HEX_LENGTH);
    receipt[LAGHU_SHA256_HEX_LENGTH] = '\0';
    assert(issue_receipt(rum, limit_template, snapshot, receipt, 300U, 60U));
  }

  {
    char directory[] = "/tmp/laghu-chrome-import.XXXXXX";
    char path[sizeof(directory) + LAGHU_RUNTIME_KEY_SIZE * 2U + 8U];
    FILE *report;
    static const char import_receipt[] = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    static const char import_snapshot[] = "1111111111111111111111111111111111111111111111111111111111111111";
    record.updated_at = 202U;
    assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 202U, &record, sizeof(record), NULL));
    assert(issue_receipt(rum, key, import_snapshot, import_receipt, 203U, 60U));
    assert(mkdtemp(directory) != NULL);
    assert(snprintf(path, sizeof(path), "%s/%s-%s.json", directory, import_snapshot, import_receipt) > 0);
    report = fopen(path, "wb");
    assert(report != NULL);
    assert(fputs("{\"version\":1,\"width\":1440,\"lcp_ordinal\":0,\"network_requests_blocked\":0,\"template\":\""
                 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"receipt\":\""
                 "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\",\"snapshot\":\""
                 "1111111111111111111111111111111111111111111111111111111111111111\"}",
                 report) >= 0);
    assert(fclose(report) == 0);
    assert(laghu_runtime_import_chrome_analysis(rum, directory, 203U, 60U) == 1U);
    assert(access(path, F_OK) != 0);

    /* A canonical report can arrive at another process before its issuer's
     * asynchronous RUM replication. It must wait, then consume exactly once. */
    {
      static const char pending_receipt[] = "2222222222222222222222222222222222222222222222222222222222222222";
      static const char pending_snapshot[] = "3333333333333333333333333333333333333333333333333333333333333333";
      memset(&record, 0, sizeof(record));
      record.version = LAGHU_INSTRUMENTATION_VERSION;
      strcpy(record.template_key, key);
      record.updated_at = 500U;
      record.media_count = 1U;
      assert(laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 500U, &record, sizeof(record), NULL));
      assert(snprintf(path, sizeof(path), "%s/%s-%s.json", directory, pending_snapshot, pending_receipt) > 0);
      report = fopen(path, "wb");
      assert(report != NULL);
      assert(fprintf(report,
                     "{\"version\":1,\"width\":1440,\"lcp_ordinal\":0,\"network_requests_blocked\":0,\"template\":\"%s\","
                     "\"receipt\":\"%s\",\"snapshot\":\"%s\"}",
                     key, pending_receipt, pending_snapshot) > 0 &&
             fclose(report) == 0);
      assert(laghu_runtime_import_chrome_analysis(rum, directory, 501U, 60U) == 0U);
      assert(access(path, F_OK) == 0);
      assert(issue_receipt(rum, key, pending_snapshot, pending_receipt, 501U, 60U));
      assert(laghu_runtime_import_chrome_analysis(rum, directory, 501U, 60U) == 1U);
      assert(access(path, F_OK) != 0);
    }

    assert(snprintf(path, sizeof(path), "%s/malformed.json", directory) > 0);
    report = fopen(path, "wb");
    assert(report != NULL && fputs("<!-- forged report -->", report) >= 0 && fclose(report) == 0);
    assert(laghu_runtime_import_chrome_analysis(rum, directory, 502U, 60U) == 0U);
    assert(access(path, F_OK) != 0);
    assert(snprintf(path, sizeof(path), "%s/.laghu-chrome-analysis.lock", directory) > 0);
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
  }
  laghu_rum_engine_destroy(rum);
  return 0;
}
