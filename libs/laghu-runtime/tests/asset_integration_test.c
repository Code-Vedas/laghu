// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "laghu/assets.h"
#include "laghu/types.h"
#include "test_fixture.h"

static void test_asset_policy(void) {
  laghu_asset_policy asset;
  char rewritten[LAGHU_RUNTIME_PATH_SIZE];
  char object_key[LAGHU_RUNTIME_PATH_SIZE];
  char error[128];

  laghu_asset_policy_init(&asset);
  strcpy(asset.source_domain, "https://origin.example.test");
  strcpy(asset.public_domain, "https://cdn.example.test");
  strcpy(asset.source_prefix, "/assets");
  strcpy(asset.public_prefix, "/immutable");
  strcpy(asset.mime_types, "image/png, application/pdf");
  assert(laghu_asset_policy_validate(&asset, error, sizeof(error)));
  assert(laghu_asset_object_key(&asset, "https://origin.example.test/assets/logo.png?x=1#preview",
                                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", object_key));
  assert(strcmp(object_key,
                "/immutable/"
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789ab"
                "cdef/logo.png") == 0);
  assert(laghu_asset_url_rewrite(&asset, "https://origin.example.test/assets/logo.png?x=1#preview", "image/png",
                                 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", rewritten, sizeof(rewritten)));
  assert(strcmp(rewritten,
                "https://cdn.example.test/immutable/"
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789ab"
                "cdef/logo.png?x=1#preview") == 0);
  {
    laghu_asset_record record = {0};
    strcpy(record.source_url, "https://origin.example.test/assets/logo.png?x=1#preview");
    strcpy(record.content_type, "image/png");
    strcpy(record.content_hash, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    strcpy(record.object_key,
           "/immutable/"
           "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef/"
           "logo.png");
    record.body_length = 1U;
    assert(!laghu_asset_record_rewrite(&asset, &record, rewritten, sizeof(rewritten)));
    record.state = LAGHU_ASSET_READY;
    assert(laghu_asset_record_rewrite(&asset, &record, rewritten, sizeof(rewritten)));
    record.object_key[0] = '\0';
    assert(!laghu_asset_record_rewrite(&asset, &record, rewritten, sizeof(rewritten)));
  }
  asset.preserve_query = false;
  assert(laghu_asset_url_rewrite(&asset, "https://origin.example.test/assets/logo.png?x=1#preview", "image/png",
                                 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", rewritten, sizeof(rewritten)));
  assert(strstr(rewritten, "?x=1") == NULL && strstr(rewritten, "#preview") != NULL);
  strcpy(asset.public_domain, "https://user@cdn.example.test");
  assert(!laghu_asset_policy_validate(&asset, error, sizeof(error)));
}

static void test_asset_config_and_jobs(const laghu_test_workspace *workspace) {
  laghu_asset_config asset_config;
  laghu_asset_record asset_record = {0}, loaded_record = {0}, taken_record;
  char config_path[LAGHU_RUNTIME_PATH_SIZE];
  char catalog_path[LAGHU_RUNTIME_PATH_SIZE];
  char asset_queue_path[LAGHU_RUNTIME_PATH_SIZE];
  char catalog_key[LAGHU_RUNTIME_KEY_SIZE];
  char job_path[LAGHU_RUNTIME_PATH_SIZE];
  char configuration[4096U];
  char error[256];
  unsigned char *taken_body = NULL;
  size_t taken_length = 0U;
  static const unsigned char body[] = "asset-body";
  int length;

  assert(laghu_test_workspace_path(workspace, "assets.conf", config_path, sizeof(config_path)));
  assert(laghu_test_workspace_path(workspace, "asset-catalog", catalog_path, sizeof(catalog_path)));
  assert(laghu_test_workspace_path(workspace, "asset-queue", asset_queue_path, sizeof(asset_queue_path)));
  length = snprintf(configuration, sizeof(configuration),
                    "version=1\nsource_domain=https://origin.example.test\n"
                    "public_domain=https://cdn.example.test\n"
                    "source_prefix=/assets\npublic_prefix=/immutable\n"
                    "mime_types=image/png,application/pdf\n"
                    "mode=rewrite_only\n"
                    "catalog_path=%s\nqueue_path=%s\n"
                    "endpoint=https://s3.example.test\nregion=test-1\n"
                    "bucket=assets\naccess_key_env=TEST_ACCESS_KEY\n"
                    "secret_key_env=TEST_SECRET_KEY\n",
                    catalog_path, asset_queue_path);
  assert(length > 0 && (size_t)length < sizeof(configuration));
  assert(laghu_test_workspace_write(workspace, "assets.conf", (const unsigned char *)configuration, (size_t)length));
  assert(laghu_asset_config_load(config_path, &asset_config, error, sizeof(error)));
  strcpy(asset_record.source_url, "https://origin.example.test/assets/logo.png?version=1");
  strcpy(asset_record.source_validator, "etag-1");
  strcpy(asset_record.content_hash, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  strcpy(asset_record.policy_digest, asset_config.digest);
  strcpy(asset_record.provider_digest, asset_config.digest);
  strcpy(asset_record.content_type, "image/png");
  assert(laghu_asset_object_key(&asset_config.policy, asset_record.source_url, asset_record.content_hash, asset_record.object_key));
  asset_record.body_length = sizeof(body) - 1U;
  asset_record.state = LAGHU_ASSET_PENDING;
  assert(laghu_asset_catalog_key(&asset_record, catalog_key));
  assert(laghu_asset_catalog_publish(catalog_path, &asset_record));
  assert(laghu_asset_catalog_lookup(catalog_path, catalog_key, &loaded_record));
  assert(strcmp(loaded_record.source_url, asset_record.source_url) == 0);
  asset_record.state = LAGHU_ASSET_READY;
  asset_record.updated_at = (uint64_t)time(NULL);
  assert(laghu_asset_catalog_publish(catalog_path, &asset_record));
  {
    static const unsigned char document[] = "<img src=\"https://origin.example.test/assets/logo.png?version=1\">";
    unsigned char *rewritten_document = NULL;
    size_t rewritten_length = 0U;
    assert(laghu_asset_rewrite_document(&asset_config, (laghu_buffer){document, sizeof(document) - 1U}, &rewritten_document, &rewritten_length));
    assert(rewritten_document != NULL && strstr((const char *)rewritten_document, "https://cdn.example.test/immutable/") != NULL);
    free(rewritten_document);
    rewritten_document = NULL;
    {
      static const unsigned char root_document[] = "body{background:url('/assets/logo.png?version=1')}";
      assert(laghu_asset_rewrite_document_at(&asset_config, (laghu_buffer){root_document, sizeof(root_document) - 1U}, "/styles/site.css",
                                             &rewritten_document, &rewritten_length));
      assert(rewritten_document != NULL && strstr((const char *)rewritten_document, "https://cdn.example.test/immutable/") != NULL);
      free(rewritten_document);
    }
  }
  asset_record.state = LAGHU_ASSET_PENDING;
  assert(laghu_asset_job_publish(&asset_config, &asset_record, (laghu_buffer){body, sizeof(body) - 1U}));
  assert(laghu_asset_job_take(&asset_config, &taken_record, &taken_body, &taken_length, job_path));
  assert(taken_length == sizeof(body) - 1U && memcmp(taken_body, body, taken_length) == 0);
  free(taken_body);
  assert(laghu_asset_job_complete(job_path));
  memset(&taken_record, 0, sizeof(taken_record));
  memset(&asset_record, 0, sizeof(asset_record));
  strcpy(asset_record.source_url, "https://origin.example.test/assets/fallback.png");
  strcpy(asset_record.policy_digest, asset_config.digest);
  strcpy(asset_record.provider_digest, asset_config.digest);
  assert(laghu_asset_job_publish(&asset_config, &asset_record, (laghu_buffer){NULL, 0U}));
  assert(laghu_asset_job_take(&asset_config, &taken_record, &taken_body, &taken_length, job_path));
  assert(taken_body == NULL && taken_length == 0U && strcmp(taken_record.source_url, asset_record.source_url) == 0);
  assert(laghu_asset_job_complete(job_path));
  assert(laghu_asset_retry_after(&asset_config.policy, 1U, 100U) == 102U);
}

int main(void) {
  laghu_test_workspace workspace;

  assert(laghu_test_workspace_create(&workspace));
  test_asset_policy();
  test_asset_config_and_jobs(&workspace);
  assert(laghu_test_workspace_remove(&workspace));
  puts("laghu_runtime_asset_integration_test: all tests passed");
  return 0;
}
