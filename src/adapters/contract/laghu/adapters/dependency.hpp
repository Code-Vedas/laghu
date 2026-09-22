// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include <laghu/core/contract.hpp>

namespace laghu::adapters {

enum class DependencyLogLevel : std::uint8_t {
  warning,
  error,
};

struct DependencyLogRecord final {
  static constexpr std::size_t component_capacity = 24;
  static constexpr std::size_t message_capacity = 96;

  DependencyLogLevel level{DependencyLogLevel::error};
  core::DependencyId dependency{core::DependencyId::none};
  core::DependencyOperation operation{core::DependencyOperation::none};
  core::DependencyStatus status{core::DependencyStatus::unknown};
  std::int32_t native_code{};
  std::array<char, component_capacity> component_bytes{};
  std::array<char, message_capacity> message_bytes{};
  std::uint8_t component_size{};
  std::uint8_t message_size{};

  [[nodiscard]] constexpr std::string_view component() const noexcept {
    return {component_bytes.data(), component_size};
  }
  [[nodiscard]] constexpr std::string_view message() const noexcept {
    return {message_bytes.data(), message_size};
  }
};

using DependencyLogWrite = void (*)(void*, const DependencyLogRecord&) noexcept;

// The caller owns `context` and the function it names. The adapter holds only
// this copyable, non-owning function-table value; no logger is global.
struct DependencyLogSink final {
  void* context{};
  DependencyLogWrite write{};

  [[nodiscard]] constexpr bool enabled() const noexcept { return write != nullptr; }
};

// This boundary has no native-string parameter. Adapter diagnostics are
// derived solely from Laghu enums and the signed 32-bit native code.
[[nodiscard]] core::Error normalize_dependency_error(
    core::DependencyId dependency, core::DependencyOperation operation,
    core::DependencyStatus status, std::int32_t native_code) noexcept;

// Logging is best effort. The sink callback is noexcept and therefore cannot
// propagate a dependency or caller callback failure beyond this adapter.
void log_dependency_error(const DependencyLogSink& sink, const core::Error& error) noexcept;

static_assert(std::is_trivially_copyable_v<DependencyLogRecord>);
static_assert(std::is_standard_layout_v<DependencyLogRecord>);
static_assert(std::is_trivially_copyable_v<DependencyLogSink>);
static_assert(std::is_standard_layout_v<DependencyLogSink>);

}  // namespace laghu::adapters
