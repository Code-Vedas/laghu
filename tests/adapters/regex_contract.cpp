// SPDX-License-Identifier: AGPL-3.0-only
#include <type_traits>

#include <laghu/adapters/regex.hpp>

static_assert(std::is_trivially_copyable_v<laghu::adapters::RegexCompileLimits>);
static_assert(std::is_trivially_copyable_v<laghu::adapters::RegexMatchLimits>);
static_assert(std::is_trivially_copyable_v<laghu::adapters::RegexCapture>);
static_assert(!std::is_copy_constructible_v<laghu::adapters::RegexPattern>);

int main() { return 0; }
