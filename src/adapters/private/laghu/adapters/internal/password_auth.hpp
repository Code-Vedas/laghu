// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <laghu/adapters/dependency.hpp>
#include <laghu/adapters/password_auth.hpp>
#include <laghu/core/views.hpp>

namespace laghu::adapters::internal {

// This adapter-private value is the sole owned password copy.  It cannot be
// copied, and every move clears its source before that source can leave scope.
class SecretPassword final {
 public:
  static constexpr std::size_t storage_capacity =
      PasswordVerificationLimits::maximum_supported_password_bytes + 1;

  [[nodiscard]] static core::Result<SecretPassword> from(core::TextView password) noexcept {
    if (password.size() >= storage_capacity) {
      return std::unexpected{core::Error{core::ErrorDomain::core,
                                         core::ErrorCode::invalid_range, 0,
                                         "text does not fit including terminator"}};
    }

    SecretPassword output;
    const std::string_view input = password.string_view();
    for (std::size_t index = 0; index < input.size(); ++index) {
      const char character = input[index];
      if (character == '\0') {
        return std::unexpected{core::Error{core::ErrorDomain::core,
                                           core::ErrorCode::invalid_input, 0,
                                           "text contains an embedded NUL"}};
      }
      output.bytes_[index] = character;
    }
    output.size_ = password.size();
    return output;
  }

  SecretPassword(const SecretPassword&) = delete;
  SecretPassword& operator=(const SecretPassword&) = delete;

  SecretPassword(SecretPassword&& other) noexcept { move_from(other); }

  SecretPassword& operator=(SecretPassword&& other) noexcept {
    if (this != &other) {
      cleanse();
      move_from(other);
    }
    return *this;
  }

  ~SecretPassword() { cleanse(); }

  [[nodiscard]] const char* c_str() const noexcept { return bytes_.data(); }
  [[nodiscard]] std::string_view view() const noexcept { return {bytes_.data(), size_}; }

  // This private invariant is exercised by the adapter's owner tests.
  [[nodiscard]] bool is_cleansed() const noexcept {
    if (size_ != 0) {
      return false;
    }
    for (const char byte : bytes_) {
      if (byte != '\0') {
        return false;
      }
    }
    return true;
  }

 private:
  SecretPassword() noexcept = default;

  void move_from(SecretPassword& other) noexcept {
    bytes_ = other.bytes_;
    size_ = other.size_;
    other.cleanse();
  }

  void cleanse() noexcept {
    for (char& byte : bytes_) {
      volatile char& secured_byte = byte;
      secured_byte = '\0';
    }
    size_ = 0;
  }

  std::array<char, storage_capacity> bytes_{};
  std::size_t size_{};
};

// The private boundary exposes no native type and lets focused tests exercise
// every errno class without a global hook or a password-auth side effect.
[[nodiscard]] core::Error password_auth_native_error(
    std::int32_t native_code, const DependencyLogSink& log_sink) noexcept;

}  // namespace laghu::adapters::internal
