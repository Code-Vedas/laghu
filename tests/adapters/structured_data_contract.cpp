// SPDX-License-Identifier: AGPL-3.0-only
#include <type_traits>

#include <laghu/adapters/structured_data.hpp>

static_assert(std::is_trivially_copyable_v<laghu::adapters::JsonDocumentLimits>);
static_assert(std::is_trivially_copyable_v<laghu::adapters::JsonValue>);
static_assert(!std::is_copy_constructible_v<laghu::adapters::JsonDocument>);

int main() { return 0; }
