// SPDX-License-Identifier: AGPL-3.0-only
#include <type_traits>

#include <laghu/adapters/dns.hpp>

static_assert(std::is_trivially_copyable_v<laghu::adapters::DnsAddress>);
static_assert(std::is_trivially_copyable_v<laghu::adapters::DnsNameserver>);
static_assert(std::is_trivially_copyable_v<laghu::adapters::DnsQueryToken>);
static_assert(std::is_trivially_copyable_v<laghu::adapters::DnsQueryResult>);
static_assert(!std::is_copy_constructible_v<laghu::adapters::DnsResolver>);
static_assert(!std::is_copy_assignable_v<laghu::adapters::DnsResolver>);

int main() { return 0; }
