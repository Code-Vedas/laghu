// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_BASE_H
#define LAGHU_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_SHA256_HEX_LENGTH 64U
#define LAGHU_SHA256_HEX_SIZE (LAGHU_SHA256_HEX_LENGTH + 1U)
#define LAGHU_SHA256_BLOCK_SIZE 64U
#define LAGHU_SHA256_DIGEST_SIZE 32U

typedef struct {
  const unsigned char *data;
  size_t length;
} laghu_buffer;

typedef struct {
  unsigned char *data;
  size_t length;
  size_t capacity;
} laghu_builder;

typedef struct {
  uint32_t state[8];
  uint64_t bit_length;
  unsigned char block[LAGHU_SHA256_BLOCK_SIZE];
  size_t block_length;
} laghu_sha256_context;

typedef enum {
  LAGHU_URL_REJECT_FRAGMENT = UINT32_C(1) << 0,
  LAGHU_URL_REJECT_QUERY = UINT32_C(1) << 1,
  LAGHU_URL_REJECT_API = UINT32_C(1) << 2,
  LAGHU_URL_REJECT_TRAVERSAL = UINT32_C(1) << 3,
  LAGHU_URL_REJECT_UNSAFE_BYTES = UINT32_C(1) << 4
} laghu_url_flags;

unsigned char laghu_base_ascii_lower(unsigned char value);
bool laghu_base_ascii_equal(laghu_buffer value, const char *expected);
bool laghu_base_string_copy(char *output, size_t capacity, const char *value);
bool laghu_base_builder_append(laghu_builder *builder, const void *data,
                               size_t length);
bool laghu_base_parse_u64(const char *value, uint64_t minimum, uint64_t maximum,
                          uint64_t *output);
bool laghu_base_url_resolve_same_origin(const char *page_path,
                                        const char *page_origin,
                                        laghu_buffer value, uint32_t flags,
                                        char *output, size_t capacity);
int laghu_base_hex_value(unsigned char value);
bool laghu_sha256_hex(laghu_buffer input, char output[LAGHU_SHA256_HEX_SIZE]);
void laghu_sha256_init(laghu_sha256_context *context);
void laghu_sha256_update(laghu_sha256_context *context,
                         const unsigned char *data, size_t length);
void laghu_sha256_final(laghu_sha256_context *context,
                        unsigned char digest[LAGHU_SHA256_DIGEST_SIZE]);
void laghu_sha256_final_hex(laghu_sha256_context *context,
                            char output[LAGHU_SHA256_HEX_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
