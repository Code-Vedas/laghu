// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/base.h"

#include <limits.h>
#include <string.h>

static const uint32_t laghu_sha256_round_constants[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
    UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
    UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
    UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
    UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
    UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
    UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
    UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
    UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
    UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
    UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
    UINT32_C(0xc67178f2)};

unsigned char laghu_base_ascii_lower(unsigned char value) {
  return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A'))
                                      : value;
}

bool laghu_base_ascii_equal(laghu_buffer value, const char *expected) {
  size_t index;
  if (expected == NULL || strlen(expected) != value.length ||
      (value.data == NULL && value.length != 0U))
    return false;
  for (index = 0U; index < value.length; ++index)
    if (laghu_base_ascii_lower(value.data[index]) !=
        laghu_base_ascii_lower((unsigned char)expected[index]))
      return false;
  return true;
}

bool laghu_base_string_copy(char *output, size_t capacity, const char *value) {
  size_t length;
  if (output == NULL || capacity == 0U || value == NULL) return false;
  length = strlen(value);
  if (length >= capacity) {
    output[0] = '\0';
    return false;
  }
  memcpy(output, value, length + 1U);
  return true;
}

bool laghu_base_builder_append(laghu_builder *builder, const void *data,
                               size_t length) {
  if (builder == NULL || (data == NULL && length != 0U) ||
      builder->length > builder->capacity ||
      length > builder->capacity - builder->length)
    return false;
  if (length != 0U) memcpy(builder->data + builder->length, data, length);
  builder->length += length;
  return true;
}

bool laghu_base_parse_u64(const char *value, uint64_t minimum, uint64_t maximum,
                          uint64_t *output) {
  uint64_t parsed = 0U;
  const unsigned char *cursor = (const unsigned char *)value;
  if (value == NULL || *value == '\0' || output == NULL || minimum > maximum)
    return false;
  while (*cursor != '\0') {
    uint64_t digit;
    if (*cursor < '0' || *cursor > '9') return false;
    digit = (uint64_t)(*cursor++ - '0');
    if (parsed > (UINT64_MAX - digit) / 10U) return false;
    parsed = parsed * 10U + digit;
  }
  if (parsed < minimum || parsed > maximum) return false;
  *output = parsed;
  return true;
}

bool laghu_base_url_resolve_same_origin(const char *page_path,
                                        const char *page_origin,
                                        laghu_buffer value, uint32_t flags,
                                        char *output, size_t capacity) {
  const unsigned char *url = value.data;
  size_t length = value.length;
  size_t origin_length = page_origin == NULL ? 0U : strlen(page_origin);
  size_t prefix = 0U, index;
  const char *base = page_path != NULL && page_path[0] == '/' ? page_path : "/";
  if (output == NULL || capacity == 0U || url == NULL || length == 0U ||
      length >= capacity || url[0] == '#' ||
      (length >= 2U && url[0] == '/' && url[1] == '/'))
    return false;
  if ((length >= 7U && memcmp(url, "http://", 7U) == 0) ||
      (length >= 8U && memcmp(url, "https://", 8U) == 0)) {
    if (origin_length == 0U || length <= origin_length ||
        memcmp(url, page_origin, origin_length) != 0 ||
        url[origin_length] != '/')
      return false;
    url += origin_length;
    length -= origin_length;
  } else if (memchr(url, ':', length) != NULL) {
    return false;
  }
  if (url[0] != '/') {
    const char *slash = strrchr(base, '/');
    prefix = slash == NULL ? 1U : (size_t)(slash - base + 1U);
    if (prefix + length >= capacity) return false;
    if (slash == NULL)
      output[0] = '/';
    else
      memcpy(output, base, prefix);
  }
  memcpy(output + prefix, url, length);
  output[prefix + length] = '\0';
  if (((flags & LAGHU_URL_REJECT_TRAVERSAL) != 0U &&
       strstr(output, "..") != NULL) ||
      ((flags & LAGHU_URL_REJECT_QUERY) != 0U && strchr(output, '?') != NULL) ||
      ((flags & LAGHU_URL_REJECT_FRAGMENT) != 0U &&
       strchr(output, '#') != NULL) ||
      ((flags & LAGHU_URL_REJECT_API) != 0U &&
       ((strncmp(output, "/api", 4U) == 0 &&
         (output[4] == '\0' || output[4] == '/')) ||
        (strncmp(output, "/graphql", 8U) == 0 &&
         (output[8] == '\0' || output[8] == '/')))))
    return false;
  if ((flags & LAGHU_URL_REJECT_UNSAFE_BYTES) != 0U)
    for (index = 0U; output[index] != '\0'; ++index)
      if ((unsigned char)output[index] < 0x21U || output[index] == '>' ||
          output[index] == '"' || output[index] == '\\')
        return false;
  return true;
}

