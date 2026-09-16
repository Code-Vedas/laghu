// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>

#include <laghu/adapters/dependency.hpp>

namespace laghu::adapters::internal {

// This is the state owned by an adapter while a native library may invoke its
// C-style error callback. It is fixed-size and contains no native object.
struct DependencyCallbackState final {
  DependencyLogSink log_sink{};
  core::DependencyId dependency{core::DependencyId::none};
  core::DependencyOperation operation{core::DependencyOperation::none};
  core::DependencyStatus status{core::DependencyStatus::unknown};
  core::Error error{core::ErrorDomain::dependency, core::ErrorCode::dependency};
  bool failed{};
};

// Native callbacks terminate here: they update caller-owned adapter state and
// optionally write a bounded Laghu record, then return noexcept to the native
// dependency. Native diagnostic text is deliberately not accepted.
void dependency_failure_callback(void* context, std::int32_t native_code) noexcept;

}  // namespace laghu::adapters::internal
