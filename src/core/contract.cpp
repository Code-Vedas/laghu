// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/contract.hpp>

#include <laghu/core/internal/compiler_extensions.hpp>
#include <laghu/capabilities.hpp>

int laghu::core::compiler_family_id() noexcept {
  static_assert(laghu::capability::has_if_consteval);
  return laghu::core::internal::compiler_family_id();
}
