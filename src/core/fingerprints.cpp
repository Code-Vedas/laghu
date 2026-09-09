// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/internal/fingerprints.hpp>

namespace laghu::core::internal {

Result<SecretFingerprint> SecretFingerprint::from_canonical_sanitized_bytes(
    const CryptoProvider& provider, FingerprintKeyId key_id, ByteView borrowed_key,
    ByteView canonical_sanitized_bytes) noexcept {
  if (borrowed_key.empty()) {
    return std::unexpected{Error{ErrorDomain::crypto, ErrorCode::invalid_input, 0,
                                 "fingerprint key must not be empty"}};
  }
  if (provider.hmac_sha256 == nullptr) {
    return std::unexpected{Error{ErrorDomain::crypto, ErrorCode::unavailable_capability, 0,
                                 "HMAC-SHA-256 provider is unavailable"}};
  }
  const auto value = provider.hmac_sha256(borrowed_key, canonical_sanitized_bytes);
  if (!value.has_value()) {
    return std::unexpected{value.error()};
  }
  return SecretFingerprint{key_id, *value};
}

bool constant_time_equal(const CryptoProvider& provider, const SecretFingerprint& left,
                         const SecretFingerprint& right) noexcept {
  if (provider.constant_time_equal == nullptr) {
    return false;
  }
  const bool values_equal = provider.constant_time_equal(left.value_, right.value_);
  const bool key_ids_equal = left.key_id_.value() == right.key_id_.value();
  return values_equal && key_ids_equal;
}

}  // namespace laghu::core::internal
