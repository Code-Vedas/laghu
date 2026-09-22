// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <laghu/core/contract.hpp>

namespace laghu::adapters::internal {

// Normalizes a libidn2 status at the adapter boundary. Native constants stay
// private to the implementation and adapter-owned tests.
[[nodiscard]] core::DependencyStatus idn2_status(int status) noexcept;

}  // namespace laghu::adapters::internal
