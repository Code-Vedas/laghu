// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include <laghu/core/digest.hpp>
#include <laghu/core/internal/fingerprints.hpp>

namespace {

struct Fixture final {
  bool fail_sha256{};
  bool fail_hmac{};
  std::size_t sha256_calls{};
  std::size_t hmac_calls{};
  std::size_t compare_calls{};
};

Fixture fixture{};

template <std::size_t Size>
[[nodiscard]] laghu::core::ByteView view_of(const std::array<std::byte, Size>& bytes) noexcept {
  return *laghu::core::ByteView::from(std::span<const std::byte>{bytes});
}

[[nodiscard]] laghu::core::Result<laghu::core::Sha256Digest> fake_sha256(
    laghu::core::ByteView input) noexcept {
  ++fixture.sha256_calls;
  if (fixture.fail_sha256) {
    return std::unexpected{laghu::core::Error{laghu::core::ErrorDomain::crypto,
                                               laghu::core::ErrorCode::crypto, 41,
                                               "injected SHA-256 failure"}};
  }
  std::array<std::byte, laghu::core::Sha256Digest::size> output{};
  for (std::size_t index = 0; index < output.size(); ++index) {
    const auto source = input.empty() ? std::byte{0} : input.span()[index % input.size()];
    output[index] = source ^ static_cast<std::byte>(index);
  }
  return laghu::core::Sha256Digest::from_array(output);
}

[[nodiscard]] laghu::core::Result<laghu::core::Sha256Digest> fake_hmac_sha256(
    laghu::core::ByteView key, laghu::core::ByteView input) noexcept {
  ++fixture.hmac_calls;
  if (fixture.fail_hmac) {
    return std::unexpected{laghu::core::Error{laghu::core::ErrorDomain::crypto,
                                               laghu::core::ErrorCode::crypto, 42,
                                               "injected HMAC-SHA-256 failure"}};
  }
  std::array<std::byte, laghu::core::Sha256Digest::size> output{};
  for (std::size_t index = 0; index < output.size(); ++index) {
    const auto key_byte = key.span()[index % key.size()];
    const auto input_byte = input.empty() ? std::byte{0} : input.span()[index % input.size()];
    output[index] = key_byte ^ input_byte ^ static_cast<std::byte>(index);
  }
  return laghu::core::Sha256Digest::from_array(output);
}

[[nodiscard]] bool fake_constant_time_equal(const laghu::core::Sha256Digest& left,
                                             const laghu::core::Sha256Digest& right) noexcept {
  ++fixture.compare_calls;
  std::uint8_t difference{};
  for (std::size_t index = 0; index < left.bytes().size(); ++index) {
    difference |= std::to_integer<std::uint8_t>(left.bytes()[index] ^ right.bytes()[index]);
  }
  return difference == 0;
}

[[nodiscard]] laghu::core::CryptoProvider provider() noexcept {
  return laghu::core::CryptoProvider{fake_sha256, fake_hmac_sha256, nullptr, nullptr,
                                     nullptr, fake_constant_time_equal, nullptr};
}

[[nodiscard]] bool check_public_digest() noexcept {
  constexpr std::array<std::byte, 3> bytes{std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}};
  const auto digest = laghu::core::PublicDigest::from_canonical_sanitized_bytes(
      provider(), view_of(bytes));
  constexpr std::array<char, 64> expected_hex{
      'a', 'b', 'c', 'c', 'e', 'd', 'a', '8', 'c', '9', 'e', 'a', 'a', 'd', 'c', 'a',
      'e', '7', 'a', '2', 'c', '7', 'e', '4', 'a', '7', 'c', '0', 'e', '1', 'a', '4',
      'd', 'd', 'f', 'e', 'b', '9', 'd', 'e', 'f', 'b', 'b', 'e', 'd', 'b', 'f', '8',
      'b', '3', 'd', '4', 'f', '5', 'b', '0', 'd', '1', 'f', '2', 'b', '5', 'd', '2'};
  return digest.has_value() && digest->lowercase_hex() == expected_hex &&
         digest->value().bytes()[0] == std::byte{0xAB} &&
         digest->value().bytes()[31] == std::byte{0xD2};
}

