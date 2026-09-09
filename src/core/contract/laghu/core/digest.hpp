// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>

#include <laghu/core/crypto.hpp>

namespace laghu::core {

class PublicDigest final {
 public:
  static constexpr std::size_t value_size = Sha256Digest::size;
  static constexpr std::size_t lowercase_hex_size = value_size * 2;

  // The caller has already canonicalized and sanitized these non-secret bytes.
  [[nodiscard]] static Result<PublicDigest> from_canonical_sanitized_bytes(
      const CryptoProvider& provider, ByteView canonical_sanitized_bytes) noexcept;

  [[nodiscard]] constexpr const Sha256Digest& value() const noexcept { return value_; }
  [[nodiscard]] constexpr const std::array<char, lowercase_hex_size>& lowercase_hex() const noexcept {
    return lowercase_hex_;
  }

 private:
  explicit constexpr PublicDigest(Sha256Digest value,
                                  std::array<char, lowercase_hex_size> lowercase_hex) noexcept
      : value_(value), lowercase_hex_(lowercase_hex) {}

  Sha256Digest value_;
  std::array<char, lowercase_hex_size> lowercase_hex_;
};

static_assert(sizeof(PublicDigest) == PublicDigest::value_size + PublicDigest::lowercase_hex_size);

}  // namespace laghu::core
