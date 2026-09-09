// SPDX-License-Identifier: AGPL-3.0-only
#include <cstdint>
#include <span>

#include <laghu/core/shared_offsets.hpp>

laghu::core::SharedSchema<std::span<const std::uint32_t>> invalid_schema;
