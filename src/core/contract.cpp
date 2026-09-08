// SPDX-License-Identifier: AGPL-3.0-only
#include "compiler_extensions.hpp"
#include "laghu/capabilities.hpp"

int laghu_core_contract() noexcept {
  static_assert(laghu::capability::has_if_consteval);
  return laghu::core::compiler_extensions::family_id();
}
