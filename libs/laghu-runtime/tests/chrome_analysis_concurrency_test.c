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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "laghu/instrumentation.h"

static void laghu_chrome_analysis_test_fail(const char *expression, unsigned int line) {
  (void)fprintf(stderr, "chrome-analysis-concurrency check failed at line %u: %s\n", line, expression);
  abort();
}

#undef assert
#define assert(expression) ((expression) ? (void)0 : laghu_chrome_analysis_test_fail(#expression, __LINE__))

static const char template_key[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char receipt[] = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
static const char snapshot[] = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
#define LAGHU_CHROME_ANALYSIS_CLAIM_NAME ".laghu-chrome-analysis.lock"

typedef struct {
  unsigned char imported;
  unsigned char lcp_observations;
} laghu_chrome_import_result;

static bool write_report(const char *path) {
  FILE *file = fopen(path, "wb");
  bool written;
  if (file == NULL) return false;
  written = fprintf(file,
                    "{\"version\":1,\"width\":1440,\"lcp_ordinal\":0,\"network_requests_blocked\":0,\"template\":\"%s\","
                    "\"receipt\":\"%s\",\"snapshot\":\"%s\"}",
                    template_key, receipt, snapshot) > 0;
  return fclose(file) == 0 && written;
}

static void import_child(int start, int result, const char *directory, uint64_t now) {
  laghu_rum_options options;
  laghu_rum_engine *rum;
  laghu_rum_instrumentation_record record = {0};
  unsigned char begin;
  laghu_chrome_import_result outcome = {0};
  if (read(start, &begin, 1U) != 1) _exit(2);
  laghu_rum_options_init(&options);
  options.store_uri = "memory:";
  options.ttl_seconds = 60U;
  options.sync_interval_seconds = 1U;
  rum = laghu_rum_engine_create(&options, NULL, 0U);
  if (rum == NULL) _exit(2);
  record.version = LAGHU_INSTRUMENTATION_VERSION;
  strcpy(record.template_key, template_key);
  record.updated_at = now;
  record.media_count = 1U;
  if (!laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, template_key, now, &record, sizeof(record), NULL) ||
      !laghu_runtime_issue_chrome_analysis_receipt(rum, template_key, snapshot, receipt, now, 60U, 100U))
    _exit(2);
  outcome.imported = (unsigned char)laghu_runtime_import_chrome_analysis(rum, directory, now, 60U);
  if (!laghu_rum_engine_read(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, template_key, now, &record, sizeof(record), NULL)) _exit(2);
  outcome.lcp_observations = (unsigned char)record.lcp_observations[2];
  laghu_rum_engine_destroy(rum);
  if (write(result, &outcome, sizeof(outcome)) != (ssize_t)sizeof(outcome)) _exit(2);
  _exit(0);
}

static void wait_child(pid_t child) {
  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static bool read_exact(int descriptor, void *output, size_t length) {
  size_t offset = 0U;
  while (offset < length) {
    ssize_t bytes = read(descriptor, (unsigned char *)output + offset, length - offset);
    if (bytes <= 0) return false;
    offset += (size_t)bytes;
  }
  return true;
}

static void concurrent_imports(const char *directory, uint64_t now, unsigned int *imports, unsigned int *observations) {
  int start[2], result[2];
  pid_t first, second;
  unsigned char begin[] = {1U, 1U};
  laghu_chrome_import_result outcome;
  unsigned int index;
  assert(imports != NULL && observations != NULL);
  *imports = *observations = 0U;
  assert(pipe(start) == 0 && pipe(result) == 0);
  first = fork();
  assert(first >= 0);
  if (first == 0) {
    (void)close(start[1]);
    (void)close(result[0]);
    import_child(start[0], result[1], directory, now);
  }
  second = fork();
  assert(second >= 0);
  if (second == 0) {
    (void)close(start[1]);
    (void)close(result[0]);
    import_child(start[0], result[1], directory, now);
  }
  (void)close(start[0]);
  (void)close(result[1]);
  assert(write(start[1], begin, sizeof(begin)) == (ssize_t)sizeof(begin));
  (void)close(start[1]);
  for (index = 0U; index < 2U; ++index) {
    assert(read_exact(result[0], &outcome, sizeof(outcome)));
    *imports += outcome.imported;
    *observations += outcome.lcp_observations;
  }
  (void)close(result[0]);
  wait_child(first);
  wait_child(second);
}

static void assert_crash_releases_claim(const char *directory, uint64_t now) {
  char path[LAGHU_RUNTIME_PATH_SIZE * 2U];
  int ready[2];
  pid_t holder;
  int descriptor;
  unsigned int imports, observations;
  assert(snprintf(path, sizeof(path), "%s/%s", directory, LAGHU_CHROME_ANALYSIS_CLAIM_NAME) > 0);
  assert(pipe(ready) == 0);
  holder = fork();
  assert(holder >= 0);
  if (holder == 0) {
    unsigned char acquired = 1U;
    (void)close(ready[0]);
    descriptor = open(path, O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0 || flock(descriptor, LOCK_EX) != 0 || write(ready[1], &acquired, 1U) != 1) _exit(2);
    _exit(0);
  }
  (void)close(ready[1]);
  {
    unsigned char acquired;
    assert(read(ready[0], &acquired, 1U) == 1 && acquired == 1U);
  }
  (void)close(ready[0]);
  wait_child(holder);
  concurrent_imports(directory, now, &imports, &observations);
  assert(imports == 1U && observations == 1U);
}

int main(void) {
  char directory[] = "/tmp/laghu-chrome-analysis-concurrency.XXXXXX";
  char path[sizeof(directory) + LAGHU_RUNTIME_KEY_SIZE * 2U + 8U];
  char claim[LAGHU_RUNTIME_PATH_SIZE * 2U];
  uint64_t now = (uint64_t)time(NULL);
  unsigned int iteration, imports, observations;
  assert(mkdtemp(directory) != NULL);
  assert(snprintf(path, sizeof(path), "%s/%s-%s.json", directory, snapshot, receipt) > 0);
  for (iteration = 0U; iteration < 16U; ++iteration) {
    assert(write_report(path));
    concurrent_imports(directory, now, &imports, &observations);
    assert(imports == 1U && observations == 1U);
    assert(access(path, F_OK) != 0);
  }
  assert(write_report(path));
  assert_crash_releases_claim(directory, now);
  assert(access(path, F_OK) != 0);
  assert(snprintf(claim, sizeof(claim), "%s/%s", directory, LAGHU_CHROME_ANALYSIS_CLAIM_NAME) > 0);
  assert(unlink(claim) == 0);
  assert(rmdir(directory) == 0);
  return 0;
}
