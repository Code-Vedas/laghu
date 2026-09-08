// SPDX-License-Identifier: AGPL-3.0-only
#include <expected>
constexpr std::expected<int, int> result{23};
static_assert(result.has_value() && *result == 23);
