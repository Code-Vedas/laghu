// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/queue.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/persisted_state_wire.h"
#include "../src/runtime_platform.h"
#include "test_fixture.h"

static void laghu_test_hash(char output[LAGHU_RUNTIME_KEY_SIZE], char value) {
  memset(output, value, LAGHU_RUNTIME_KEY_SIZE - 1U);
  output[LAGHU_RUNTIME_KEY_SIZE - 1U] = '\0';
}

static laghu_runtime_job laghu_test_job(unsigned char *payload, size_t payload_length) {
  laghu_runtime_job job;
  memset(&job, 0, sizeof(job));
  job.kind = LAGHU_RUNTIME_JOB_JAVASCRIPT;
  laghu_test_hash(job.index_key, 'a');
  laghu_test_hash(job.policy_key, 'b');
  strcpy(job.request_path, "/app.js");
  strcpy(job.content_type, "application/javascript");
  strcpy(job.javascript_target, "es2020");
  job.payload = (laghu_buffer){payload, payload_length};
  return job;
}

int main(void) {
  char root[LAGHU_RUNTIME_PATH_SIZE], path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_runtime_queue queue, reader, contended, move_source, move_destination, move_second_source;
  laghu_runtime_queue_snapshot snapshot;
  laghu_runtime_shared_mapping lock;
  laghu_runtime_job job, taken;
  unsigned char payload[] = "console.log(1)";
  unsigned char output[64U];
  size_t mapping_size = LAGHU_WIRE_QUEUE_HEADER_SIZE + 2U * (LAGHU_WIRE_QUEUE_SLOT_HEADER_SIZE + 64U);
  FILE *file;

  assert(laghu_test_directory(root, sizeof(root)));
  assert(snprintf(path, sizeof(path), "%s/jobs.queue", root) > 0);
  laghu_runtime_queue_init(&queue);
  laghu_runtime_queue_init(&reader);
  laghu_runtime_queue_init(&contended);
  laghu_runtime_queue_init(&move_source);
  laghu_runtime_queue_init(&move_destination);
  laghu_runtime_queue_init(&move_second_source);
  assert(laghu_runtime_queue_create(&queue, path, 2U, 64U));
  assert(laghu_runtime_queue_snapshot_get(&queue, &snapshot));
  assert(snapshot.capacity == 2U && snapshot.occupied == 0U && snapshot.payload_capacity == 64U);
  assert(laghu_runtime_queue_set_backend(&queue, 7U, "test-worker"));
  assert(laghu_runtime_queue_heartbeat(&queue, 123456U));
  assert(laghu_runtime_queue_snapshot_get(&queue, &snapshot));
  assert(snapshot.capabilities == 7U && snapshot.worker_heartbeat == 123456U && strcmp(snapshot.backend_id, "test-worker") == 0);
  assert(laghu_runtime_queue_open(&reader, path));
  assert(laghu_runtime_queue_snapshot_get(&reader, &snapshot));
  assert(snapshot.worker_heartbeat == 123456U);
  assert(laghu_runtime_queue_heartbeat(&queue, 123457U));
  assert(laghu_runtime_queue_refresh(&reader));
  assert(laghu_runtime_queue_snapshot_get(&reader, &snapshot));
  assert(snapshot.worker_heartbeat == 123457U);

  job = laghu_test_job(payload, sizeof(payload) - 1U);
  job.accept_avif = true;
  job.accept_jxl = true;
  job.save_data = true;
  job.index_key_content_classified = true;
  assert(laghu_trace_context_parse("00-0123456789abcdef0123456789abcdef-0123456789abcdef-01", NULL, &job.trace));
  assert(laghu_runtime_queue_try_publish(&queue, &job));
  assert(laghu_runtime_queue_try_publish(&queue, &job));
  assert(!laghu_runtime_queue_try_publish(&queue, &job));
  assert(laghu_runtime_queue_snapshot_get(&queue, &snapshot));
  assert(snapshot.occupied == 2U);
  assert(laghu_runtime_queue_try_take(&reader, &taken, output, sizeof(output)));
  assert(taken.kind == LAGHU_RUNTIME_JOB_JAVASCRIPT && taken.payload.length == sizeof(payload) - 1U && taken.accept_avif && taken.accept_jxl &&
         taken.save_data && taken.index_key_content_classified && memcmp(output, payload, taken.payload.length) == 0 &&
         strcmp(taken.trace.trace_id, job.trace.trace_id) == 0 && taken.trace.sampled);
  assert(laghu_runtime_queue_try_take(&reader, &taken, output, sizeof(output)));
  memset(&job, 0, sizeof(job));
  job.kind = LAGHU_RUNTIME_JOB_SPRITE;
  laghu_test_hash(job.index_key, 'a');
  laghu_test_hash(job.policy_key, 'b');
  job.sprite_count = 2U;
  laghu_test_hash(job.sprite_variant_keys[0], 'a');
  laghu_test_hash(job.sprite_variant_keys[1], 'c');
  job.sprite_width[0] = 20U;
  job.sprite_width[1] = 30U;
  assert(laghu_runtime_queue_try_publish(&queue, &job));
  assert(laghu_runtime_queue_try_take(&reader, &taken, output, sizeof(output)));
  assert(taken.kind == LAGHU_RUNTIME_JOB_SPRITE && taken.sprite_count == 2U && taken.payload.length == 0U && taken.sprite_width[1] == 30U &&
         strcmp(taken.sprite_variant_keys[1], job.sprite_variant_keys[1]) == 0);
  job.sprite_count = 1U;
  assert(!laghu_runtime_queue_try_publish(&queue, &job));
  job.sprite_count = 2U;
  strcpy(job.sprite_variant_keys[1], "not-a-content-key");
  assert(!laghu_runtime_queue_try_publish(&queue, &job));

  memset(&job, 0, sizeof(job));
  job.kind = LAGHU_RUNTIME_JOB_BROWSER_ANALYSIS;
  laghu_test_hash(job.index_key, 'a');
  laghu_test_hash(job.policy_key, 'b');
  strcpy(job.request_path, "/fixture.html");
  strcpy(job.content_type, "text/html");
  job.analysis_timeout_ms = 1500U;
  job.payload = (laghu_buffer){payload, sizeof(payload) - 1U};
  assert(laghu_runtime_queue_try_publish(&queue, &job));
  assert(laghu_runtime_queue_try_take(&reader, &taken, output, sizeof(output)));
  assert(taken.kind == LAGHU_RUNTIME_JOB_BROWSER_ANALYSIS && taken.analysis_timeout_ms == 1500U);
  job.analysis_timeout_ms = 99U;
  assert(!laghu_runtime_queue_try_publish(&queue, &job));

  job = laghu_test_job(payload, sizeof(payload) - 1U);
  laghu_runtime_shared_mapping_init(&lock);
  assert(laghu_runtime_shared_mapping_open(&lock, path, mapping_size));
  assert(laghu_runtime_shared_mapping_try_lock(&lock));
  assert(laghu_runtime_queue_open(&contended, path));
  assert(!laghu_runtime_queue_heartbeat(&queue, 123458U));
  assert(!laghu_runtime_queue_try_publish(&queue, &job));
  laghu_runtime_shared_mapping_unlock(&lock);
  assert(laghu_runtime_queue_snapshot_get(&contended, &snapshot));
  laghu_runtime_queue_close(&contended);
  laghu_runtime_shared_mapping_close(&lock);
  assert(laghu_runtime_queue_heartbeat(&queue, 123458U));
  assert(laghu_runtime_queue_try_publish(&queue, &job));
  laghu_runtime_queue_close(&reader);
  laghu_runtime_queue_close(&queue);

  assert(laghu_runtime_queue_open(&move_source, path));
  assert(laghu_runtime_queue_open(&move_second_source, path));
  assert(laghu_runtime_queue_move(&move_destination, &move_source));
  assert(!laghu_runtime_queue_snapshot_get(&move_source, &snapshot));
  assert(!laghu_runtime_queue_move(&move_destination, &move_source));
  assert(!laghu_runtime_queue_move(&move_destination, &move_second_source));
  laghu_runtime_queue_close(&move_source);
  assert(laghu_runtime_queue_snapshot_get(&move_destination, &snapshot));
  laghu_runtime_queue_close(&move_destination);
  laghu_runtime_queue_close(&move_second_source);

  file = fopen(path, "r+b");
  assert(file != NULL && fseek(file, LAGHU_WIRE_QUEUE_HEADER_VERSION_OFFSET, SEEK_SET) == 0 && fputc(0xff, file) != EOF && fclose(file) == 0);
  assert(!laghu_runtime_queue_open(&reader, path));
  assert(reader.implementation == NULL);
  file = fopen(path, "wb");
  assert(file != NULL && fputc(0, file) != EOF && fclose(file) == 0);
  assert(!laghu_runtime_queue_open(&reader, path));
  assert(reader.implementation == NULL);
  return 0;
}
