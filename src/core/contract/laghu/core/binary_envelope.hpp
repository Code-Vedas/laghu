// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include <laghu/core/shared_offsets.hpp>

namespace laghu::core {

inline constexpr std::size_t binary_envelope_header_size = 16;
inline constexpr std::uint16_t binary_envelope_version = 1;

class BinaryEnvelope final {
 public:
  constexpr BinaryEnvelope() noexcept = default;

  [[nodiscard]] static constexpr Result<BinaryEnvelope> from_parts(
      std::uint16_t kind, ByteView payload) noexcept {
    if (kind == 0) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_input, 0,
                                   "binary envelope kind must be nonzero"}};
    }
    return BinaryEnvelope{kind, payload};
  }

  [[nodiscard]] constexpr std::uint16_t kind() const noexcept { return kind_; }
  [[nodiscard]] constexpr ByteView payload() const noexcept { return payload_; }

 private:
  constexpr BinaryEnvelope(std::uint16_t kind, ByteView payload) noexcept
      : kind_(kind), payload_(payload) {}

  std::uint16_t kind_{};
  ByteView payload_{};
};

namespace detail {

[[nodiscard]] constexpr Error binary_envelope_error(ErrorCode code,
                                                     std::string_view diagnostic) noexcept {
  return Error{ErrorDomain::core, code, 0, diagnostic};
}

[[nodiscard]] constexpr std::uint32_t crc32c_extend(std::uint32_t state,
                                                     ByteView bytes) noexcept {
  for (const std::byte byte : bytes.span()) {
    state ^= std::to_integer<std::uint8_t>(byte);
    for (std::uint8_t bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - (state & 1U);
      state = (state >> 1U) ^ (UINT32_C(0x82F63B78) & mask);
    }
  }
  return state;
}

}  // namespace detail

[[nodiscard]] constexpr std::uint32_t crc32c_castagnoli(ByteView bytes) noexcept {
  return detail::crc32c_extend(UINT32_MAX, bytes) ^ UINT32_MAX;
}

[[nodiscard]] constexpr std::uint32_t crc32c_castagnoli(ByteView first,
                                                         ByteView second) noexcept {
  return detail::crc32c_extend(detail::crc32c_extend(UINT32_MAX, first), second) ^ UINT32_MAX;
}

[[nodiscard]] constexpr Result<std::size_t> binary_envelope_encoded_size(
    std::size_t payload_size) noexcept {
  if (payload_size > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected{detail::binary_envelope_error(
        ErrorCode::overflow, "binary envelope payload exceeds the 32-bit wire length")};
  }
  return checked_add(binary_envelope_header_size, payload_size);
}

[[nodiscard]] constexpr Result<std::size_t> encode_binary_envelope(
    const BinaryEnvelope& envelope, MutableByteView output) noexcept {
  if (envelope.kind() == 0) {
    return std::unexpected{detail::binary_envelope_error(
        ErrorCode::invalid_input, "binary envelope kind must be nonzero")};
  }
  const auto encoded_size = binary_envelope_encoded_size(envelope.payload().size());
  if (!encoded_size.has_value()) {
    return std::unexpected{encoded_size.error()};
  }
  if (output.size() < *encoded_size) {
    return std::unexpected{detail::binary_envelope_error(
        ErrorCode::invalid_range, "binary envelope output is too small")};
  }

  std::array<std::byte, binary_envelope_header_size> header{
      std::byte{'L'}, std::byte{'G'}, std::byte{'H'}, std::byte{'U'}};
  const auto version = encode_big_endian(binary_envelope_version);
  const auto kind = encode_big_endian(envelope.kind());
  const auto payload_length =
      encode_big_endian(static_cast<std::uint32_t>(envelope.payload().size()));
  for (std::size_t index = 0; index < version.size(); ++index) {
    header[4 + index] = version[index];
    header[6 + index] = kind[index];
  }
  for (std::size_t index = 0; index < payload_length.size(); ++index) {
    header[8 + index] = payload_length[index];
  }

  const auto header_without_checksum =
      ByteView::from(std::span<const std::byte>{header}.first(12));
  if (!header_without_checksum.has_value()) {
    return std::unexpected{header_without_checksum.error()};
  }
  const auto checksum = crc32c_castagnoli(*header_without_checksum, envelope.payload());
  const auto encoded_checksum = encode_big_endian(checksum);
  for (std::size_t index = 0; index < encoded_checksum.size(); ++index) {
    header[12 + index] = encoded_checksum[index];
  }

  for (std::size_t index = 0; index < header.size(); ++index) {
    output.span()[index] = header[index];
  }
  for (std::size_t index = 0; index < envelope.payload().size(); ++index) {
    output.span()[binary_envelope_header_size + index] = envelope.payload().span()[index];
  }
  return *encoded_size;
}

