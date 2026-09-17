// SPDX-License-Identifier: AGPL-3.0-only
#include <type_traits>

#include <laghu/adapters/http2.hpp>

static_assert(!std::is_copy_constructible_v<laghu::adapters::Http2Session>);
static_assert(std::is_nothrow_move_constructible_v<laghu::adapters::Http2Session>);
static_assert(std::is_trivially_copyable_v<laghu::adapters::Http2Event>);

int main() { return 0; }
