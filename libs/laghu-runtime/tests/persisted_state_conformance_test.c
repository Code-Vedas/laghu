/* Copyright Codevedas Inc. 2026-present
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#include "../src/runtime_platform.h"
#include "laghu/cache.h"
#include "laghu/operational.h"
#include "laghu/queue.h"
#include "test_fixture.h"

static void conformance_hash(char output[LAGHU_RUNTIME_KEY_SIZE], char value) {
  memset(output, value, LAGHU_RUNTIME_KEY_SIZE - 1U);
  output[LAGHU_RUNTIME_KEY_SIZE - 1U] = '\0';
}

static bool conformance_contains(const unsigned char *data, size_t length,
                                 const char *needle) {
  size_t needle_length = strlen(needle), index;
  if (data == NULL || needle_length == 0U || needle_length > length)
    return false;
  for (index = 0U; index <= length - needle_length; ++index)
    if (memcmp(data + index, needle, needle_length) == 0) return true;
  return false;
}

static uint64_t conformance_now(void) {
  time_t value = time(NULL);
  assert(value > 0);
  return (uint64_t)value;
}

static bool conformance_path(char *output, size_t output_size,
                             const char *directory, const char *suffix) {
  int written;
  if (output == NULL || output_size == 0U || directory == NULL ||
      suffix == NULL)
    return false;
#ifdef _WIN32
  written = snprintf(output, output_size, "%s\\%s", directory, suffix);
#else
  written = snprintf(output, output_size, "%s/%s", directory, suffix);
#endif
  return written > 0 && (size_t)written < output_size;
}

static void conformance_sleep(void) {
#ifdef _WIN32
  Sleep(10U);
#else
  struct timespec delay = {0, 10000000L};
  (void)nanosleep(&delay, NULL);
#endif
}

static void conformance_wait_for(const char *path) {
  unsigned int attempt;
  uint64_t size;
  for (attempt = 0U; attempt < 1000U; ++attempt) {
    if (laghu_runtime_file_size(path, &size)) return;
    conformance_sleep();
  }
  assert(false);
}

static void conformance_run_worker_once(const char *worker,
                                        const char *queue_path,
                                        const char *cache_path) {
  char command[LAGHU_RUNTIME_PATH_SIZE * 3U];
  int written =
      snprintf(command, sizeof(command), "\"%s\" --once \"%s\" \"%s\"", worker,
               queue_path, cache_path);
  assert(written > 0 && (size_t)written < sizeof(command));
#ifdef _WIN32
  STARTUPINFOA startup;
  PROCESS_INFORMATION process;
  DWORD exit_code;
  memset(&startup, 0, sizeof(startup));
  memset(&process, 0, sizeof(process));
  startup.cb = sizeof(startup);
  assert(CreateProcessA(NULL, command, NULL, NULL, FALSE, 0U, NULL, NULL,
                        &startup, &process));
  assert(WaitForSingleObject(process.hProcess, 30000U) == WAIT_OBJECT_0);
  assert(GetExitCodeProcess(process.hProcess, &exit_code));
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  assert(exit_code == 0U);
#else
  assert(system(command) == 0);
#endif
}

static void conformance_signal(const char *base, const char *name,
                               const char *suffix) {
  char path[LAGHU_RUNTIME_PATH_SIZE];
  int written = snprintf(path, sizeof(path), "%s.%s%s", base, name, suffix);
  assert(written > 0 && (size_t)written < sizeof(path));
  assert(
      laghu_runtime_file_write_atomic(path, (const unsigned char *)"ok", 2U));
}

static const laghu_operational_slot_snapshot *conformance_javascript_slot(
    const laghu_operational_snapshot *snapshot) {
  unsigned int index;
  for (index = 0U; index < snapshot->slot_count; ++index) {
    const laghu_operational_slot_snapshot *slot = &snapshot->slots[index];
    if (slot->active != 0U &&
        slot->process_kind == LAGHU_OPERATIONAL_PROCESS_JAVASCRIPT)
      return slot;
  }
  return NULL;
}

static void conformance_registry_peer(const char *cache_path,
                                      const char *signal_path) {
  laghu_operational_registry observer;
  laghu_operational_snapshot snapshot;
  laghu_operational_readiness readiness;
  const laghu_operational_slot_snapshot *javascript;
  char output[LAGHU_OPERATIONAL_RENDER_SIZE];
  size_t length;
  uint64_t first_generation = 0U;
  const char *steps[] = {"healthy", "stale", "released", "replaced"};
  unsigned int index;
  laghu_operational_registry_init(&observer);
  assert(laghu_operational_registry_open(
      &observer, cache_path, LAGHU_OPERATIONAL_SURFACE_STANDALONE,
      LAGHU_OPERATIONAL_PROCESS_ADAPTER, false, conformance_now()));
  conformance_signal(signal_path, "ready", "");
  for (index = 0U; index < sizeof(steps) / sizeof(steps[0]); ++index) {
    char request[LAGHU_RUNTIME_PATH_SIZE];
    {
      int written = snprintf(request, sizeof(request), "%s.%s", signal_path,
                             steps[index]);
      assert(written > 0 && (size_t)written < sizeof(request));
    }
    conformance_wait_for(request);
    assert(laghu_operational_registry_snapshot(&observer, &snapshot));
    javascript = conformance_javascript_slot(&snapshot);
    if (strcmp(steps[index], "healthy") == 0) {
      assert(javascript != NULL && javascript->healthy != 0U &&
             javascript->queue_capacity == 8U &&
             javascript->queue_occupied == 0U &&
             javascript->latency_count == 1U &&
             javascript->failures[LAGHU_OPERATIONAL_FAILURE_TRANSFORM] == 1U);
      first_generation = javascript->generation;
      assert(laghu_operational_render_prometheus(
          &snapshot, conformance_now(), output, sizeof(output), &length));
      assert(strstr(output, "laghu_worker_up{process_kind=\"javascript\"") !=
             NULL);
      assert(strstr(output,
                    "laghu_failures_total{subsystem=\"transform\"} 1") != NULL);
      assert(laghu_operational_readiness_evaluate(
          &snapshot, conformance_now(), true, true, true, &readiness));
      assert(readiness.workers_ready && !readiness.degraded &&
             readiness.configured_workers == 1U &&
             readiness.healthy_workers == 1U);
    } else if (strcmp(steps[index], "stale") == 0) {
      assert(javascript != NULL && javascript->heartbeat == 1U);
      assert(laghu_operational_readiness_evaluate(
          &snapshot, conformance_now(), true, true, true, &readiness));
      assert(!readiness.workers_ready && readiness.degraded &&
             readiness.configured_workers == 1U &&
             readiness.healthy_workers == 0U);
    } else if (strcmp(steps[index], "released") == 0) {
      assert(javascript == NULL);
    } else {
      assert(javascript != NULL && javascript->generation > first_generation &&
             javascript->healthy != 0U);
    }
    conformance_signal(signal_path, steps[index], ".ok");
  }
  laghu_operational_registry_close(&observer);
}

static void conformance_consume_rust_job(const char *queue_path) {
  laghu_runtime_queue queue;
  laghu_runtime_job job;
  unsigned char payload[128U];
  laghu_runtime_queue_init(&queue);
  assert(laghu_runtime_queue_open(&queue, queue_path));
  assert(laghu_runtime_queue_try_take(&queue, &job, payload, sizeof(payload)));
  assert(job.kind == LAGHU_RUNTIME_JOB_JAVASCRIPT &&
         strcmp(job.request_path, "/cross-language.js") == 0 &&
         strcmp(job.javascript_target, "last 2 chrome versions") == 0 &&
         job.payload.length == strlen("console.log('rust producer');") &&
         memcmp(payload, "console.log('rust producer');", job.payload.length) ==
             0);
  laghu_runtime_queue_close(&queue);
}

static void conformance_c_job_to_rust(const char *worker) {
  char root[LAGHU_RUNTIME_PATH_SIZE], queue_path[LAGHU_RUNTIME_PATH_SIZE];
  char cache_path[LAGHU_RUNTIME_PATH_SIZE];
  char index[LAGHU_RUNTIME_KEY_SIZE], validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  char policy[LAGHU_RUNTIME_KEY_SIZE];
  unsigned char source[] =
      "function publicName(longLocal){const repeatedValue=longLocal+1;return "
      "repeatedValue+repeatedValue+repeatedValue;}";
  unsigned char *result;
  const size_t result_capacity = 2U * 1024U * 1024U;
  laghu_runtime_queue queue;
  laghu_runtime_job job;
  laghu_runtime_cache_entry entry;
  assert(worker != NULL && worker[0] != '\0');
  result = malloc(result_capacity);
  assert(result != NULL);
  assert(laghu_test_directory(root, sizeof(root)));
  assert(conformance_path(queue_path, sizeof(queue_path), root, "jobs.queue"));
  assert(conformance_path(cache_path, sizeof(cache_path), root, "cache"));
  conformance_hash(index, 'a');
  conformance_hash(validator, 'b');
  conformance_hash(policy, 'c');
  memset(&job, 0, sizeof(job));
  job.kind = LAGHU_RUNTIME_JOB_JAVASCRIPT;
  memcpy(job.index_key, index, sizeof(job.index_key));
  memcpy(job.policy_key, policy, sizeof(job.policy_key));
  strcpy(job.request_path, "/cross-language.js");
  strcpy(job.validator, validator);
  strcpy(job.content_type, "application/javascript");
  strcpy(job.javascript_target, "last 2 chrome versions");
  job.payload = (laghu_buffer){source, sizeof(source) - 1U};
  laghu_runtime_queue_init(&queue);
  assert(laghu_runtime_queue_create(&queue, queue_path, 2U, result_capacity));
  assert(laghu_runtime_queue_try_publish(&queue, &job));
  conformance_run_worker_once(worker, queue_path, cache_path);
  assert(laghu_runtime_cache_lookup(cache_path, index, validator, &entry));
  assert(strcmp(entry.content_type, "application/javascript") == 0 &&
         entry.length < sizeof(source) - 1U);
  assert(laghu_runtime_cache_read(&entry, result, result_capacity));
  assert(conformance_contains(result, entry.length, "publicName"));
  laghu_runtime_queue_close(&queue);
  free(result);
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--consume-rust-job") == 0) {
    conformance_consume_rust_job(argv[2]);
    return 0;
  }
  if (argc == 4 && strcmp(argv[1], "--registry-peer") == 0) {
    conformance_registry_peer(argv[2], argv[3]);
    return 0;
  }
  assert(argc == 2);
  conformance_c_job_to_rust(argv[1]);
  return 0;
}
