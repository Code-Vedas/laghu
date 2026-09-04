// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "laghu/instrumentation.h"

#define LAGHU_CHROME_ANALYSIS_MAX_JSON 16384U
#define LAGHU_CHROME_ANALYSIS_MAX_NETWORK_REQUESTS 100000U
#define LAGHU_CHROME_ANALYSIS_CLAIM_NAME ".laghu-chrome-analysis.lock"

typedef enum {
  LAGHU_CHROME_ANALYSIS_REJECT = 0,
  LAGHU_CHROME_ANALYSIS_RETRY = 1,
  LAGHU_CHROME_ANALYSIS_APPLIED = 2
} laghu_chrome_analysis_apply_result;

typedef struct {
  char template_key[LAGHU_RUNTIME_KEY_SIZE];
  char snapshot_key[LAGHU_RUNTIME_KEY_SIZE];
  char receipt[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int ordinal;
  unsigned int bucket;
  uint64_t now;
  unsigned int ttl_seconds;
  laghu_chrome_analysis_apply_result result;
} laghu_chrome_analysis_context;

typedef struct {
  char template_key[LAGHU_RUNTIME_KEY_SIZE];
  char snapshot_key[LAGHU_RUNTIME_KEY_SIZE];
  char receipt[LAGHU_RUNTIME_KEY_SIZE];
  uint64_t now;
  uint64_t expires_at;
  unsigned int ttl_seconds;
  unsigned int required_lifetime_seconds;
} laghu_chrome_receipt_context;

static bool laghu_chrome_hex_key(const char value[LAGHU_RUNTIME_KEY_SIZE]) {
  size_t index;
  if (value == NULL || value[LAGHU_SHA256_HEX_LENGTH] != '\0') return false;
  for (index = 0U; index < LAGHU_SHA256_HEX_LENGTH; ++index)
    if (!((value[index] >= '0' && value[index] <= '9') || (value[index] >= 'a' && value[index] <= 'f'))) return false;
  return true;
}

static bool laghu_chrome_receipt_valid(const laghu_chrome_analysis_receipt *receipt) {
  if (receipt == NULL || receipt->consumed > 1U) return false;
  if (receipt->value[0] == '\0') return receipt->snapshot_key[0] == '\0' && receipt->expires_at == 0U && receipt->consumed == 0U;
  return laghu_chrome_hex_key(receipt->value) && laghu_chrome_hex_key(receipt->snapshot_key) && receipt->expires_at != 0U;
}

static bool laghu_chrome_record_receipts_valid(const laghu_rum_instrumentation_record *record) {
  size_t index;
  if (record == NULL) return false;
  for (index = 0U; index < LAGHU_CHROME_ANALYSIS_MAX_RECEIPTS; ++index)
    if (!laghu_chrome_receipt_valid(&record->chrome_analysis_receipts[index])) return false;
  return true;
}

static bool laghu_chrome_literal(const char **cursor, const char *literal) {
  size_t length;
  if (cursor == NULL || *cursor == NULL || literal == NULL) return false;
  length = strlen(literal);
  if (strncmp(*cursor, literal, length) != 0) return false;
  *cursor += length;
  return true;
}

static bool laghu_chrome_uint(const char **cursor, unsigned int maximum, unsigned int *output) {
  const char *value;
  unsigned long number = 0U;
  if (cursor == NULL || *cursor == NULL || output == NULL || !isdigit((unsigned char)**cursor)) return false;
  value = *cursor;
  if (*value == '0') {
    ++value;
    if (isdigit((unsigned char)*value)) return false;
  } else {
    do {
      number = number * 10U + (unsigned long)(*value - '0');
      if (number > maximum) return false;
      ++value;
    } while (isdigit((unsigned char)*value));
  }
  if (**cursor == '0') number = 0U;
  *output = (unsigned int)number;
  *cursor = value;
  return true;
}

static bool laghu_chrome_key(const char **cursor, char output[LAGHU_RUNTIME_KEY_SIZE]) {
  if (cursor == NULL || *cursor == NULL || output == NULL || **cursor != '"') return false;
  ++*cursor;
  if (strlen(*cursor) < LAGHU_SHA256_HEX_LENGTH + 1U || (*cursor)[LAGHU_SHA256_HEX_LENGTH] != '"') return false;
  memcpy(output, *cursor, LAGHU_SHA256_HEX_LENGTH);
  output[LAGHU_SHA256_HEX_LENGTH] = '\0';
  *cursor += LAGHU_SHA256_HEX_LENGTH + 1U;
  return laghu_chrome_hex_key(output);
}

/* The worker emits one fixed, ordered object. Requiring that exact grammar
 * rejects duplicate fields, comment/script payloads and parser differentials. */
static bool laghu_chrome_report(const char *json, laghu_chrome_analysis_context *context) {
  const char *cursor = json;
  unsigned int version, width, ordinal, blocked;
  if (json == NULL || context == NULL || !laghu_chrome_literal(&cursor, "{\"version\":") || !laghu_chrome_uint(&cursor, 1U, &version) ||
      version != 1U || !laghu_chrome_literal(&cursor, ",\"width\":") || !laghu_chrome_uint(&cursor, 100000U, &width) || width == 0U ||
      !laghu_chrome_literal(&cursor, ",\"lcp_ordinal\":") || !laghu_chrome_uint(&cursor, LAGHU_LCP_MAX_CANDIDATES - 1U, &ordinal) ||
      !laghu_chrome_literal(&cursor, ",\"network_requests_blocked\":") ||
      !laghu_chrome_uint(&cursor, LAGHU_CHROME_ANALYSIS_MAX_NETWORK_REQUESTS, &blocked) || !laghu_chrome_literal(&cursor, ",\"template\":") ||
      !laghu_chrome_key(&cursor, context->template_key) || !laghu_chrome_literal(&cursor, ",\"receipt\":") ||
      !laghu_chrome_key(&cursor, context->receipt) || !laghu_chrome_literal(&cursor, ",\"snapshot\":") ||
      !laghu_chrome_key(&cursor, context->snapshot_key) || !laghu_chrome_literal(&cursor, "}") || *cursor != '\0')
    return false;
  (void)blocked;
  context->ordinal = ordinal;
  context->bucket = width < 768U ? 0U : 1U;
  return true;
}

static bool laghu_chrome_issue_merge(void *data, size_t length, void *opaque) {
  laghu_rum_instrumentation_record *record = data;
  const laghu_chrome_receipt_context *context = opaque;
  uint64_t record_expires_at, expires_at;
  size_t index;
  laghu_chrome_analysis_receipt *available = NULL;
  if (record == NULL || context == NULL || length != sizeof(*record) || record->version != LAGHU_INSTRUMENTATION_VERSION ||
      strcmp(record->template_key, context->template_key) != 0 || context->now < record->updated_at ||
      context->now - record->updated_at > context->ttl_seconds || !laghu_chrome_record_receipts_valid(record))
    return false;
  if (record->updated_at > UINT64_MAX - context->ttl_seconds) return false;
  record_expires_at = record->updated_at + context->ttl_seconds;
  if (record_expires_at < context->now || record_expires_at - context->now < context->required_lifetime_seconds) return false;
  expires_at = context->expires_at < record_expires_at ? context->expires_at : record_expires_at;
  for (index = 0U; index < LAGHU_CHROME_ANALYSIS_MAX_RECEIPTS; ++index) {
    laghu_chrome_analysis_receipt *entry = &record->chrome_analysis_receipts[index];
    if (entry->value[0] != '\0' && entry->expires_at < context->now) memset(entry, 0, sizeof(*entry));
    if (memcmp(entry->value, context->receipt, sizeof(entry->value)) == 0) return false;
    if (available == NULL && entry->value[0] == '\0') available = entry;
  }
  if (available == NULL) return false;
  memcpy(available->value, context->receipt, sizeof(available->value));
  memcpy(available->snapshot_key, context->snapshot_key, sizeof(available->snapshot_key));
  available->expires_at = expires_at;
  available->consumed = 0U;
  return true;
}

static bool laghu_chrome_revoke_merge(void *data, size_t length, void *opaque) {
  laghu_rum_instrumentation_record *record = data;
  const laghu_chrome_receipt_context *context = opaque;
  size_t index;
  if (record == NULL || context == NULL || length != sizeof(*record) || record->version != LAGHU_INSTRUMENTATION_VERSION ||
      strcmp(record->template_key, context->template_key) != 0 || !laghu_chrome_record_receipts_valid(record))
    return false;
  for (index = 0U; index < LAGHU_CHROME_ANALYSIS_MAX_RECEIPTS; ++index) {
    laghu_chrome_analysis_receipt *entry = &record->chrome_analysis_receipts[index];
    if (memcmp(entry->value, context->receipt, sizeof(entry->value)) == 0) {
      memset(entry, 0, sizeof(*entry));
      return true;
    }
  }
  return false;
}

bool laghu_runtime_issue_chrome_analysis_receipt(laghu_rum_engine *rum, const char *template_key, const char *snapshot_key, const char *receipt,
                                                 uint64_t now, unsigned int ttl_seconds, unsigned int analysis_timeout_ms) {
  laghu_chrome_receipt_context context;
  unsigned int execution_seconds, sync_seconds, rum_ttl_seconds, effective_ttl_seconds;
  if (rum == NULL || template_key == NULL || snapshot_key == NULL || receipt == NULL || ttl_seconds == 0U || analysis_timeout_ms < 100U ||
      analysis_timeout_ms > 10000U || now > UINT64_MAX - ttl_seconds)
    return false;
  execution_seconds = (analysis_timeout_ms + 999U) / 1000U;
  rum_ttl_seconds = laghu_rum_engine_ttl_seconds(rum);
  sync_seconds = laghu_rum_engine_sync_interval_seconds(rum);
  if (rum_ttl_seconds == 0U || sync_seconds == 0U) return false;
  effective_ttl_seconds = ttl_seconds < rum_ttl_seconds ? ttl_seconds : rum_ttl_seconds;
  if (now > UINT64_MAX - effective_ttl_seconds) return false;
  memset(&context, 0, sizeof(context));
  if (snprintf(context.template_key, sizeof(context.template_key), "%s", template_key) != LAGHU_SHA256_HEX_LENGTH ||
      snprintf(context.snapshot_key, sizeof(context.snapshot_key), "%s", snapshot_key) != LAGHU_SHA256_HEX_LENGTH ||
      snprintf(context.receipt, sizeof(context.receipt), "%s", receipt) != LAGHU_SHA256_HEX_LENGTH || !laghu_chrome_hex_key(context.template_key) ||
      !laghu_chrome_hex_key(context.snapshot_key) || !laghu_chrome_hex_key(context.receipt))
    return false;
  context.now = now;
  context.expires_at = now + effective_ttl_seconds;
  context.ttl_seconds = effective_ttl_seconds;
  /* The issuer can be one cadence away from pushing the receipt, and a
   * different importer can be one cadence away from pulling it.  The final
   * second covers the importer lifecycle pass after that pull. */
  context.required_lifetime_seconds = execution_seconds + sync_seconds * 2U + 1U;
  return laghu_rum_engine_update_preserving_updated_at(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, context.template_key, now, laghu_chrome_issue_merge,
                                                       &context, NULL);
}

bool laghu_runtime_revoke_chrome_analysis_receipt(laghu_rum_engine *rum, const char *template_key, const char *receipt, uint64_t now) {
  laghu_chrome_receipt_context context;
  if (rum == NULL || template_key == NULL || receipt == NULL) return false;
  memset(&context, 0, sizeof(context));
  if (snprintf(context.template_key, sizeof(context.template_key), "%s", template_key) != LAGHU_SHA256_HEX_LENGTH ||
      snprintf(context.receipt, sizeof(context.receipt), "%s", receipt) != LAGHU_SHA256_HEX_LENGTH || !laghu_chrome_hex_key(context.template_key) ||
      !laghu_chrome_hex_key(context.receipt))
    return false;
  return laghu_rum_engine_update_preserving_updated_at(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, context.template_key, now, laghu_chrome_revoke_merge,
                                                       &context, NULL);
}

static bool laghu_chrome_merge(void *data, size_t length, void *opaque) {
  laghu_rum_instrumentation_record *record = data;
  laghu_chrome_analysis_context *context = opaque;
  unsigned int lcp_bucket;
  size_t index;
  laghu_chrome_analysis_receipt *receipt = NULL;
  if (record == NULL || context == NULL || length != sizeof(*record) || record->version != LAGHU_INSTRUMENTATION_VERSION ||
      strcmp(record->template_key, context->template_key) != 0 || context->now < record->updated_at ||
      context->now - record->updated_at > context->ttl_seconds || context->ordinal >= record->media_count ||
      !laghu_chrome_record_receipts_valid(record)) {
    if (context != NULL) context->result = LAGHU_CHROME_ANALYSIS_REJECT;
    return false;
  }
  for (index = 0U; index < LAGHU_CHROME_ANALYSIS_MAX_RECEIPTS; ++index) {
    laghu_chrome_analysis_receipt *entry = &record->chrome_analysis_receipts[index];
    if (memcmp(entry->value, context->receipt, sizeof(entry->value)) == 0) {
      receipt = entry;
      break;
    }
  }
  if (receipt == NULL) {
    context->result = LAGHU_CHROME_ANALYSIS_RETRY;
    return false;
  }
  if (receipt->consumed != 0U || receipt->expires_at < context->now ||
      memcmp(receipt->snapshot_key, context->snapshot_key, sizeof(receipt->snapshot_key)) != 0) {
    context->result = LAGHU_CHROME_ANALYSIS_REJECT;
    return false;
  }
  receipt->consumed = 1U;
  lcp_bucket = context->bucket * 2U;
  if (record->lcp_observations[lcp_bucket] != UINT16_MAX) ++record->lcp_observations[lcp_bucket];
  if (record->lcp_candidates[lcp_bucket][context->ordinal] != UINT16_MAX) ++record->lcp_candidates[lcp_bucket][context->ordinal];
  record->updated_at = context->now;
  context->result = LAGHU_CHROME_ANALYSIS_APPLIED;
  return true;
}

static bool laghu_chrome_parse_report(laghu_buffer json, laghu_chrome_analysis_context *context) {
  char *text;
  bool parsed;
  if (json.data == NULL || json.length == 0U || json.length > LAGHU_CHROME_ANALYSIS_MAX_JSON || context == NULL ||
      memchr(json.data, '\0', json.length) != NULL)
    return false;
  text = malloc(json.length + 1U);
  if (text == NULL) return false;
  memcpy(text, json.data, json.length);
  text[json.length] = '\0';
  memset(context, 0, sizeof(*context));
  parsed = laghu_chrome_report(text, context);
  free(text);
  return parsed;
}

static laghu_chrome_analysis_apply_result laghu_chrome_apply_report(laghu_rum_engine *rum, laghu_chrome_analysis_context *context, uint64_t now,
                                                                    unsigned int ttl_seconds) {
  if (rum == NULL || context == NULL || ttl_seconds == 0U) return LAGHU_CHROME_ANALYSIS_REJECT;
  context->now = now;
  context->ttl_seconds = ttl_seconds;
  context->result = LAGHU_CHROME_ANALYSIS_RETRY;
  if (!laghu_rum_engine_update(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, context->template_key, now, laghu_chrome_merge, context, NULL))
    return context->result;
  return context->result;
}

bool laghu_runtime_apply_chrome_analysis(laghu_rum_engine *rum, laghu_buffer json, uint64_t now, unsigned int ttl_seconds) {
  laghu_chrome_analysis_context context;
  return laghu_chrome_parse_report(json, &context) && laghu_chrome_apply_report(rum, &context, now, ttl_seconds) == LAGHU_CHROME_ANALYSIS_APPLIED;
}

static bool laghu_chrome_read_exact(int descriptor, unsigned char *payload, size_t length) {
  size_t offset = 0U;
  while (offset < length) {
    ssize_t bytes = read(descriptor, payload + offset, length - offset);
    if (bytes <= 0) return false;
    offset += (size_t)bytes;
  }
  return true;
}

static bool laghu_chrome_report_name_matches(const char *name, const laghu_chrome_analysis_context *context) {
  size_t expected = LAGHU_SHA256_HEX_LENGTH * 2U + sizeof("-.json") - 1U;
  if (name == NULL || context == NULL || strlen(name) != expected) return false;
  return memcmp(name, context->snapshot_key, LAGHU_SHA256_HEX_LENGTH) == 0 && name[LAGHU_SHA256_HEX_LENGTH] == '-' &&
         memcmp(name + LAGHU_SHA256_HEX_LENGTH + 1U, context->receipt, LAGHU_SHA256_HEX_LENGTH) == 0 &&
         strcmp(name + LAGHU_SHA256_HEX_LENGTH * 2U + 1U, ".json") == 0;
}

static bool laghu_chrome_report_expired(const struct stat *status, unsigned int ttl_seconds) {
  time_t current;
  if (status == NULL || ttl_seconds == 0U || status->st_mtime < 0) return false;
  current = time(NULL);
  if (current == (time_t)-1 || status->st_mtime > current) return false;
  return (uint64_t)(current - status->st_mtime) > ttl_seconds;
}

static bool laghu_chrome_path_matches_locked(int descriptor, const char *path, const struct stat *expected) {
  struct stat current, opened;
  if (descriptor < 0 || path == NULL || expected == NULL || fstat(descriptor, &opened) != 0 || lstat(path, &current) != 0 ||
      !S_ISREG(current.st_mode) || opened.st_dev != expected->st_dev || opened.st_ino != expected->st_ino || current.st_dev != expected->st_dev ||
      current.st_ino != expected->st_ino)
    return false;
  return true;
}

static bool laghu_chrome_unlink_locked(int descriptor, const char *path, const struct stat *expected) {
  if (!laghu_chrome_path_matches_locked(descriptor, path, expected)) return false;
  return unlink(path) == 0;
}

static int laghu_chrome_directory_claim(const char *directory) {
  char path[LAGHU_RUNTIME_PATH_SIZE * 2U];
  int descriptor, length;
  if (directory == NULL) return -1;
  length = snprintf(path, sizeof(path), "%s/%s", directory, LAGHU_CHROME_ANALYSIS_CLAIM_NAME);
  if (length <= 0 || (size_t)length >= sizeof(path)) return -1;
  descriptor = open(path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
  if (descriptor < 0 || flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    if (descriptor >= 0) (void)close(descriptor);
    return -1;
  }
  return descriptor;
}

unsigned int laghu_runtime_import_chrome_analysis(laghu_rum_engine *rum, const char *directory, uint64_t now, unsigned int ttl_seconds) {
  DIR *stream;
  struct dirent *entry;
  unsigned int imported = 0U, attempted = 0U;
  int directory_claim;
  if (rum == NULL || directory == NULL || directory[0] == '\0') return 0U;
  directory_claim = laghu_chrome_directory_claim(directory);
  if (directory_claim < 0) return 0U;
  stream = opendir(directory);
  if (stream == NULL) {
    (void)close(directory_claim);
    return 0U;
  }
  while (attempted < 8U && (entry = readdir(stream)) != NULL) {
    char path[LAGHU_RUNTIME_PATH_SIZE * 2U];
    struct stat status;
    unsigned char payload[LAGHU_CHROME_ANALYSIS_MAX_JSON];
    int descriptor;
    bool parsed;
    laghu_chrome_analysis_context context;
    laghu_chrome_analysis_apply_result result;
    size_t name_length = strlen(entry->d_name);
    if (name_length <= 5U || strcmp(entry->d_name + name_length - 5U, ".json") != 0 ||
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >= (int)sizeof(path))
      continue;
    ++attempted;
    descriptor = open(path, O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0 || flock(descriptor, LOCK_EX | LOCK_NB) != 0 || fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size <= 0 || status.st_size > (off_t)sizeof(payload)) {
      if (descriptor >= 0) (void)close(descriptor);
      continue;
    }
    /* The directory lock was acquired before opening any report, closing the
     * open-before-lock replay race between cooperating importers. This file
     * lock and inode checks retain that claim through terminal cleanup. */
    if (!laghu_chrome_path_matches_locked(descriptor, path, &status)) {
      (void)close(descriptor);
      continue;
    }
    parsed = laghu_chrome_read_exact(descriptor, payload, (size_t)status.st_size) &&
             laghu_chrome_parse_report((laghu_buffer){payload, (size_t)status.st_size}, &context);
    if (!parsed || !laghu_chrome_report_name_matches(entry->d_name, &context)) {
      /* Workers publish atomically. A malformed or mismatched report cannot
       * become valid, including forged DOM/comment/script marker output. */
      (void)laghu_chrome_unlink_locked(descriptor, path, &status);
      (void)close(descriptor);
      continue;
    }
    result = laghu_chrome_apply_report(rum, &context, now, ttl_seconds);
    if (result == LAGHU_CHROME_ANALYSIS_APPLIED) {
      if (laghu_chrome_unlink_locked(descriptor, path, &status)) ++imported;
    } else if (result == LAGHU_CHROME_ANALYSIS_REJECT || laghu_chrome_report_expired(&status, ttl_seconds)) {
      /* A recognized replay/mismatch is terminal. Unknown canonical receipts
       * wait through metadata TTL for issuer-to-importer RUM replication. */
      (void)laghu_chrome_unlink_locked(descriptor, path, &status);
    }
    (void)close(descriptor);
  }
  (void)closedir(stream);
  (void)close(directory_claim);
  return imported;
}
