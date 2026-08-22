// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/precompressed.h"

#include <brotli/encode.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "laghu/types.h"

static bool laghu_precompressed_key(const char *payload_hash, laghu_precompressed_coding coding, char output[LAGHU_RUNTIME_KEY_SIZE]) {
  const char *name = laghu_precompressed_coding_name(coding);
  char material[LAGHU_RUNTIME_KEY_SIZE + 32U];
  int length;
  if (payload_hash == NULL || name == NULL) return false;
  length = snprintf(material, sizeof(material), "laghu-precompressed\n%s\n%s", payload_hash, name);
  return length > 0 && (size_t)length < sizeof(material) && laghu_sha256_hex((laghu_buffer){(const unsigned char *)material, (size_t)length}, output);
}

const char *laghu_precompressed_coding_name(laghu_precompressed_coding coding) {
  switch (coding) {
    case LAGHU_PRECOMPRESSED_GZIP:
      return "gzip";
    case LAGHU_PRECOMPRESSED_BROTLI:
      return "br";
    default:
      return NULL;
  }
}

bool laghu_precompressed_text_type(const char *content_type) {
  const char *suffix;
  if (content_type == NULL) return false;
  while (*content_type == ' ' || *content_type == '\t') ++content_type;
  suffix = strchr(content_type, ';');
  if (suffix == NULL) suffix = content_type + strlen(content_type);
  return (size_t)(suffix - content_type) >= 5U &&
         (strncmp(content_type, "text/", 5U) == 0 || strncmp(content_type, "application/javascript", 22U) == 0 ||
          strncmp(content_type, "application/json", 16U) == 0 || strncmp(content_type, "application/xml", 15U) == 0 ||
          strncmp(content_type, "application/wasm", 16U) == 0 || strncmp(content_type, "image/svg+xml", 13U) == 0);
}

static bool laghu_precompressed_encode_gzip(laghu_buffer input, unsigned char **output, size_t *output_length) {
  z_stream stream = {0};
  unsigned char *buffer;
  uLong bound;
  int status;
  if (input.length > UINT_MAX || output == NULL || output_length == NULL) return false;
  bound = deflateBound(&stream, (uLong)input.length);
  if (bound > SIZE_MAX) return false;
  buffer = malloc((size_t)bound);
  if (buffer == NULL) return false;
  status = deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
  if (status != Z_OK) {
    free(buffer);
    return false;
  }
  stream.next_in = (Bytef *)(void *)input.data;
  stream.avail_in = (uInt)input.length;
  stream.next_out = buffer;
  stream.avail_out = (uInt)bound;
  status = deflate(&stream, Z_FINISH);
  if (status != Z_STREAM_END || deflateEnd(&stream) != Z_OK) {
    free(buffer);
    return false;
  }
  *output = buffer;
  *output_length = stream.total_out;
  return true;
}

static bool laghu_precompressed_encode_brotli(laghu_buffer input, unsigned char **output, size_t *output_length) {
  size_t bound;
  unsigned char *buffer;
  if (output == NULL || output_length == NULL) return false;
  bound = BrotliEncoderMaxCompressedSize(input.length);
  if (bound == 0U) return false;
  buffer = malloc(bound);
  if (buffer == NULL) return false;
  if (!BrotliEncoderCompress(8, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_TEXT, input.length, input.data, &bound, buffer)) {
    free(buffer);
    return false;
  }
  *output = buffer;
  *output_length = bound;
  return true;
}

static bool laghu_precompressed_publish_one(const char *cache_path, const char *payload_hash, const char *content_type, const char *validator,
                                            laghu_precompressed_coding coding, laghu_buffer source) {
  unsigned char *encoded = NULL;
  size_t length = 0U;
  char key[LAGHU_RUNTIME_KEY_SIZE];
  laghu_runtime_cache_entry entry;
  bool encoded_ok = coding == LAGHU_PRECOMPRESSED_GZIP ? laghu_precompressed_encode_gzip(source, &encoded, &length)
                                                       : laghu_precompressed_encode_brotli(source, &encoded, &length);
  bool published = false;
  if (!encoded_ok || length >= source.length || !laghu_precompressed_key(payload_hash, coding, key)) {
    free(encoded);
    return false;
  }
  published = laghu_runtime_cache_publish(cache_path, key, key, validator == NULL ? "" : validator, content_type,
                                          laghu_precompressed_coding_name(coding), (laghu_buffer){encoded, length}, &entry);
  free(encoded);
  return published;
}

