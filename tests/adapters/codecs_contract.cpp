// SPDX-License-Identifier: AGPL-3.0-only
#include <type_traits>

#include <laghu/adapters/codecs.hpp>

static_assert(std::is_move_constructible_v<laghu::adapters::CodecStream>);
static_assert(!std::is_copy_constructible_v<laghu::adapters::CodecStream>);
static_assert(std::is_trivially_copyable_v<laghu::adapters::CodecLimits>);
static_assert(std::is_trivially_copyable_v<laghu::adapters::CodecProgress>);

int main() { return 0; }
