// SPDX-License-Identifier: AGPL-3.0-only
#include <memory>

#include <laghu/core/shared_offsets.hpp>

laghu::core::SharedSchema<std::unique_ptr<std::uint32_t>> invalid_schema;
