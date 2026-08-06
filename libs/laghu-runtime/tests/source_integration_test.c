// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "laghu/source.h"
#include "test_fixture.h"

static void test_file_mapping(const char *directory) {
  laghu_source_policy *source = malloc(sizeof(*source));
  laghu_source_policy *loaded = malloc(sizeof(*loaded));
  unsigned char *body = NULL;
  size_t length = 0U;
  char type[LAGHU_RUNTIME_TYPE_SIZE], validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
  char mapping[LAGHU_RUNTIME_KEY_SIZE], path[LAGHU_RUNTIME_PATH_SIZE];
  char registry[LAGHU_RUNTIME_PATH_SIZE];
  FILE *file;
  assert(source != NULL && loaded != NULL);
  laghu_source_policy_init(source);
  assert(laghu_source_mode_parse("mapped", false, &source->mode));
  assert(!laghu_source_mode_parse("native", false, &source->mode));
  source->mode = LAGHU_SOURCE_FILE_MAPPED;
  assert(laghu_source_mapping_add(source, "https://origin.example.test/assets/",
                                  directory));
  assert(!laghu_source_mapping_add(
      source, "https://origin.example.test/assets/", directory));
  assert(laghu_source_policy_validate(source, false, NULL, 0U));
  assert(snprintf(path, sizeof(path), "%s/source.png", directory) > 0);
  file = fopen(path, "wb");
  assert(file != NULL);
  assert(fwrite("png-body", sizeof("png-body") - 1U, 1U, file) == 1U);
  assert(fclose(file) == 0);
  assert(snprintf(registry, sizeof(registry), "%s/source.queue", directory) >
         0);
  assert(laghu_source_registry_publish(registry, source));
  assert(laghu_source_registry_load(registry, loaded));
  assert(laghu_source_file_load(
             loaded, "https://origin.example.test/assets/source.png", &body,
             &length, type, validator, mapping) == LAGHU_SOURCE_LOAD_READY);
  assert(length == sizeof("png-body") - 1U &&
         memcmp(body, "png-body", length) == 0 &&
         strcmp(type, "image/png") == 0 && validator[0] != '\0' &&
         mapping[0] != '\0');
  free(body);
  assert(laghu_source_file_load(
             loaded, "https://origin.example.test/assets/../source.png", &body,
             &length, type, validator, mapping) == LAGHU_SOURCE_LOAD_UNSAFE);
  {
    char link[LAGHU_RUNTIME_PATH_SIZE];
    assert(snprintf(link, sizeof(link), "%s/link.png", directory) > 0);
    assert(symlink(path, link) == 0);
    assert(laghu_source_file_load(
               loaded, "https://origin.example.test/assets/link.png", &body,
               &length, type, validator, mapping) == LAGHU_SOURCE_LOAD_UNSAFE);
  }
  free(loaded);
  free(source);
}

static void test_public_addresses(void) {
  struct sockaddr_in address4;
  struct sockaddr_in6 address6;
  memset(&address4, 0, sizeof(address4));
  address4.sin_family = AF_INET;
  assert(inet_pton(AF_INET, "10.1.2.3", &address4.sin_addr) == 1);
  assert(!laghu_source_address_public((struct sockaddr *)&address4));
  assert(inet_pton(AF_INET, "203.0.113.1", &address4.sin_addr) == 1);
  assert(!laghu_source_address_public((struct sockaddr *)&address4));
  assert(inet_pton(AF_INET, "8.8.8.8", &address4.sin_addr) == 1);
  assert(laghu_source_address_public((struct sockaddr *)&address4));
  memset(&address6, 0, sizeof(address6));
  address6.sin6_family = AF_INET6;
  assert(inet_pton(AF_INET6, "::ffff:127.0.0.1", &address6.sin6_addr) == 1);
  assert(!laghu_source_address_public((struct sockaddr *)&address6));
  assert(inet_pton(AF_INET6, "2001:db8::1", &address6.sin6_addr) == 1);
  assert(!laghu_source_address_public((struct sockaddr *)&address6));
  assert(inet_pton(AF_INET6, "2606:4700:4700::1111", &address6.sin6_addr) == 1);
  assert(laghu_source_address_public((struct sockaddr *)&address6));
}

int main(void) {
  char directory[LAGHU_RUNTIME_PATH_SIZE];
  assert(laghu_test_directory(directory, sizeof(directory)));
  test_file_mapping(directory);
  test_public_addresses();
  puts("laghu_runtime_source_integration_test: all tests passed");
  return 0;
}
