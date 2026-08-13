// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/operational.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/persisted_state_wire.h"
#include "../src/runtime_platform.h"
#include "test_fixture.h"

enum { TEST_OPERATIONAL_FILE_SIZE = LAGHU_WIRE_OPERATIONAL_HEADER_SIZE + LAGHU_WIRE_OPERATIONAL_SLOT_COUNT * LAGHU_WIRE_OPERATIONAL_SLOT_SIZE };

static bool operational_path(const char *root, char *path, size_t capacity) {
  int written;
  if (root == NULL || path == NULL || capacity == 0U) return false;
  written = snprintf(path, capacity, "%s.laghu-operations-v4", root);
  return written > 0 && (size_t)written < capacity;
}

static unsigned char *operational_slot(laghu_runtime_shared_mapping *mapping, unsigned int index) {
  return (unsigned char *)mapping->mapping + LAGHU_WIRE_OPERATIONAL_HEADER_SIZE + (size_t)index * LAGHU_WIRE_OPERATIONAL_SLOT_SIZE;
}

static unsigned int operational_active_slot(laghu_runtime_shared_mapping *mapping) {
  unsigned int index;
  for (index = 0U; index < LAGHU_WIRE_OPERATIONAL_SLOT_COUNT; ++index)
    if (laghu_wire_u32_read(operational_slot(mapping, index) + LAGHU_WIRE_OPERATIONAL_SLOT_ACTIVE_OFFSET) != 0U) return index;
  return LAGHU_WIRE_OPERATIONAL_SLOT_COUNT;
}

static void test_snapshot_ignores_whole_file_lock(const char *root) {
  char path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_operational_registry registry;
  laghu_operational_snapshot snapshot;
  assert(operational_path(root, path, sizeof(path)));
  laghu_operational_registry_init(&registry);
  assert(laghu_operational_registry_open(&registry, root, LAGHU_OPERATIONAL_SURFACE_STANDALONE, LAGHU_OPERATIONAL_PROCESS_ADAPTER, false, 100U));
  {
    int ready[2], release[2], child_status;
    pid_t child;
    char signal;
    assert(pipe(ready) == 0 && pipe(release) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
      laghu_runtime_shared_mapping mapping;
      laghu_runtime_shared_mapping_init(&mapping);
      if (!laghu_runtime_shared_mapping_open(&mapping, path, TEST_OPERATIONAL_FILE_SIZE) || !laghu_runtime_shared_mapping_try_lock(&mapping))
        _exit(1);
      (void)write(ready[1], "1", 1U);
      (void)read(release[0], &signal, 1U);
      laghu_runtime_shared_mapping_unlock(&mapping);
      laghu_runtime_shared_mapping_close(&mapping);
      _exit(0);
    }
    assert(read(ready[0], &signal, 1U) == 1);
    assert(laghu_operational_registry_snapshot(&registry, &snapshot));
    assert(write(release[1], "1", 1U) == 1);
    assert(waitpid(child, &child_status, 0) == child);
    assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    (void)close(ready[0]);
    (void)close(ready[1]);
    (void)close(release[0]);
    (void)close(release[1]);
  }
  laghu_operational_registry_close(&registry);
}

