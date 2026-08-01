// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>
#include <string.h>

#define main laghu_asset_worker_entry
#include "main.c"
#undef main

int main(void) {
  static const char checksum[] =
      "0000000000000000000000000000000000000000000000000000000000000000";
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
  assert(laghu_s3_authorization_at(&s3, "PUT", "/bucket/immutable/a.png",
                                   checksum, "image/png", checksum,
                                   (time_t)1369353600, date, authorization));
  assert(strcmp(date, "20130524T000000Z") == 0);
  assert(strcmp(authorization, expected) == 0);
  assert(laghu_s3_response_result(ready, strlen(ready), "HEAD", 3U, "image/png",
                                  checksum) == LAGHU_ASSET_PROVIDER_OK);
  assert(laghu_s3_response_result("HTTP/1.1 503 Unavailable\r\n\r\n",
                                  strlen("HTTP/1.1 503 Unavailable\r\n\r\n"),
                                  "PUT", 3U, "image/png",
                                  checksum) == LAGHU_ASSET_PROVIDER_RETRYABLE);
  assert(laghu_s3_response_result("HTTP/1.1 403 Forbidden\r\n\r\n",
                                  strlen("HTTP/1.1 403 Forbidden\r\n\r\n"),
                                  "PUT", 3U, "image/png",
                                  checksum) == LAGHU_ASSET_PROVIDER_PERMANENT);
  assert(
      laghu_s3_response_result("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n"
                               "Content-Type: image/png\r\n\r\n",
                               strlen("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n"
                                      "Content-Type: image/png\r\n\r\n"),
                               "HEAD", 3U, "image/png",
                               checksum) == LAGHU_ASSET_PROVIDER_PERMANENT);
  laghu_asset_policy_init(&policy);
  (void)snprintf(policy.source_domain, sizeof(policy.source_domain),
                 "https://origin.example.com");
  assert(laghu_origin_redirect(&policy, "https://origin.example.com/a", "/b",
                               next));
  assert(strcmp(next, "https://origin.example.com/b") == 0);
  assert(!laghu_origin_redirect(&policy, "https://origin.example.com/a",
                                "//evil.example/b", next));
  assert(!laghu_origin_redirect(&policy, "https://origin.example.com/a",
                                "https://evil.example/b", next));
  assert(!laghu_origin_redirect(&policy, "https://origin.example.com/a",
                                "https://origin.example.com/a", next));
  return 0;
}