int laghu_base_hex_value(unsigned char value) {
  if (value >= '0' && value <= '9') return value - '0';
  value = laghu_base_ascii_lower(value);
  return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

static uint32_t laghu_rotate_right(uint32_t value, unsigned int count) {
  return (value >> count) | (value << (32U - count));
}

static uint32_t laghu_load_u32_be(const unsigned char *value) {
  return ((uint32_t)value[0] << 24U) | ((uint32_t)value[1] << 16U) |
         ((uint32_t)value[2] << 8U) | (uint32_t)value[3];
}

static void laghu_store_u32_be(unsigned char *output, uint32_t value) {
  output[0] = (unsigned char)(value >> 24U);
  output[1] = (unsigned char)(value >> 16U);
  output[2] = (unsigned char)(value >> 8U);
  output[3] = (unsigned char)value;
}

static void laghu_sha256_transform(laghu_sha256_context *context) {
  uint32_t words[64], a, b, c, d, e, f, g, h;
  size_t index;
  for (index = 0U; index < 16U; ++index)
    words[index] = laghu_load_u32_be(&context->block[index * 4U]);
  for (index = 16U; index < 64U; ++index) {
    uint32_t s0 = laghu_rotate_right(words[index - 15U], 7U) ^
                  laghu_rotate_right(words[index - 15U], 18U) ^
                  (words[index - 15U] >> 3U);
    uint32_t s1 = laghu_rotate_right(words[index - 2U], 17U) ^
                  laghu_rotate_right(words[index - 2U], 19U) ^
                  (words[index - 2U] >> 10U);
    words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
  }
  a = context->state[0];
  b = context->state[1];
  c = context->state[2];
  d = context->state[3];
  e = context->state[4];
  f = context->state[5];
  g = context->state[6];
  h = context->state[7];
  for (index = 0U; index < 64U; ++index) {
    uint32_t sum1 = laghu_rotate_right(e, 6U) ^ laghu_rotate_right(e, 11U) ^
                    laghu_rotate_right(e, 25U);
    uint32_t choice = (e & f) ^ ((~e) & g);
    uint32_t temporary1 =
        h + sum1 + choice + laghu_sha256_round_constants[index] + words[index];
    uint32_t sum0 = laghu_rotate_right(a, 2U) ^ laghu_rotate_right(a, 13U) ^
                    laghu_rotate_right(a, 22U);
    uint32_t temporary2 = sum0 + ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }
  context->state[0] += a;
  context->state[1] += b;
  context->state[2] += c;
  context->state[3] += d;
  context->state[4] += e;
  context->state[5] += f;
  context->state[6] += g;
  context->state[7] += h;
}

void laghu_sha256_init(laghu_sha256_context *context) {
  static const uint32_t initial[8] = {
      UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85), UINT32_C(0x3c6ef372),
      UINT32_C(0xa54ff53a), UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
      UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)};
  memcpy(context->state, initial, sizeof(initial));
  context->bit_length = 0U;
  context->block_length = 0U;
}

void laghu_sha256_update(laghu_sha256_context *context,
                         const unsigned char *data, size_t length) {
  size_t index;
  for (index = 0U; index < length; ++index) {
    context->block[context->block_length++] = data[index];
    if (context->block_length == LAGHU_SHA256_BLOCK_SIZE) {
      laghu_sha256_transform(context);
      context->bit_length += UINT64_C(512);
      context->block_length = 0U;
    }
  }
}

void laghu_sha256_final(laghu_sha256_context *context,
                        unsigned char digest[LAGHU_SHA256_DIGEST_SIZE]) {
  size_t index = context->block_length;
  uint64_t bit_length;
  context->block[index++] = 0x80U;
  if (index > 56U) {
    while (index < LAGHU_SHA256_BLOCK_SIZE) context->block[index++] = 0U;
    laghu_sha256_transform(context);
    index = 0U;
  }
  while (index < 56U) context->block[index++] = 0U;
  bit_length =
      context->bit_length + ((uint64_t)context->block_length * UINT64_C(8));
  for (index = 0U; index < 8U; ++index)
    context->block[63U - index] = (unsigned char)(bit_length >> (index * 8U));
  laghu_sha256_transform(context);
  for (index = 0U; index < 8U; ++index)
    laghu_store_u32_be(&digest[index * 4U], context->state[index]);
}

bool laghu_sha256_hex(laghu_buffer input, char output[LAGHU_SHA256_HEX_SIZE]) {
  static const char digits[] = "0123456789abcdef";
  laghu_sha256_context context;
  unsigned char digest[LAGHU_SHA256_DIGEST_SIZE];
  size_t index;
  if (output == NULL) return false;
  if (input.data == NULL && input.length != 0U) {
    output[0] = '\0';
    return false;
  }
  laghu_sha256_init(&context);
  laghu_sha256_update(&context, input.data, input.length);
  laghu_sha256_final(&context, digest);
  for (index = 0U; index < LAGHU_SHA256_DIGEST_SIZE; ++index) {
    output[index * 2U] = digits[digest[index] >> 4U];
    output[index * 2U + 1U] = digits[digest[index] & 0x0fU];
  }
  output[LAGHU_SHA256_HEX_LENGTH] = '\0';
  return true;
}

void laghu_sha256_final_hex(laghu_sha256_context *context,
                            char output[LAGHU_SHA256_HEX_SIZE]) {
  static const char digits[] = "0123456789abcdef";
  unsigned char digest[LAGHU_SHA256_DIGEST_SIZE];
  size_t index;
  if (context == NULL || output == NULL) return;
  laghu_sha256_final(context, digest);
  for (index = 0U; index < LAGHU_SHA256_DIGEST_SIZE; ++index) {
    output[index * 2U] = digits[digest[index] >> 4U];
    output[index * 2U + 1U] = digits[digest[index] & 0x0fU];
  }
  output[LAGHU_SHA256_HEX_LENGTH] = '\0';
}
