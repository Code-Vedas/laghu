// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/cache.h"
#include "laghu/catalog.h"
#include "laghu/rum.h"
#include "laghu/types.h"
#include "test_fixture.h"

static void test_cache_backend_uri(const laghu_test_workspace *workspace) {
  laghu_cache_backend *backend = calloc(1U, sizeof(*backend));
  laghu_cache_limits limits;
  char uri[LAGHU_RUNTIME_PATH_SIZE];
  char parsed[LAGHU_RUNTIME_PATH_SIZE];

  assert(backend != NULL);
  assert(laghu_test_cache_uri(workspace->path, uri, sizeof(uri)));
  assert(laghu_cache_backend_uri_parse(uri, parsed, sizeof(parsed)));
  assert(strcmp(parsed, workspace->path) == 0);
  assert(!laghu_cache_backend_uri_parse("memcached://127.0.0.1", parsed,
                                        sizeof(parsed)));
  assert(!laghu_cache_backend_uri_parse("file://server/cache", parsed,
                                        sizeof(parsed)));
  assert(!laghu_cache_backend_uri_parse("file:///tmp/cache?secret=x", parsed,
                                        sizeof(parsed)));
  assert(!laghu_cache_backend_uri_parse("file:///tmp/%2fcache", parsed,
                                        sizeof(parsed)));
  laghu_cache_limits_init(&limits);
  assert(laghu_cache_backend_open(backend, uri, &limits));
  assert(strcmp(backend->path, workspace->path) == 0);
  laghu_cache_backend_close(backend);
  assert(laghu_cache_backend_open_path(backend, workspace->path, &limits));
  laghu_cache_backend_close(backend);
  free(backend);
}

