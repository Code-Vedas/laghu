// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>
#include <type_traits>

#include <laghu/core/crypto.hpp>

namespace laghu::core::internal {

class FingerprintKeyId final {
 public:
  [[nodiscard]] static constexpr Result<FingerprintKeyId> from_uint64(
      std::uint64_t value) noexcept {
    if (value == 0) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_input, 0,
                                   "fingerprint key identifier must be nonzero"}};
    }
    return FingerprintKeyId{value};
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

 private:
  explicit constexpr FingerprintKeyId(std::uint64_t value) noexcept : value_(value) {}

  std::uint64_t value_{};
};

class SecretFingerprint final {
 public:
  [[nodiscard]] static Result<SecretFingerprint> from_canonical_sanitized_bytes(
      const CryptoProvider& provider, FingerprintKeyId key_id, ByteView borrowed_key,
      ByteView canonical_sanitized_bytes) noexcept;

 private:
  explicit constexpr SecretFingerprint(FingerprintKeyId key_id, Sha256Digest value) noexcept
      : key_id_(key_id), value_(value) {}

  FingerprintKeyId key_id_;
  Sha256Digest value_;

  friend bool constant_time_equal(const CryptoProvider&, const SecretFingerprint&,
                                  const SecretFingerprint&) noexcept;
};

// This is intentionally the only observation operation for secret fingerprints.
// The provider compares the HMAC values in constant time; key identifiers scope
// equality to one caller-managed key without retaining any key material.
[[nodiscard]] bool constant_time_equal(const CryptoProvider& provider,
                                       const SecretFingerprint& left,
                                       const SecretFingerprint& right) noexcept;

static_assert(sizeof(FingerprintKeyId) == sizeof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<FingerprintKeyId>);
static_assert(std::is_standard_layout_v<FingerprintKeyId>);
static_assert(sizeof(SecretFingerprint) == sizeof(FingerprintKeyId) + sizeof(Sha256Digest));
static_assert(std::is_trivially_copyable_v<SecretFingerprint>);
static_assert(std::is_standard_layout_v<SecretFingerprint>);

}  // namespace laghu::core::internal
