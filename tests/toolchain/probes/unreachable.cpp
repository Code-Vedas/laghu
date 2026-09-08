// SPDX-License-Identifier: AGPL-3.0-only
#include <utility>
constexpr int selected(bool enabled) { if (enabled) return 23; std::unreachable(); }
static_assert(selected(true) == 23);
