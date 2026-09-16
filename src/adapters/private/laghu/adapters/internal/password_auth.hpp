// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>

#include <laghu/adapters/dependency.hpp>

namespace laghu::adapters::internal {

// The private boundary exposes no native type and lets focused tests exercise
// every errno class without a global hook or a password-auth side effect.
[[nodiscard]] core::Error password_auth_native_error(
    std::int32_t native_code, const DependencyLogSink& log_sink) noexcept;

}  // namespace laghu::adapters::internal