static void test_cache_backend_governance(const laghu_test_workspace *workspace,
                                          const unsigned char *payload,
                                          size_t payload_length,
                                          const char *index_key,
                                          const char *no_webp_index_key,
                                          const char *policy_key) {
  laghu_cache_backend *backend = calloc(1U, sizeof(*backend));
  laghu_cache_limits limits;
  laghu_cache_stats stats;
  laghu_runtime_cache_entry backend_entry;
  char backend_path[LAGHU_RUNTIME_PATH_SIZE];
  char placeholder[LAGHU_RUNTIME_PATH_SIZE];

  laghu_cache_limits_init(&limits);
  limits.metadata_size = 16384U;
  limits.size_limit = payload_length;
  limits.inode_limit = 16U;
  assert(laghu_test_workspace_path(workspace, "backend-cache", backend_path,
                                   sizeof(backend_path)));
  assert(laghu_test_workspace_path(workspace, "backend-cache/.keep",
                                   placeholder, sizeof(placeholder)));
  assert(
      laghu_test_workspace_write(workspace, "backend-cache/.keep", NULL, 0U));
  assert(remove(placeholder) == 0);
  assert(backend != NULL);
  assert(laghu_cache_backend_open_path(backend, backend_path, &limits));
  assert(laghu_cache_backend_publish(
      backend, index_key, policy_key, "etag", "text/plain", "test",
      (laghu_buffer){payload, payload_length}, &backend_entry));
  assert(
      laghu_cache_backend_lookup(backend, index_key, "etag", &backend_entry));
  {
    unsigned char cached[64];
    assert(laghu_cache_backend_read(backend, &backend_entry, cached,
                                    sizeof(cached)));
  }
  assert(laghu_cache_backend_health(backend, &stats));
  assert(stats.bytes == payload_length && stats.files == 3U &&
         stats.hits == 1U && stats.publications == 1U &&
         stats.variant_occupancy == 1U && stats.cache_generation == 1U);
  {
    char metadata_path[LAGHU_RUNTIME_PATH_SIZE];
    unsigned char *metadata = malloc(16384U);
    FILE *file;
    size_t length;
    size_t cursor;
    bool payload_found = false;
    assert(snprintf(metadata_path, sizeof(metadata_path), "%s.laghu-metadata",
                    backend_path) > 0);
    file = fopen(metadata_path, "rb");
    assert(file != NULL && metadata != NULL);
    length = fread(metadata, 1U, 16384U, file);
    assert(fclose(file) == 0 && length == 16384U);
    for (cursor = 0U; cursor + payload_length <= length; ++cursor)
      if (memcmp(metadata + cursor, payload, payload_length) == 0)
        payload_found = true;
    assert(!payload_found);
    free(metadata);
  }
  assert(!laghu_cache_backend_publish(
      backend, no_webp_index_key, no_webp_index_key, "etag-2", "text/plain",
      "test", (laghu_buffer){payload, payload_length}, &backend_entry));
  assert(laghu_cache_backend_health(backend, &stats));
  assert(stats.rejected_publications == 1U);
  assert(laghu_cache_backend_maintain(backend, 100U));
  assert(
      !laghu_cache_backend_lookup(backend, index_key, "etag", &backend_entry));
  assert(laghu_cache_backend_health(backend, &stats));
  assert(stats.bytes == 0U && stats.files == 0U && stats.evictions == 1U &&
         stats.variant_occupancy == 0U);
  {
    char normalized[LAGHU_RUNTIME_PATH_SIZE];
    char source_hash[LAGHU_RUNTIME_KEY_SIZE];
    bool control = false;
    uint64_t matched = 0U, generation = 0U;
    assert(laghu_cache_source_normalize("/image.png?v=1&laghu=purge#ignored",
                                        normalized, sizeof(normalized),
                                        &control));
    assert(control && strcmp(normalized, "/image.png?v=1") == 0);
    assert(!laghu_cache_source_normalize("/image.png?laghu=purge&laghu=purge",
                                         normalized, sizeof(normalized),
                                         &control));
    assert(laghu_cache_source_hash("/image.png?v=1", source_hash));
    assert(laghu_cache_backend_associate(backend, index_key, source_hash));
    assert(laghu_cache_backend_publish(
        backend, index_key, policy_key, "etag", "text/plain", "test",
        (laghu_buffer){payload, payload_length}, &backend_entry));
    assert(laghu_cache_backend_purge_url(backend, "/image.png?v=1", 200U,
                                         &matched) ==
           LAGHU_CACHE_PURGE_ACCEPTED);
    assert(matched == 1U);
    assert(!laghu_cache_backend_lookup(backend, index_key, "etag",
                                       &backend_entry));
    assert(laghu_cache_backend_flush(backend, 7U, 201U, &generation));
    assert(generation == 7U);
    assert(!laghu_cache_backend_flush(backend, 6U, 202U, &generation));
    assert(laghu_cache_backend_health(backend, &stats));
    assert(stats.url_purges == 1U && stats.full_purges == 1U &&
           stats.invalidated_artifacts >= 1U && stats.cache_generation == 7U);
  }
  {
    char metadata_path[LAGHU_RUNTIME_PATH_SIZE];
    uint64_t zero = 0U;
    FILE *file;
    assert(snprintf(metadata_path, sizeof(metadata_path), "%s.laghu-metadata",
                    backend_path) > 0);
    file = fopen(metadata_path, "r+b");
    assert(file != NULL && fwrite(&zero, sizeof(zero), 1U, file) == 1U);
    assert(fclose(file) == 0);
    assert(laghu_cache_backend_health(backend, &stats));
    assert(stats.corrupt_removals == 1U && stats.rebuilding == false);
  }
  laghu_cache_backend_close(backend);
  ++limits.inode_limit;
  assert(!laghu_cache_backend_open_path(backend, backend_path, &limits));
  free(backend);
}

