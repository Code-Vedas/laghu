// SPDX-License-Identifier: AGPL-3.0-only
#include <type_traits>

#include <laghu/adapters/http3.hpp>
#include <laghu/adapters/quic.hpp>

static_assert(!std::is_copy_constructible_v<laghu::adapters::QuicSession>);
static_assert(!std::is_copy_constructible_v<laghu::adapters::Http3Session>);
static_assert(std::is_trivially_copyable_v<laghu::adapters::QuicConnectionId>);
static_assert(std::is_trivially_copyable_v<laghu::adapters::Http3Event>);

int main() { return 0; }