[[nodiscard]] constexpr Result<void> decode_binary_envelope(ByteView input,
                                                             std::size_t maximum_payload_size,
                                                             BinaryEnvelope& output) noexcept {
  if (input.size() < binary_envelope_header_size) {
    return std::unexpected{detail::binary_envelope_error(
        ErrorCode::corrupt_data, "binary envelope is shorter than its header")};
  }
  const auto header = input.slice(0, binary_envelope_header_size);
  if (!header.has_value()) {
    return std::unexpected{header.error()};
  }
  const auto header_bytes = header->span();
  if (header_bytes[0] != std::byte{'L'} || header_bytes[1] != std::byte{'G'} ||
      header_bytes[2] != std::byte{'H'} || header_bytes[3] != std::byte{'U'}) {
    return std::unexpected{detail::binary_envelope_error(
        ErrorCode::corrupt_data, "binary envelope magic is invalid")};
  }

  const auto encoded_version = header->slice(4, 2);
  const auto encoded_kind = header->slice(6, 2);
  const auto encoded_length = header->slice(8, 4);
  const auto encoded_checksum = header->slice(12, 4);
  if (!encoded_version.has_value() || !encoded_kind.has_value() || !encoded_length.has_value() ||
      !encoded_checksum.has_value()) {
    return std::unexpected{detail::binary_envelope_error(
        ErrorCode::corrupt_data, "binary envelope header is malformed")};
  }
  const auto version = decode_big_endian<std::uint16_t>(*encoded_version);
  const auto kind = decode_big_endian<std::uint16_t>(*encoded_kind);
  const auto payload_length = decode_big_endian<std::uint32_t>(*encoded_length);
  const auto checksum = decode_big_endian<std::uint32_t>(*encoded_checksum);
  if (!version.has_value() || !kind.has_value() || !payload_length.has_value() ||
      !checksum.has_value()) {
    return std::unexpected{detail::binary_envelope_error(
        ErrorCode::corrupt_data, "binary envelope fields are malformed")};
  }
  if (*version != binary_envelope_version) {
    return std::unexpected{detail::binary_envelope_error(
        ErrorCode::unsupported_version, "binary envelope version is unsupported")};
  }
  if (*kind == 0) {
    return std::unexpected{detail::binary_envelope_error(
        ErrorCode::invalid_input, "binary envelope kind must be nonzero")};
  }
  const auto payload_size = checked_narrow<std::size_t>(*payload_length);
  if (!payload_size.has_value()) {
    return std::unexpected{payload_size.error()};
  }
  if (*payload_size > maximum_payload_size) {
    return std::unexpected{detail::binary_envelope_error(
        ErrorCode::invalid_range, "binary envelope payload exceeds the caller maximum")};
  }
  const auto expected_size = binary_envelope_encoded_size(*payload_size);
  if (!expected_size.has_value()) {
    return std::unexpected{expected_size.error()};
  }
  if (input.size() < *expected_size) {
    return std::unexpected{detail::binary_envelope_error(
        ErrorCode::corrupt_data, "binary envelope payload is truncated")};
  }
  if (input.size() > *expected_size) {
    return std::unexpected{detail::binary_envelope_error(
        ErrorCode::corrupt_data, "binary envelope has trailing bytes")};
  }
  const auto payload = input.slice(binary_envelope_header_size, *payload_size);
  const auto checksum_header = input.slice(0, 12);
  if (!payload.has_value() || !checksum_header.has_value()) {
    return std::unexpected{detail::binary_envelope_error(
        ErrorCode::corrupt_data, "binary envelope payload range is invalid")};
  }
  if (crc32c_castagnoli(*checksum_header, *payload) != *checksum) {
    return std::unexpected{detail::binary_envelope_error(
        ErrorCode::checksum, "binary envelope checksum does not match")};
  }
  const auto decoded = BinaryEnvelope::from_parts(*kind, *payload);
  if (!decoded.has_value()) {
    return std::unexpected{decoded.error()};
  }
  output = *decoded;
  return {};
}

static_assert(binary_envelope_header_size == 16);

}  // namespace laghu::core