static void test_catalog_learning(const char *temporary, const char *index_key,
                                  const char *policy_key) {
  laghu_rum_options rum_options;
  laghu_rum_engine *rum_engine;
  laghu_catalog_record catalog = {0};
  laghu_catalog_record loaded;
  char catalog_key[LAGHU_RUNTIME_KEY_SIZE];

  laghu_rum_options_init(&rum_options);
  rum_engine = laghu_rum_engine_create(&rum_options, NULL, 0U);
  assert(rum_engine != NULL);
  assert(laghu_catalog_key("/image.png", index_key, policy_key, 0x55aaU,
                           catalog_key));
  catalog.version = LAGHU_CATALOG_VERSION;
  strcpy(catalog.normalized_url, "/image.png");
  strcpy(catalog.source_hash, index_key);
  strcpy(catalog.policy_key, policy_key);
  catalog.capability_mask = 0x55aaU;
  catalog.natural_width = 800U;
  catalog.natural_height = 600U;
  catalog.variant_count = 2U;
  catalog.variants[0].width = 320U;
  catalog.variants[0].ready = true;
  catalog.updated_at = 1000U;
  catalog.last_accessed_at = 1000U;
  assert(laghu_catalog_publish(temporary, catalog_key, &catalog));
  assert(laghu_catalog_lookup(temporary, catalog_key, 1001U, 604800U, &loaded));
  assert(loaded.natural_width == 800U && loaded.variant_count == 2U);
  {
    static const unsigned char json[] =
        "{\"url\":\"/image.png\",\"width\":240,\"height\":180,"
        "\"viewport_width\":390,\"dpr_hundredths\":200,"
        "\"above_fold\":true,\"mobile\":true}";
    laghu_image_beacon_record beacon;
    assert(laghu_runtime_parse_image_beacon(
        (laghu_buffer){json, sizeof(json) - 1U}, &beacon));
    assert(beacon.mobile && beacon.above_fold && beacon.width == 240U);
    assert(laghu_catalog_apply_beacon(rum_engine, temporary, policy_key,
                                      0x55aaU, 1002U, 604800U, &beacon));
    assert(laghu_catalog_lookup_url(temporary, "/image.png", policy_key,
                                    0x55aaU, 1003U, 604800U, &loaded));
    {
      char identity[LAGHU_RUNTIME_KEY_SIZE];
      laghu_rum_image_record learning;
      laghu_rum_value value;
      assert(laghu_catalog_url_identity("/image.png", policy_key, 0x55aaU,
                                        identity));
      assert(laghu_rum_engine_read(rum_engine, LAGHU_RUM_RECORD_IMAGE, identity,
                                   1003U, &learning, sizeof(learning), &value));
      assert(value.length == sizeof(learning));
      assert(learning.mobile_width == 240U && learning.above_fold);
    }
    {
      static const unsigned char invalid[] = "{\"url\":\"https://x\"}";
      assert(!laghu_runtime_parse_image_beacon(
          (laghu_buffer){invalid, sizeof(invalid) - 1U}, &beacon));
    }
  }
  assert(
      !laghu_catalog_lookup(temporary, catalog_key, 700000U, 604800U, &loaded));
  {
    char lock_path[LAGHU_RUNTIME_PATH_SIZE];
    FILE *lock;
    memset(&catalog, 0, sizeof(catalog));
    catalog.version = LAGHU_CATALOG_VERSION;
    strcpy(catalog.normalized_url, "/locked.png");
    strcpy(catalog.source_hash, index_key);
    strcpy(catalog.policy_key, policy_key);
    catalog.capability_mask = 0x55aaU;
    catalog.updated_at = 800000U;
    catalog.last_accessed_at = 800000U;
    assert(laghu_catalog_key(catalog.normalized_url, catalog.source_hash,
                             catalog.policy_key, catalog.capability_mask,
                             catalog_key));
    assert(snprintf(lock_path, sizeof(lock_path), "%s/catalog/%s.meta.lock",
                    temporary, catalog_key) > 0);
    lock = fopen(lock_path, "wb");
    assert(lock != NULL && fclose(lock) == 0);
    assert(!laghu_catalog_publish(temporary, catalog_key, &catalog));
    assert(remove(lock_path) == 0);
    assert(laghu_catalog_publish(temporary, catalog_key, &catalog));

    strcpy(catalog.normalized_url, "/newest.png");
    catalog.updated_at = 800100U;
    catalog.last_accessed_at = 800100U;
    assert(laghu_catalog_publish_url(temporary, &catalog));
    assert(laghu_catalog_prune(temporary, 800101U, 1U, 604800U));
    assert(!laghu_catalog_lookup_url(temporary, "/locked.png", policy_key,
                                     0x55aaU, 800101U, 604800U, &loaded));
    assert(laghu_catalog_lookup_url(temporary, "/newest.png", policy_key,
                                    0x55aaU, 800101U, 604800U, &loaded));
  }
  laghu_rum_engine_destroy(rum_engine);
}

