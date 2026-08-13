// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define main laghu_asset_worker_entry
#include "laghu/assets.h"
#include "laghu/source.h"
#include "laghu/types.h"
#include "main.c"
#undef main

static laghu_asset_provider_result contract_ok_upload(void *context, const char *key, laghu_buffer body, const char *type, const char *checksum) {
  (void)context;
  assert(key != NULL && body.length == sizeof("file-body") - 1U);
  assert(strcmp(type, "image/png") == 0 && checksum[0] != '\0');
  return LAGHU_ASSET_PROVIDER_OK;
}

static laghu_asset_provider_result contract_ok_verify(void *context, const char *key, size_t size, const char *type, const char *checksum) {
  (void)context;
  assert(key != NULL && size == sizeof("file-body") - 1U);
  assert(strcmp(type, "image/png") == 0 && checksum[0] != '\0');
  return LAGHU_ASSET_PROVIDER_OK;
}

int main(void) {
  static const char checksum[] = "0000000000000000000000000000000000000000000000000000000000000000";
  static const char expected[] =
      "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20130524/us-east-1/s3/"
      "aws4_request, SignedHeaders=content-type;host;x-amz-content-sha256;"
      "x-amz-date;x-amz-meta-laghu-sha256, Signature="
      "a8b074c8e8899d5a8fb0369ba896f2380b8c5f613c71895964747f20b81d4611";
  static const char ready[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Type: image/png\r\n"
      "X-Amz-Meta-Laghu-Sha256: "
      "0000000000000000000000000000000000000000000000000000000000000000"
      "\r\n\r\n";
  laghu_s3 s3 = {0};
  char date[17], authorization[1024], next[LAGHU_RUNTIME_PATH_SIZE];
  laghu_asset_policy policy;
  (void)snprintf(s3.host, sizeof(s3.host), "s3.example.com");
  (void)snprintf(s3.config.region, sizeof(s3.config.region), "us-east-1");
  s3.access_key = "AKIDEXAMPLE";
  s3.secret_key = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
  assert(laghu_s3_authorization_at(&s3, "PUT", "/bucket/immutable/a.png", checksum, "image/png", checksum, (time_t)1369353600, date, authorization));
  assert(strcmp(date, "20130524T000000Z") == 0);
  assert(strcmp(authorization, expected) == 0);
  assert(laghu_s3_response_result(ready, strlen(ready), "HEAD", 3U, "image/png", checksum) == LAGHU_ASSET_PROVIDER_OK);
  assert(laghu_s3_response_result("HTTP/1.1 503 Unavailable\r\n\r\n", strlen("HTTP/1.1 503 Unavailable\r\n\r\n"), "PUT", 3U, "image/png", checksum) ==
         LAGHU_ASSET_PROVIDER_RETRYABLE);
  assert(laghu_s3_response_result("HTTP/1.1 403 Forbidden\r\n\r\n", strlen("HTTP/1.1 403 Forbidden\r\n\r\n"), "PUT", 3U, "image/png", checksum) ==
         LAGHU_ASSET_PROVIDER_PERMANENT);
  assert(laghu_s3_response_result("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n"
                                  "Content-Type: image/png\r\n\r\n",
                                  strlen("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n"
                                         "Content-Type: image/png\r\n\r\n"),
                                  "HEAD", 3U, "image/png", checksum) == LAGHU_ASSET_PROVIDER_PERMANENT);
  laghu_asset_policy_init(&policy);
  (void)snprintf(policy.source_domain, sizeof(policy.source_domain), "https://origin.example.com");
  assert(laghu_origin_redirect(&policy, "https://origin.example.com/a", "/b", next));
  assert(strcmp(next, "https://origin.example.com/b") == 0);
  assert(!laghu_origin_redirect(&policy, "https://origin.example.com/a", "//evil.example/b", next));
  assert(!laghu_origin_redirect(&policy, "https://origin.example.com/a", "https://evil.example/b", next));
  assert(!laghu_origin_redirect(&policy, "https://origin.example.com/a", "https://origin.example.com/a", next));
  {
    char root[LAGHU_RUNTIME_PATH_SIZE], queue[LAGHU_RUNTIME_PATH_SIZE];
    char catalog[LAGHU_RUNTIME_PATH_SIZE], source_path[LAGHU_RUNTIME_PATH_SIZE];
    laghu_source_policy source;
    laghu_asset_record pending = {0}, ready_record;
    laghu_asset_provider provider = {contract_ok_upload, contract_ok_verify, NULL, laghu_s3_healthy, &s3};
    FILE *file;
    assert(snprintf(root, sizeof(root), "/tmp/laghu-source-contract-%ld", (long)getpid()) > 0);
    assert(mkdir(root, 0700) == 0 || errno == EEXIST);
    assert(snprintf(queue, sizeof(queue), "%s/queue", root) > 0);
    assert(snprintf(catalog, sizeof(catalog), "%s/catalog", root) > 0);
    assert(snprintf(source_path, sizeof(source_path), "%s/source.png", root) > 0);
    file = fopen(source_path, "wb");
    assert(file != NULL);
    assert(fwrite("file-body", sizeof("file-body") - 1U, 1U, file) == 1U);
    assert(fclose(file) == 0);
    laghu_asset_policy_init(&s3.config.policy);
    (void)snprintf(s3.config.policy.source_domain, sizeof(s3.config.policy.source_domain), "https://origin.example.com");
    (void)snprintf(s3.config.policy.public_domain, sizeof(s3.config.policy.public_domain), "https://cdn.example.com");
    (void)snprintf(s3.config.policy.mime_types, sizeof(s3.config.policy.mime_types), "image/png");
    s3.config.policy.upload = true;
    (void)snprintf(s3.config.queue_path, sizeof(s3.config.queue_path), "%s", queue);
    (void)snprintf(s3.config.catalog_path, sizeof(s3.config.catalog_path), "%s", catalog);
    assert(laghu_sha256_hex((laghu_buffer){(const unsigned char *)"config", sizeof("config") - 1U}, s3.config.digest));
    laghu_source_policy_init(&source);
    source.mode = LAGHU_SOURCE_FILE_MAPPED;
    assert(laghu_source_mapping_add(&source, "https://origin.example.com/assets/", root));
    assert(laghu_source_registry_publish(queue, &source));
    pending.state = LAGHU_ASSET_PENDING;
    (void)snprintf(pending.source_url, sizeof(pending.source_url), "https://origin.example.com/assets/source.png");
    (void)snprintf(pending.policy_digest, sizeof(pending.policy_digest), "%s", s3.config.digest);
    (void)snprintf(pending.provider_digest, sizeof(pending.provider_digest), "%s", s3.config.digest);
    assert(laghu_asset_job_publish(&s3.config, &pending, (laghu_buffer){NULL, 0U}));
    assert(laghu_asset_process(&s3, &provider) == 0);
    assert(laghu_asset_catalog_lookup_url(&s3.config, pending.source_url, &ready_record));
    assert(ready_record.state == LAGHU_ASSET_READY && ready_record.body_length == sizeof("file-body") - 1U &&
           ready_record.source_validator[0] != '\0');
  }
  return 0;
}