static void test_invalid_files_fail_open(const char *root) {
  char path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_runtime_shared_mapping mapping;
  laghu_operational_registry registry;
  unsigned char byte = 1U;
  assert(operational_path(root, path, sizeof(path)));
  assert(laghu_runtime_file_remove(path));
  laghu_runtime_shared_mapping_init(&mapping);
  assert(laghu_runtime_shared_mapping_open(&mapping, path, TEST_OPERATIONAL_FILE_SIZE));
  assert(laghu_runtime_shared_mapping_try_lock(&mapping));
  laghu_wire_u64_write((unsigned char *)mapping.mapping + LAGHU_WIRE_OPERATIONAL_HEADER_MAGIC_OFFSET, LAGHU_WIRE_OPERATIONAL_MAGIC);
  laghu_wire_u32_write((unsigned char *)mapping.mapping + LAGHU_WIRE_OPERATIONAL_HEADER_VERSION_OFFSET, LAGHU_WIRE_OPERATIONAL_VERSION + 1U);
  laghu_wire_u32_write((unsigned char *)mapping.mapping + LAGHU_WIRE_OPERATIONAL_HEADER_SLOT_COUNT_OFFSET, LAGHU_WIRE_OPERATIONAL_SLOT_COUNT);
  laghu_runtime_shared_mapping_unlock(&mapping);
  laghu_runtime_shared_mapping_close(&mapping);
  laghu_operational_registry_init(&registry);
  assert(!laghu_operational_registry_open(&registry, root, LAGHU_OPERATIONAL_SURFACE_STANDALONE, LAGHU_OPERATIONAL_PROCESS_ADAPTER, false, 100U));
  assert(laghu_runtime_file_remove(path));
  laghu_runtime_shared_mapping_init(&mapping);
  assert(laghu_runtime_shared_mapping_open(&mapping, path, TEST_OPERATIONAL_FILE_SIZE));
  assert(laghu_runtime_shared_mapping_try_lock(&mapping));
  laghu_wire_u64_write((unsigned char *)mapping.mapping + LAGHU_WIRE_OPERATIONAL_HEADER_MAGIC_OFFSET, LAGHU_WIRE_OPERATIONAL_MAGIC);
  laghu_wire_u32_write((unsigned char *)mapping.mapping + LAGHU_WIRE_OPERATIONAL_HEADER_VERSION_OFFSET, LAGHU_WIRE_OPERATIONAL_VERSION);
  laghu_wire_u32_write((unsigned char *)mapping.mapping + LAGHU_WIRE_OPERATIONAL_HEADER_SLOT_COUNT_OFFSET, LAGHU_WIRE_OPERATIONAL_SLOT_COUNT);
  operational_slot(&mapping, 0U)[LAGHU_WIRE_OPERATIONAL_SLOT_SIZE - 1U] = 1U;
  laghu_runtime_shared_mapping_unlock(&mapping);
  laghu_runtime_shared_mapping_close(&mapping);
  assert(!laghu_operational_registry_open(&registry, root, LAGHU_OPERATIONAL_SURFACE_STANDALONE, LAGHU_OPERATIONAL_PROCESS_ADAPTER, false, 100U));
  assert(laghu_runtime_file_remove(path));
  assert(laghu_runtime_file_write_atomic(path, &byte, sizeof(byte)));
  assert(!laghu_operational_registry_open(&registry, root, LAGHU_OPERATIONAL_SURFACE_STANDALONE, LAGHU_OPERATIONAL_PROCESS_ADAPTER, false, 100U));
  assert(laghu_runtime_file_remove(path));
}