static void test_file_cache_artifacts(const char *temporary,
                                      const unsigned char *payload,
                                      size_t payload_length,
                                      const char *index_key,
                                      const char *policy_key) {
  laghu_runtime_cache_entry entry;
  unsigned char cached[64];

  assert(laghu_runtime_cache_publish(
      temporary, index_key, policy_key, "etag", "image/png", "test-backend",
      (laghu_buffer){payload, payload_length}, &entry));
  assert(!laghu_runtime_cache_publish(
      NULL, index_key, policy_key, "etag", "image/png", "test-backend",
      (laghu_buffer){payload, payload_length}, &entry));
  assert(!laghu_runtime_cache_lookup(temporary, index_key, NULL, &entry));
  assert(laghu_runtime_cache_lookup(temporary, index_key, "etag", &entry));
  assert(laghu_runtime_cache_lookup_variant(temporary, policy_key, &entry));
  assert(laghu_runtime_cache_read(&entry, cached, sizeof(cached)));
  assert(memcmp(cached, payload, payload_length) == 0);
  assert(!laghu_runtime_cache_lookup(temporary, index_key, "changed", &entry));
  {
    unsigned char corrupted = (unsigned char)(payload[0] ^ 0xffU);
    FILE *file = fopen(entry.variant_path, "r+b");
    assert(file != NULL);
    assert(fwrite(&corrupted, 1U, 1U, file) == 1U);
    assert(fclose(file) == 0);
  }
  assert(laghu_runtime_cache_lookup(temporary, index_key, "etag", &entry));
  assert(!laghu_runtime_cache_read(&entry, cached, sizeof(cached)));
  {
    FILE *file = fopen(entry.variant_path, "wb");
    assert(file != NULL);
    assert(fwrite(payload, 1U, 1U, file) == 1U);
    assert(fclose(file) == 0);
  }
  assert(!laghu_runtime_cache_lookup(temporary, index_key, "etag", &entry));
}

int main(void) {
  static const unsigned char payload[] = "runtime payload";
  laghu_test_workspace workspace;
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char no_webp_index_key[LAGHU_RUNTIME_KEY_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];

  assert(laghu_test_workspace_create(&workspace));
  assert(laghu_sha256_hex((laghu_buffer){payload, sizeof(payload) - 1U},
                          policy_key));
  assert(laghu_runtime_index_key("/image.png", "etag", policy_key, true, false,
                                 index_key));
  assert(laghu_runtime_index_key("/image.png", "etag", policy_key, false, false,
                                 no_webp_index_key));
  assert(strcmp(index_key, no_webp_index_key) != 0);
  test_cache_backend_uri(&workspace);
  test_cache_backend_governance(&workspace, payload, sizeof(payload) - 1U,
                                index_key, no_webp_index_key, policy_key);
  test_catalog_learning(workspace.path, index_key, policy_key);
  test_file_cache_artifacts(workspace.path, payload, sizeof(payload) - 1U,
                            index_key, policy_key);
  assert(laghu_test_workspace_remove(&workspace));
  puts("laghu_runtime_cache_catalog_integration_test: all tests passed");
  return 0;
}
