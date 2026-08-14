// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_PRECOMPRESSED_H
#define LAGHU_PRECOMPRESSED_H

#include "laghu/cache.h"

#define LAGHU_PRECOMPRESSED_MINIMUM 128U

typedef enum { LAGHU_PRECOMPRESSED_IDENTITY = 0, LAGHU_PRECOMPRESSED_GZIP, LAGHU_PRECOMPRESSED_BROTLI } laghu_precompressed_coding;

bool laghu_precompressed_text_type(const char *content_type);
bool laghu_precompressed_publish(const char *cache_path, laghu_buffer body, const char *content_type, const char *validator);
bool laghu_precompressed_select(const char *cache_path, laghu_buffer body, const char *accept_encoding, laghu_runtime_cache_entry *entry,
                                laghu_precompressed_coding *coding);
bool laghu_precompressed_select_hash(const char *cache_path, const char *payload_hash, const char *accept_encoding,
                                     laghu_runtime_cache_entry *entry, laghu_precompressed_coding *coding);
const char *laghu_precompressed_coding_name(laghu_precompressed_coding coding);

#endif