static void test_generation_sequence_and_saturation(const char *root) {
  char path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_runtime_shared_mapping mapping;
  laghu_operational_registry owner, replacement, other;
  laghu_operational_snapshot snapshot;
  unsigned int slot;
  uint64_t generation;
  assert(operational_path(root, path, sizeof(path)));
  laghu_operational_registry_init(&owner);
  assert(laghu_operational_registry_open(&owner, root, LAGHU_OPERATIONAL_SURFACE_STANDALONE, LAGHU_OPERATIONAL_PROCESS_ADAPTER, false, 100U));
  laghu_runtime_shared_mapping_init(&mapping);
  assert(laghu_runtime_shared_mapping_open(&mapping, path, TEST_OPERATIONAL_FILE_SIZE));
  assert(laghu_runtime_shared_mapping_try_lock(&mapping));
  slot = operational_active_slot(&mapping);
  assert(slot < LAGHU_WIRE_OPERATIONAL_SLOT_COUNT);
  generation = laghu_wire_u64_read(operational_slot(&mapping, slot) + LAGHU_WIRE_OPERATIONAL_SLOT_GENERATION_OFFSET);
  assert(!laghu_operational_registry_heartbeat(&owner, 101U, true, 4U, 1U));
  laghu_runtime_shared_mapping_unlock(&mapping);
  laghu_runtime_shared_mapping_close(&mapping);
  laghu_operational_registry_worker_job(&owner, true, UINT64_MAX, LAGHU_OPERATIONAL_FAILURE_WORKER);
  laghu_operational_registry_worker_job(&owner, true, UINT64_MAX, LAGHU_OPERATIONAL_FAILURE_WORKER);
  assert(laghu_operational_registry_snapshot(&owner, &snapshot));
  assert(snapshot.slots[slot].latency_sum_microseconds == UINT64_MAX);
  assert(laghu_operational_registry_open(&replacement, root, LAGHU_OPERATIONAL_SURFACE_WORKER, LAGHU_OPERATIONAL_PROCESS_JAVASCRIPT, true, 200U));
  assert(!laghu_operational_registry_heartbeat(&owner, 200U, true, 1U, 0U));
  laghu_runtime_shared_mapping_init(&mapping);
  assert(laghu_runtime_shared_mapping_open(&mapping, path, TEST_OPERATIONAL_FILE_SIZE));
  slot = operational_active_slot(&mapping);
  assert(slot < LAGHU_WIRE_OPERATIONAL_SLOT_COUNT);
  assert(laghu_wire_u64_read(operational_slot(&mapping, slot) + LAGHU_WIRE_OPERATIONAL_SLOT_GENERATION_OFFSET) != generation);
  laghu_runtime_shared_mapping_close(&mapping);
  laghu_operational_registry_close(&owner);
  assert(laghu_operational_registry_snapshot(&replacement, &snapshot));
  assert(snapshot.slots[slot].active != 0U);
  laghu_operational_registry_close(&replacement);

  laghu_operational_registry_init(&owner);
  laghu_operational_registry_init(&other);
  assert(laghu_operational_registry_open(&owner, root, LAGHU_OPERATIONAL_SURFACE_STANDALONE, LAGHU_OPERATIONAL_PROCESS_ADAPTER, false, 300U));
  laghu_runtime_shared_mapping_init(&mapping);
  assert(laghu_runtime_shared_mapping_open(&mapping, path, TEST_OPERATIONAL_FILE_SIZE));
  assert(laghu_runtime_shared_mapping_try_lock(&mapping));
  slot = operational_active_slot(&mapping);
  assert(slot < LAGHU_WIRE_OPERATIONAL_SLOT_COUNT);
  generation = laghu_wire_u64_read(operational_slot(&mapping, slot) + LAGHU_WIRE_OPERATIONAL_SLOT_GENERATION_OFFSET);
  laghu_wire_u64_write(operational_slot(&mapping, slot) + LAGHU_WIRE_OPERATIONAL_SLOT_SEQUENCE_OFFSET, 3U);
  laghu_runtime_shared_mapping_unlock(&mapping);
  assert(!laghu_operational_registry_snapshot(&owner, &snapshot));
  assert(laghu_operational_registry_open(&other, root, LAGHU_OPERATIONAL_SURFACE_WORKER, LAGHU_OPERATIONAL_PROCESS_JAVASCRIPT, true, 400U));
  assert(laghu_wire_u64_read(operational_slot(&mapping, slot) + LAGHU_WIRE_OPERATIONAL_SLOT_GENERATION_OFFSET) == generation);
  laghu_wire_u64_write(operational_slot(&mapping, slot) + LAGHU_WIRE_OPERATIONAL_SLOT_SEQUENCE_OFFSET, 4U);
  laghu_runtime_shared_mapping_close(&mapping);
  laghu_operational_registry_close(&other);
  laghu_operational_registry_close(&owner);
}

int main(void) {
  char root[LAGHU_RUNTIME_PATH_SIZE];
  char operations_path_buffer[LAGHU_RUNTIME_PATH_SIZE];
  char output[LAGHU_OPERATIONAL_RENDER_SIZE];
  uint64_t file_size = 0U;
  size_t length = 0U;
  laghu_operational_registry registry;
  laghu_operational_snapshot snapshot;
  assert(laghu_test_directory(root, sizeof(root)));
  laghu_operational_registry_init(&registry);
  assert(laghu_operational_registry_open(&registry, root, LAGHU_OPERATIONAL_SURFACE_STANDALONE, LAGHU_OPERATIONAL_PROCESS_ADAPTER, false, 100U));
  assert(operational_path(root, operations_path_buffer, sizeof(operations_path_buffer)));
  assert(laghu_runtime_file_size(operations_path_buffer, &file_size));
  assert(file_size == TEST_OPERATIONAL_FILE_SIZE);
  laghu_operational_registry_record(&registry, LAGHU_OPERATIONAL_DECISION_ORIGINAL, 100U, 80U, 5000U);
  assert(laghu_operational_registry_snapshot(&registry, &snapshot));
  assert(laghu_operational_render_prometheus(&snapshot, 100U, output, sizeof(output), &length));
  assert(length != 0U && strstr(output, "laghu_requests_total") != NULL);
  laghu_operational_registry_close(&registry);
  test_invalid_files_fail_open(root);
  test_snapshot_ignores_whole_file_lock(root);
  test_generation_sequence_and_saturation(root);
  return 0;
}
