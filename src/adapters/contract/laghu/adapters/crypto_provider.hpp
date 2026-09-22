// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <laghu/adapters/dependency.hpp>
#include <laghu/core/crypto.hpp>

namespace laghu::adapters {

[[nodiscard]] core::CryptoProvider crypto_provider() noexcept;

// This concrete wrapper is the opt-in logging surface for callers that own a
// DependencyLogSink. It retains only the sink's non-owning function-table
// value and does not alter the dependency-free core CryptoProvider contract.
class CryptoProviderWithLog final {
 public:
  explicit constexpr CryptoProviderWithLog(DependencyLogSink sink) noexcept
      : sink_(sink) {}

  [[nodiscard]] core::Result<core::Sha256Digest> sha256(core::ByteView input) const noexcept;
  [[nodiscard]] core::Result<core::Sha256Digest> hmac_sha256(
      core::ByteView key, core::ByteView input) const noexcept;
  [[nodiscard]] core::Result<core::Ed25519Signature> ed25519_sign(
      core::ByteView private_key_seed, core::ByteView input) const noexcept;
  [[nodiscard]] core::Result<bool> ed25519_verify(
      const core::Ed25519PublicKey& public_key, core::ByteView input,
      const core::Ed25519Signature& signature) const noexcept;
  [[nodiscard]] core::Result<core::Sha256Digest> spki_sha256(core::ByteView der) const noexcept;

 private:
  DependencyLogSink sink_{};
};

[[nodiscard]] constexpr CryptoProviderWithLog crypto_provider_with_log(
    DependencyLogSink sink) noexcept {
  return CryptoProviderWithLog{sink};
}

}  // namespace laghu::adapters
