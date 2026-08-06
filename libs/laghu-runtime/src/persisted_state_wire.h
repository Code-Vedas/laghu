// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_PERSISTED_STATE_WIRE_H
#define LAGHU_PERSISTED_STATE_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
#define LAGHU_PERSISTED_U32(name, value) LAGHU_WIRE_##name = (value),
#define LAGHU_PERSISTED_U64(name, value)
#include "../persisted_state_contract.def"
#undef LAGHU_PERSISTED_U32
#undef LAGHU_PERSISTED_U64
};

#define LAGHU_PERSISTED_U32(name, value)
#define LAGHU_PERSISTED_U64(name, value) \
  static const uint64_t LAGHU_WIRE_##name = (value);
#include "../persisted_state_contract.def"
#undef LAGHU_PERSISTED_U32
#undef LAGHU_PERSISTED_U64

static inline uint32_t laghu_wire_u32_read(const unsigned char *input) {
  return (uint32_t)input[0] | ((uint32_t)input[1] << 8U) |
         ((uint32_t)input[2] << 16U) | ((uint32_t)input[3] << 24U);
}

static inline uint64_t laghu_wire_u64_read(const unsigned char *input) {
  uint64_t value = 0U;
  unsigned int index;
  for (index = 0U; index < 8U; ++index)
    value |= (uint64_t)input[index] << (index * 8U);
  return value;
}

static inline void laghu_wire_u32_write(unsigned char *output, uint32_t value) {
  output[0] = (unsigned char)value;
  output[1] = (unsigned char)(value >> 8U);
  output[2] = (unsigned char)(value >> 16U);
  output[3] = (unsigned char)(value >> 24U);
}

static inline void laghu_wire_u64_write(unsigned char *output, uint64_t value) {
  unsigned int index;
  for (index = 0U; index < 8U; ++index)
    output[index] = (unsigned char)(value >> (index * 8U));
}

static inline bool laghu_wire_string_valid(const unsigned char *input,
                                           size_t capacity) {
  return input != NULL && capacity != 0U &&
         memchr(input, '\0', capacity) != NULL;
}

static inline bool laghu_wire_zeroes(const unsigned char *input,
                                     size_t length) {
  size_t index;
  if (input == NULL) return false;
  for (index = 0U; index < length; ++index)
    if (input[index] != 0U) return false;
  return true;
}

#endif
