// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include <laghu/adapters/dependency.hpp>
#include <laghu/core/views.hpp>

namespace laghu::adapters {

class AsciiHostname final {
 public:
  static constexpr std::size_t maximum_text_length = 253;
  static constexpr std::size_t storage_capacity = maximum_text_length + 1;

  [[nodiscard]] static core::Result<AsciiHostname> from_ascii(
      std::string_view hostname) noexcept;

  [[nodiscard]] constexpr std::string_view value() const noexcept {
    return {bytes_.data(), length_};
  }

 private:
  explicit constexpr AsciiHostname(std::array<char, storage_capacity> bytes,
                                   std::uint16_t length) noexcept
      : bytes_(bytes), length_(length) {}

  std::array<char, storage_capacity> bytes_{};
  std::uint16_t length_{};
};

// Accepts UTF-8 input only and returns one bounded, owning ASCII hostname.
// Native libidn2 allocation remains entirely inside this adapter boundary.
[[nodiscard]] core::Result<AsciiHostname> idna_to_ascii(
    core::TextView hostname, DependencyLogSink log_sink = {}) noexcept;

static_assert(std::is_trivially_copyable_v<AsciiHostname>);
static_assert(std::is_standard_layout_v<AsciiHostname>);

}  // namespace laghu::adapters