bool laghu_precompressed_publish(const char *cache_path, laghu_buffer body, const char *content_type, const char *validator) {
  char payload_hash[LAGHU_RUNTIME_KEY_SIZE];
  if (cache_path == NULL || !laghu_precompressed_text_type(content_type) || body.data == NULL || body.length < LAGHU_PRECOMPRESSED_MINIMUM ||
      !laghu_sha256_hex(body, payload_hash))
    return false;
  (void)laghu_precompressed_publish_one(cache_path, payload_hash, content_type, validator, LAGHU_PRECOMPRESSED_GZIP, body);
  (void)laghu_precompressed_publish_one(cache_path, payload_hash, content_type, validator, LAGHU_PRECOMPRESSED_BROTLI, body);
  return true;
}

static const char *laghu_precompressed_find(const char *start, const char *end, char needle) {
  while (start < end) {
    if (*start == needle) return start;
    ++start;
  }
  return NULL;
}

static bool laghu_precompressed_q_zero(const char *start, const char *end) {
  const char *cursor = start;
  if (cursor < end && *cursor == ';') ++cursor;
  while (cursor < end) {
    const char *parameter_end = laghu_precompressed_find(cursor, end, ';');
    const char *equals;
    if (parameter_end == NULL) parameter_end = end;
    while (cursor < parameter_end && isspace((unsigned char)*cursor)) ++cursor;
    equals = laghu_precompressed_find(cursor, parameter_end, '=');
    if (equals != NULL && equals - cursor == 1 && tolower((unsigned char)cursor[0]) == 'q') {
      const char *number = equals + 1U;
      while (number < parameter_end && isspace((unsigned char)*number)) ++number;
      if (number < parameter_end && *number == '0') {
        ++number;
        if (number == parameter_end || *number == '.') {
          while (number < parameter_end && (*number == '.' || *number == '0' || isspace((unsigned char)*number))) ++number;
          if (number == parameter_end) return true;
        }
      }
    }
    cursor = parameter_end == end ? end : parameter_end + 1U;
  }
  return false;
}

static bool laghu_precompressed_token_equal(const char *left, size_t length, const char *right) {
  size_t index;
  if (strlen(right) != length) return false;
  for (index = 0U; index < length; ++index)
    if (tolower((unsigned char)left[index]) != tolower((unsigned char)right[index])) return false;
  return true;
}

static bool laghu_precompressed_accepts(const char *value, const char *coding) {
  const char *cursor = value;
  if (value == NULL || coding == NULL) return false;
  while (*cursor != '\0') {
    const char *end = strchr(cursor, ',');
    const char *token_end = end == NULL ? cursor + strlen(cursor) : end;
    const char *parameter;
    bool match;
    while (cursor < token_end && isspace((unsigned char)*cursor)) ++cursor;
    parameter = laghu_precompressed_find(cursor, token_end, ';');
    if (parameter == NULL) parameter = token_end;
    while (parameter > cursor && isspace((unsigned char)parameter[-1])) --parameter;
    match = laghu_precompressed_token_equal(cursor, (size_t)(parameter - cursor), coding) ||
            laghu_precompressed_token_equal(cursor, (size_t)(parameter - cursor), "*");
    if (match && !laghu_precompressed_q_zero(parameter, token_end)) return true;
    cursor = end == NULL ? token_end : end + 1U;
  }
  return false;
}

bool laghu_precompressed_select(const char *cache_path, laghu_buffer body, const char *accept_encoding, laghu_runtime_cache_entry *entry,
                                laghu_precompressed_coding *coding) {
  char payload_hash[LAGHU_RUNTIME_KEY_SIZE];
  if (body.data == NULL || !laghu_sha256_hex(body, payload_hash)) return false;
  return laghu_precompressed_select_hash(cache_path, payload_hash, accept_encoding, entry, coding);
}

bool laghu_precompressed_select_hash(const char *cache_path, const char *payload_hash, const char *accept_encoding,
                                     laghu_runtime_cache_entry *entry, laghu_precompressed_coding *coding) {
  char key[LAGHU_RUNTIME_KEY_SIZE];
  static const laghu_precompressed_coding preferences[] = {LAGHU_PRECOMPRESSED_BROTLI, LAGHU_PRECOMPRESSED_GZIP};
  size_t index;
  if (entry == NULL || coding == NULL || payload_hash == NULL) return false;
  for (index = 0U; index < sizeof(preferences) / sizeof(preferences[0]); ++index) {
    const char *name = laghu_precompressed_coding_name(preferences[index]);
    if (laghu_precompressed_accepts(accept_encoding, name) && laghu_precompressed_key(payload_hash, preferences[index], key) &&
        laghu_runtime_cache_lookup_variant(cache_path, key, entry)) {
      *coding = preferences[index];
      return true;
    }
  }
  *coding = LAGHU_PRECOMPRESSED_IDENTITY;
  return false;
}
