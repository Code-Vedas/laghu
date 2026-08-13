// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_SOURCE_H
#define LAGHU_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"
#include "laghu/image.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct sockaddr;

#define LAGHU_SOURCE_MAX_MAPPINGS 8U
#define LAGHU_SOURCE_PATH_SIZE 1024U
#define LAGHU_SOURCE_REGISTRY_VERSION 1U

typedef enum { LAGHU_SOURCE_FILE_OFF = 0, LAGHU_SOURCE_FILE_MAPPED, LAGHU_SOURCE_FILE_NATIVE, LAGHU_SOURCE_FILE_BOTH } laghu_source_file_mode;

typedef struct {
  char source_prefix[LAGHU_SOURCE_PATH_SIZE];
  char root[LAGHU_SOURCE_PATH_SIZE];
  char identity[LAGHU_RUNTIME_KEY_SIZE];
} laghu_source_mapping;

typedef struct {
  unsigned int version;
  laghu_source_file_mode mode;
  laghu_source_mapping mappings[LAGHU_SOURCE_MAX_MAPPINGS];
  size_t mapping_count;
  char native_root[LAGHU_SOURCE_PATH_SIZE];
  size_t max_body_bytes;
} laghu_source_policy;

typedef enum {
  LAGHU_SOURCE_LOAD_MISS = 0,
  LAGHU_SOURCE_LOAD_READY,
  LAGHU_SOURCE_LOAD_UNSAFE,
  LAGHU_SOURCE_LOAD_CHANGED,
  LAGHU_SOURCE_LOAD_OVERSIZED,
  LAGHU_SOURCE_LOAD_IO_ERROR
} laghu_source_load_result;

void laghu_source_policy_init(laghu_source_policy *policy);
bool laghu_source_mode_parse(const char *value, bool native_supported, laghu_source_file_mode *mode);
bool laghu_source_mapping_add(laghu_source_policy *policy, const char *source_prefix, const char *root);
bool laghu_source_policy_merge(laghu_source_policy *merged, const laghu_source_policy *parent, const laghu_source_policy *child);
bool laghu_source_policy_validate(const laghu_source_policy *policy, bool native_supported, char *error, size_t error_size);
bool laghu_source_registry_publish(const char *queue_path, const laghu_source_policy *policy);
bool laghu_source_registry_load(const char *queue_path, laghu_source_policy *policy);
laghu_source_load_result laghu_source_file_load(const laghu_source_policy *policy, const char *source_url, unsigned char **body, size_t *body_length,
                                                char content_type[LAGHU_RUNTIME_TYPE_SIZE], char validator[LAGHU_RUNTIME_VALIDATOR_SIZE],
                                                char mapping_identity[LAGHU_RUNTIME_KEY_SIZE]);
bool laghu_source_address_public(const struct sockaddr *address);

#ifdef __cplusplus
}
#endif

#endif
