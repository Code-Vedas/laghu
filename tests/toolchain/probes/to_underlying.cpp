// SPDX-License-Identifier: AGPL-3.0-only
#include <utility>
enum class mode : unsigned char { active = 23 };
static_assert(std::to_underlying(mode::active) == 23U);
