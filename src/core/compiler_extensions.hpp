// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
namespace laghu::core::compiler_extensions {
[[nodiscard]] constexpr int family_id() noexcept {
#if defined(__clang__)
  return 2;
#elif defined(__GNUC__)
  return 1;
#else
  return 0;
#endif
}
[[noreturn]] inline void unreachable() noexcept {
#if defined(__clang__) || defined(__GNUC__)
  __builtin_unreachable();
#else
  __builtin_trap();
#endif
}
}  // namespace laghu::core::compiler_extensions
