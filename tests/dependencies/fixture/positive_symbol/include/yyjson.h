// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>

struct yyjson_doc {};
inline constexpr unsigned int YYJSON_READ_NOFLAG = 0U;
yyjson_doc* yyjson_read_opts(const char*, std::size_t, unsigned int, const void*, void*);
