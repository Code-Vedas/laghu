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

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/rum_internal.h"
#include "laghu/base.h"
#include "laghu/catalog.h"
#include "laghu/css.h"
#include "laghu/html.h"
#include "laghu/instrumentation.h"
#include "laghu/rum.h"
#include "test_fixture.h"

static void laghu_rum_integration_test_fail(const char *expression, unsigned int line) {
  (void)fprintf(stderr, "rum integration check failed at line %u: %s\n", line, expression);
  abort();
}

#undef assert
#define assert(expression) ((expression) ? (void)0 : laghu_rum_integration_test_fail(#expression, __LINE__))

static bool receipt_seen(const laghu_rum_instrumentation_record *record, const char *value, bool consumed) {
  size_t index;
  for (index = 0U; index < LAGHU_CHROME_ANALYSIS_MAX_RECEIPTS; ++index)
    if (strcmp(record->chrome_analysis_receipts[index].value, value) == 0)
      return record->chrome_analysis_receipts[index].consumed == (unsigned char)(consumed ? 1U : 0U);
  return false;
}

static void test_rum_codec_receipts(void) {
  static const char key[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  static const char receipt[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  laghu_rum_instrumentation_record record = {0}, decoded = {0}, merged = {0}, delta = {0};
  laghu_rum_image_record image = {0};
  laghu_critical_css_record css = {0};
  unsigned char encoded[LAGHU_RUM_MAX_RECORD_BYTES];
  size_t encoded_length, legacy_length;
  record.version = LAGHU_INSTRUMENTATION_VERSION;
  strcpy(record.template_key, key);
  strcpy(record.provider_digest, key);
  strcpy(record.policy_key, key);
  record.updated_at = 7U;
  strcpy(record.chrome_analysis_receipts[0].value, receipt);
  strcpy(record.chrome_analysis_receipts[0].snapshot_key, key);
  record.chrome_analysis_receipts[0].expires_at = 99U;
  assert(laghu_rum_encode(LAGHU_RUM_RECORD_INSTRUMENTATION, &record, sizeof(record), encoded, sizeof(encoded), &encoded_length));
  legacy_length = encoded_length - LAGHU_CHROME_ANALYSIS_MAX_RECEIPTS * (sizeof(record.chrome_analysis_receipts[0].value) +
                                                                         sizeof(record.chrome_analysis_receipts[0].snapshot_key) + 8U + 1U);
  /* Existing v2 on-disk/Redis values end exactly before the appended ledger. */
  assert(legacy_length == 14479U && encoded_length == 15591U);
  assert(laghu_rum_decode(LAGHU_RUM_RECORD_INSTRUMENTATION, encoded, legacy_length, &decoded, sizeof(decoded)));
  assert(strcmp(decoded.template_key, key) == 0 && decoded.chrome_analysis_receipts[0].value[0] == '\0');
  assert(laghu_rum_decode(LAGHU_RUM_RECORD_INSTRUMENTATION, encoded, encoded_length, &decoded, sizeof(decoded)));
  assert(strcmp(decoded.chrome_analysis_receipts[0].value, receipt) == 0 && decoded.chrome_analysis_receipts[0].expires_at == 99U);
  merged = record;
  delta = record;
  delta.chrome_analysis_receipts[0].consumed = 1U;
  assert(laghu_rum_record_merge(LAGHU_RUM_RECORD_INSTRUMENTATION, &merged, &delta, sizeof(merged)));
  assert(merged.chrome_analysis_receipts[0].consumed == 1U);
  strcpy(image.identity, key);
  assert(laghu_rum_encode(LAGHU_RUM_RECORD_IMAGE, &image, sizeof(image), encoded, sizeof(encoded), &encoded_length));
  assert(encoded_length == 98U);
  css.version = LAGHU_CRITICAL_CSS_VERSION;
  strcpy(css.template_key, key);
  assert(laghu_rum_encode(LAGHU_RUM_RECORD_CRITICAL_CSS, &css, sizeof(css), encoded, sizeof(encoded), &encoded_length));
  assert(encoded_length == 1499U);
}

static bool write_chrome_report(const char *path, const char *template_key, const char *receipt, const char *snapshot_key) {
  FILE *report;
  if (path == NULL || template_key == NULL || receipt == NULL || snapshot_key == NULL) return false;
  report = fopen(path, "wb");
  return report != NULL &&
         fprintf(report,
                 "{\"version\":1,\"width\":1440,\"lcp_ordinal\":0,\"network_requests_blocked\":0,\"template\":\"%s\","
                 "\"receipt\":\"%s\",\"snapshot\":\"%s\"}",
                 template_key, receipt, snapshot_key) > 0 &&
         fclose(report) == 0;
}

static void test_rum_engine(const char *directory) {
  static const char key[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  static const unsigned char payload[] = {0U, 1U, 2U, 0xfeU, 0xffU};
  laghu_rum_options options;
  laghu_rum_engine *engine;
  laghu_rum_value value;
  unsigned char restored[sizeof(payload)];
  laghu_rum_image_record image = {0}, restored_image = {0};
  char snapshot[LAGHU_RUNTIME_PATH_SIZE];
  assert(snprintf(snapshot, sizeof(snapshot), "%s/rum.snapshot", directory) > 0);
  laghu_rum_options_init(&options);
  options.snapshot_path = snapshot;
  options.sync_interval_seconds = 1U;
  engine = laghu_rum_engine_create(&options, NULL, 0U);
  assert(engine != NULL);
  assert(laghu_rum_engine_publish(engine, LAGHU_RUM_RECORD_DECISION, key, 10U, payload, sizeof(payload), NULL));
  assert(laghu_rum_engine_read(engine, LAGHU_RUM_RECORD_DECISION, key, 11U, restored, sizeof(restored), &value));
  assert(value.length == sizeof(payload) && memcmp(restored, payload, sizeof(payload)) == 0);
  assert(laghu_rum_engine_memory_used(engine) == sizeof(payload));
  strcpy(image.identity, key);
  image.updated_at = 10U;
  image.width = 321U;
  image.above_fold = true;
  assert(laghu_rum_engine_publish(engine, LAGHU_RUM_RECORD_IMAGE, key, 10U, &image, sizeof(image), NULL));
  laghu_rum_engine_destroy(engine);
  engine = laghu_rum_engine_create(&options, NULL, 0U);
  assert(engine != NULL);
  memset(restored, 0, sizeof(restored));
  assert(laghu_rum_engine_read(engine, LAGHU_RUM_RECORD_DECISION, key, 12U, restored, sizeof(restored), &value));
  assert(memcmp(restored, payload, sizeof(payload)) == 0);
  assert(laghu_rum_engine_read(engine, LAGHU_RUM_RECORD_IMAGE, key, 12U, &restored_image, sizeof(restored_image), &value));
  assert(restored_image.width == 321U && restored_image.above_fold && !strcmp(restored_image.identity, key));
  laghu_rum_engine_destroy(engine);
  {
    laghu_rum_engine *peer_one, *peer_two;
    laghu_rum_image_record one = {0}, two = {0};
    laghu_rum_instrumentation_record rum_one = {0}, rum_two = {0}, rum_total;
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
    assert(laghu_rum_engine_publish(peer_one, LAGHU_RUM_RECORD_IMAGE, key, 20U, &one, sizeof(one), NULL));
    assert(laghu_rum_engine_publish(peer_two, LAGHU_RUM_RECORD_IMAGE, key, 21U, &two, sizeof(two), NULL));
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
    assert(laghu_rum_engine_publish(peer_one, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 20U, &rum_one, sizeof(rum_one), NULL));
    assert(laghu_rum_engine_publish(peer_two, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 21U, &rum_two, sizeof(rum_two), NULL));
    laghu_rum_engine_destroy(peer_one);
    laghu_rum_engine_destroy(peer_two);
    engine = laghu_rum_engine_create(&options, NULL, 0U);
    assert(engine != NULL && laghu_rum_engine_read(engine, LAGHU_RUM_RECORD_IMAGE, key, 22U, &restored_image, sizeof(restored_image), &value));
    assert(restored_image.width == 400U && restored_image.height == 300U && restored_image.above_fold);
    assert(laghu_rum_engine_read(engine, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 22U, &rum_total, sizeof(rum_total), &value));
    assert(rum_total.observations[0] == 5U);
    laghu_rum_engine_destroy(engine);
    engine = laghu_rum_engine_create(&options, NULL, 0U);
    assert(engine != NULL && laghu_rum_engine_read(engine, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 23U, &rum_total, sizeof(rum_total), &value));
    assert(rum_total.observations[0] == 5U);
    laghu_rum_engine_destroy(engine);
  }
  laghu_rum_options_init(&options);
  options.store_uri = "memory:";
  options.snapshot_path = NULL;
  options.ttl_seconds = 1U;
  engine = laghu_rum_engine_create(&options, NULL, 0U);
  assert(engine != NULL);
  assert(laghu_rum_engine_publish(engine, LAGHU_RUM_RECORD_DECISION, key, 20U, payload, sizeof(payload), NULL));
  assert(!laghu_rum_engine_read(engine, LAGHU_RUM_RECORD_DECISION, key, 22U, restored, sizeof(restored), &value));
  laghu_rum_engine_destroy(engine);
  options.store_uri = "memcached://127.0.0.1:11211";
  assert(laghu_rum_engine_create(&options, NULL, 0U) == NULL);
  {
    char rum_error[160U];
    options.store_uri = "redis://secret@example.test:6379/0?prefix=laghu:";
    assert(laghu_rum_engine_create(&options, rum_error, sizeof(rum_error)) == NULL);
    assert(strstr(rum_error, "secret") == NULL);
    options.store_uri = "rediss://example.test:6380/0?unknown=value";
    assert(laghu_rum_engine_create(&options, rum_error, sizeof(rum_error)) == NULL);
  }
  {
    laghu_rum_image_record aggregate = {0}, delta = {0};
    strcpy(aggregate.identity, key);
    strcpy(delta.identity, key);
    aggregate.width = 100U;
    delta.width = 200U;
    delta.above_fold = true;
    assert(laghu_rum_record_merge(LAGHU_RUM_RECORD_IMAGE, &aggregate, &delta, sizeof(aggregate)));
    assert(aggregate.width == 200U && aggregate.above_fold);
  }
}

static void test_rum_redis_receipt_race(const laghu_rum_options *options) {
  char directory[] = "/tmp/laghu-rum-receipt-race.XXXXXX";
  char template_key[LAGHU_RUNTIME_KEY_SIZE], receipt[LAGHU_RUNTIME_KEY_SIZE], snapshot[LAGHU_RUNTIME_KEY_SIZE];
  char material[sizeof(directory) + 16U];
  char path[sizeof(directory) + LAGHU_RUNTIME_KEY_SIZE * 2U + 8U];
  laghu_rum_engine *issuer, *importer;
  laghu_rum_instrumentation_record record = {0}, applied = {0};
  uint64_t now;
  unsigned int attempts, imported = 0U;
  if (options == NULL) return;
  assert(mkdtemp(directory) != NULL);
  assert(snprintf(material, sizeof(material), "%s/template", directory) > 0 &&
         laghu_sha256_hex((laghu_buffer){(const unsigned char *)material, strlen(material)}, template_key));
  assert(snprintf(material, sizeof(material), "%s/receipt", directory) > 0 &&
         laghu_sha256_hex((laghu_buffer){(const unsigned char *)material, strlen(material)}, receipt));
  assert(snprintf(material, sizeof(material), "%s/snapshot", directory) > 0 &&
         laghu_sha256_hex((laghu_buffer){(const unsigned char *)material, strlen(material)}, snapshot));
  assert(snprintf(path, sizeof(path), "%s/%s-%s.json", directory, snapshot, receipt) > 0);
  now = (uint64_t)time(NULL);
  record.version = LAGHU_INSTRUMENTATION_VERSION;
  strcpy(record.template_key, template_key);
  record.updated_at = now;
  record.media_count = 1U;

  /* First make the source template durable. Both processes then load it
   * before issuer adds the new receipt only to its pending local state. */
  issuer = laghu_rum_engine_create(options, NULL, 0U);
  assert(issuer != NULL && laghu_rum_engine_publish(issuer, LAGHU_RUM_RECORD_INSTRUMENTATION, template_key, now, &record, sizeof(record), NULL));
  laghu_rum_engine_destroy(issuer);
  issuer = laghu_rum_engine_create(options, NULL, 0U);
  importer = laghu_rum_engine_create(options, NULL, 0U);
  assert(issuer != NULL && importer != NULL);
  assert(laghu_runtime_issue_chrome_analysis_receipt(issuer, template_key, snapshot, receipt, now + 1U, 60U, 1500U));
  assert(write_chrome_report(path, template_key, receipt, snapshot));

  /* This is the former data-loss window: importer has template but no receipt.
   * It must retain, not unlink, canonical worker output. */
  assert(laghu_runtime_import_chrome_analysis(importer, directory, now + 1U, 60U) == 0U);
  assert(access(path, F_OK) == 0);

  /* Do not use shutdown to flush. With separate live engines, wait through
   * issuer push plus importer pull and prove its retry is applied once. */
  for (attempts = 0U; attempts < 50U && imported == 0U; ++attempts) {
    const struct timespec pause = {0, 100000000L};
    (void)nanosleep(&pause, NULL);
    imported = laghu_runtime_import_chrome_analysis(importer, directory, now + 1U + attempts / 10U, 60U);
  }
  assert(imported == 1U);
  assert(access(path, F_OK) != 0);
  assert(laghu_rum_engine_read(importer, LAGHU_RUM_RECORD_INSTRUMENTATION, template_key, now + 6U, &applied, sizeof(applied), NULL));
  assert(applied.lcp_observations[2] == 1U && applied.lcp_candidates[2][0] == 1U && receipt_seen(&applied, receipt, true));
  laghu_rum_engine_destroy(issuer);
  laghu_rum_engine_destroy(importer);
  assert(snprintf(path, sizeof(path), "%s/.laghu-chrome-analysis.lock", directory) > 0);
  assert(unlink(path) == 0);
  assert(rmdir(directory) == 0);
}

static void test_rum_redis(void) {
  static const char key[] = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  static const char receipt_one[] = "1111111111111111111111111111111111111111111111111111111111111111";
  static const char receipt_two[] = "2222222222222222222222222222222222222222222222222222222222222222";
  const char *uri = getenv("LAGHU_TEST_REDIS_URI");
  const char *library = getenv("LAGHU_TEST_HIREDIS_LIBRARY");
  laghu_rum_options options;
  laghu_rum_engine *one, *two, *reader;
  laghu_rum_image_record first = {0}, second = {0}, aggregate = {0};
  laghu_rum_instrumentation_record rum_first = {0}, rum_second = {0}, rum_aggregate = {0};
  laghu_critical_css_record css_first = {0}, css_second = {0}, css_aggregate = {0};
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
  strcpy(first.identity, key);
  strcpy(second.identity, key);
  first.updated_at = 100U;
  first.width = 640U;
  second.updated_at = 101U;
  second.height = 480U;
  second.above_fold = true;
  assert(laghu_rum_engine_publish(one, LAGHU_RUM_RECORD_IMAGE, key, 100U, &first, sizeof(first), NULL));
  assert(laghu_rum_engine_publish(two, LAGHU_RUM_RECORD_IMAGE, key, 101U, &second, sizeof(second), NULL));
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
  strcpy(rum_first.chrome_analysis_receipts[0].value, receipt_one);
  strcpy(rum_first.chrome_analysis_receipts[0].snapshot_key, key);
  rum_first.chrome_analysis_receipts[0].expires_at = 600U;
  strcpy(rum_second.chrome_analysis_receipts[0].value, receipt_two);
  strcpy(rum_second.chrome_analysis_receipts[0].snapshot_key, key);
  rum_second.chrome_analysis_receipts[0].expires_at = 600U;
  rum_second.chrome_analysis_receipts[0].consumed = 1U;
  assert(laghu_rum_engine_publish(one, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 100U, &rum_first, sizeof(rum_first), NULL));
  assert(laghu_rum_engine_publish(two, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 101U, &rum_second, sizeof(rum_second), NULL));
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
  assert(laghu_rum_engine_publish(one, LAGHU_RUM_RECORD_CRITICAL_CSS, key, 100U, &css_first, sizeof(css_first), NULL));
  assert(laghu_rum_engine_publish(two, LAGHU_RUM_RECORD_CRITICAL_CSS, key, 101U, &css_second, sizeof(css_second), NULL));
  assert(laghu_rum_engine_publish(one, LAGHU_RUM_RECORD_DECISION, key, 100U, decision, sizeof(decision), NULL));
  laghu_rum_engine_destroy(one);
  laghu_rum_engine_destroy(two);
  reader = laghu_rum_engine_create(&options, NULL, 0U);
  assert(reader != NULL && laghu_rum_engine_read(reader, LAGHU_RUM_RECORD_IMAGE, key, 102U, &aggregate, sizeof(aggregate), &value));
  assert(aggregate.width == 640U && aggregate.height == 480U && aggregate.above_fold);
  assert(laghu_rum_engine_read(reader, LAGHU_RUM_RECORD_INSTRUMENTATION, key, 102U, &rum_aggregate, sizeof(rum_aggregate), &value));
  assert(rum_aggregate.observations[1] == UINT64_MAX);
  assert(receipt_seen(&rum_aggregate, receipt_one, false) && receipt_seen(&rum_aggregate, receipt_two, true));
  assert(laghu_rum_engine_read(reader, LAGHU_RUM_RECORD_CRITICAL_CSS, key, 102U, &css_aggregate, sizeof(css_aggregate), &value));
  assert(css_aggregate.generation == UINT32_MAX && css_aggregate.observation_count[0] == UINT16_MAX && css_aggregate.critical_rules[0][0] == 0x81U);
  assert(laghu_rum_engine_read(reader, LAGHU_RUM_RECORD_DECISION, key, 102U, decision_result, sizeof(decision_result), &value));
  assert(memcmp(decision_result, decision, sizeof(decision)) == 0);
  laghu_rum_engine_destroy(reader);
  test_rum_redis_receipt_race(&options);
}

int main(void) {
  char directory[LAGHU_RUNTIME_PATH_SIZE];
  assert(laghu_test_directory(directory, sizeof(directory)));
  test_rum_codec_receipts();
  test_rum_engine(directory);
  test_rum_redis();
  puts("laghu_runtime_rum_integration_test: all tests passed");
  return 0;
}
