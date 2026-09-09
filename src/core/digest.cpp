// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <string_view>

#include <laghu/core/digest.hpp>

namespace laghu::core {
namespace {

[[nodiscard]] constexpr char lowercase_hex_digit(std::byte value) noexcept {
  constexpr std::array<char, 16> digits{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  return digits[std::to_integer<unsigned char>(value)];
}

[[nodiscard]] constexpr std::array<char, PublicDigest::lowercase_hex_size> to_lowercase_hex(
    const Sha256Digest& value) noexcept {
  std::array<char, PublicDigest::lowercase_hex_size> output{};
  for (std::size_t index = 0; index < value.bytes().size(); ++index) {
    const auto byte = std::to_integer<unsigned char>(value.bytes()[index]);
    output[index * 2] = lowercase_hex_digit(static_cast<std::byte>(byte >> 4U));
    output[index * 2 + 1] = lowercase_hex_digit(static_cast<std::byte>(byte & 0x0FU));
  }
  return output;
}

[[nodiscard]] constexpr Error unavailable_provider_error(std::string_view diagnostic) noexcept {
  return Error{ErrorDomain::crypto, ErrorCode::unavailable_capability, 0, diagnostic};
}

}  // namespace

Result<PublicDigest> PublicDigest::from_canonical_sanitized_bytes(
    const CryptoProvider& provider, ByteView canonical_sanitized_bytes) noexcept {
  if (provider.sha256 == nullptr) {
    return std::unexpected{unavailable_provider_error("SHA-256 provider is unavailable")};
  }
  const auto value = provider.sha256(canonical_sanitized_bytes);
  if (!value.has_value()) {
    return std::unexpected{value.error()};
  }
  return PublicDigest{*value, to_lowercase_hex(*value)};
}

}  // namespace laghu::core
