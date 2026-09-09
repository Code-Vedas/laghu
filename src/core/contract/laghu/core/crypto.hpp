// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <type_traits>

#include <laghu/core/views.hpp>

namespace laghu::core {

namespace detail {

template <std::size_t Size, class Tag>
class FixedCryptoBytes final {
 public:
  static constexpr std::size_t size = Size;

  [[nodiscard]] static constexpr FixedCryptoBytes from_array(
      std::array<std::byte, Size> bytes) noexcept {
    return FixedCryptoBytes{bytes};
  }

  [[nodiscard]] constexpr const std::array<std::byte, Size>& bytes() const noexcept {
    return bytes_;
  }

 private:
  explicit constexpr FixedCryptoBytes(std::array<std::byte, Size> bytes) noexcept
      : bytes_(bytes) {}

  std::array<std::byte, Size> bytes_{};
};

}  // namespace detail

struct Sha256DigestTag;
struct Ed25519PublicKeyTag;
struct Ed25519SignatureTag;

using Sha256Digest = detail::FixedCryptoBytes<32, Sha256DigestTag>;
using Ed25519PublicKey = detail::FixedCryptoBytes<32, Ed25519PublicKeyTag>;
using Ed25519Signature = detail::FixedCryptoBytes<64, Ed25519SignatureTag>;

using Sha256Function = Result<Sha256Digest> (*)(ByteView) noexcept;
using HmacSha256Function = Result<Sha256Digest> (*)(ByteView, ByteView) noexcept;
using Ed25519SignFunction = Result<Ed25519Signature> (*)(ByteView, ByteView) noexcept;
using Ed25519VerifyFunction = Result<bool> (*)(const Ed25519PublicKey&,
                                                ByteView,
                                                const Ed25519Signature&) noexcept;
using SpkiSha256Function = Result<Sha256Digest> (*)(ByteView) noexcept;
using ConstantTimeEqualFunction = bool (*)(const Sha256Digest&,
                                            const Sha256Digest&) noexcept;
using FillEntropyFunction = Result<void> (*)(MutableByteView) noexcept;

struct CryptoProvider final {
  Sha256Function sha256{};
  HmacSha256Function hmac_sha256{};
  Ed25519SignFunction ed25519_sign{};
  Ed25519VerifyFunction ed25519_verify{};
  SpkiSha256Function spki_sha256{};
  ConstantTimeEqualFunction constant_time_equal{};
  FillEntropyFunction fill_entropy{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return sha256 != nullptr && hmac_sha256 != nullptr && ed25519_sign != nullptr &&
           ed25519_verify != nullptr && spki_sha256 != nullptr &&
           constant_time_equal != nullptr && fill_entropy != nullptr;
  }
};

static_assert(std::is_trivially_copyable_v<Sha256Digest>);
static_assert(std::is_standard_layout_v<Sha256Digest>);
static_assert(std::is_trivially_copyable_v<Ed25519PublicKey>);
static_assert(std::is_standard_layout_v<Ed25519PublicKey>);
static_assert(std::is_trivially_copyable_v<Ed25519Signature>);
static_assert(std::is_standard_layout_v<Ed25519Signature>);
static_assert(std::is_trivially_copyable_v<CryptoProvider>);
static_assert(std::is_standard_layout_v<CryptoProvider>);

}  // namespace laghu::core