[[nodiscard]] bool check_boundaries_and_borrowing() noexcept {
  constexpr std::array<std::byte, 0> empty{};
  std::array<std::byte, 1> key{std::byte{0x11}};
  const auto key_id = laghu::core::internal::FingerprintKeyId::from_uint64(UINT64_MAX);
  const auto zero_key_id = laghu::core::internal::FingerprintKeyId::from_uint64(0);
  const auto public_empty = laghu::core::PublicDigest::from_canonical_sanitized_bytes(
      provider(), view_of(empty));
  if (!key_id.has_value()) {
    return false;
  }
  const auto first = laghu::core::internal::SecretFingerprint::from_canonical_sanitized_bytes(
      provider(), *key_id, view_of(key), view_of(empty));
  key[0] = std::byte{0x12};
  const auto after_key_mutation =
      laghu::core::internal::SecretFingerprint::from_canonical_sanitized_bytes(
          provider(), *key_id, view_of(key), view_of(empty));
  const auto empty_key = laghu::core::internal::SecretFingerprint::from_canonical_sanitized_bytes(
      provider(), *key_id, view_of(empty), view_of(empty));
  return public_empty.has_value() && !zero_key_id.has_value() &&
         zero_key_id.error().code() == laghu::core::ErrorCode::invalid_input &&
         first.has_value() && after_key_mutation.has_value() && !empty_key.has_value() &&
         empty_key.error().code() == laghu::core::ErrorCode::invalid_input &&
         !laghu::core::internal::constant_time_equal(provider(), *first, *after_key_mutation) &&
         sizeof(laghu::core::internal::SecretFingerprint) ==
             sizeof(laghu::core::Sha256Digest) + sizeof(laghu::core::internal::FingerprintKeyId);
}

[[nodiscard]] bool check_failures_and_comparison() noexcept {
  constexpr std::array<std::byte, 3> input{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  constexpr std::array<std::byte, 2> key{std::byte{0x01}, std::byte{0x02}};
  const auto key_id = laghu::core::internal::FingerprintKeyId::from_uint64(7);
  const auto different_key_id = laghu::core::internal::FingerprintKeyId::from_uint64(8);
  if (!key_id.has_value() || !different_key_id.has_value()) {
    return false;
  }

  fixture = {};
  const auto missing_sha = laghu::core::PublicDigest::from_canonical_sanitized_bytes(
      laghu::core::CryptoProvider{}, view_of(input));
  const auto missing_hmac = laghu::core::internal::SecretFingerprint::from_canonical_sanitized_bytes(
      laghu::core::CryptoProvider{}, *key_id, view_of(key), view_of(input));
  fixture.fail_sha256 = true;
  const auto failed_sha = laghu::core::PublicDigest::from_canonical_sanitized_bytes(
      provider(), view_of(input));
  fixture.fail_sha256 = false;
  fixture.fail_hmac = true;
  const auto failed_hmac = laghu::core::internal::SecretFingerprint::from_canonical_sanitized_bytes(
      provider(), *key_id, view_of(key), view_of(input));
  fixture.fail_hmac = false;
  const auto left = laghu::core::internal::SecretFingerprint::from_canonical_sanitized_bytes(
      provider(), *key_id, view_of(key), view_of(input));
  const auto right = laghu::core::internal::SecretFingerprint::from_canonical_sanitized_bytes(
      provider(), *key_id, view_of(key), view_of(input));
  const auto scoped_different = laghu::core::internal::SecretFingerprint::from_canonical_sanitized_bytes(
      provider(), *different_key_id, view_of(key), view_of(input));
  const std::size_t compares_before = fixture.compare_calls;
  const bool equal = left.has_value() && right.has_value() &&
                     laghu::core::internal::constant_time_equal(provider(), *left, *right);
  const bool mismatched_key = left.has_value() && scoped_different.has_value() &&
                              laghu::core::internal::constant_time_equal(
                                  provider(), *left, *scoped_different);
  return !missing_sha.has_value() && !missing_hmac.has_value() && !failed_sha.has_value() &&
         !failed_hmac.has_value() &&
         missing_sha.error().code() == laghu::core::ErrorCode::unavailable_capability &&
         missing_hmac.error().code() == laghu::core::ErrorCode::unavailable_capability &&
         failed_sha.error().native_code() == 41 && failed_hmac.error().native_code() == 42 &&
         equal && !mismatched_key && fixture.compare_calls == compares_before + 2;
}

static_assert(!std::is_constructible_v<laghu::core::internal::FingerprintKeyId, std::uint64_t>);
static_assert(!std::is_convertible_v<std::uint64_t, laghu::core::internal::FingerprintKeyId>);
static_assert(sizeof(laghu::core::PublicDigest) == 96);
static_assert(sizeof(laghu::core::internal::SecretFingerprint) == 40);

}  // namespace

int main() {
  if (!check_public_digest()) {
    return 1;
  }
  if (!check_boundaries_and_borrowing()) {
    return 2;
  }
  if (!check_failures_and_comparison()) {
    return 3;
  }
  return 0;
}
