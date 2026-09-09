// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>

#include <laghu/core/binary_envelope.hpp>

std::size_t allocation_attempts{};

void* operator new(std::size_t size) {
  ++allocation_attempts;
  if (void* memory = std::malloc(size); memory != nullptr) {
    return memory;
  }
  std::abort();
}

void* operator new[](std::size_t size) {
  ++allocation_attempts;
  if (void* memory = std::malloc(size); memory != nullptr) {
    return memory;
  }
  std::abort();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

using laghu::core::BinaryEnvelope;
using laghu::core::ByteView;
using laghu::core::ErrorCode;
using laghu::core::MutableByteView;

constexpr std::array<std::byte, 3> payload_bytes{
    std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
constexpr std::array<std::byte, 19> golden_envelope{
    std::byte{'L'}, std::byte{'G'}, std::byte{'H'}, std::byte{'U'},
    std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x02},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x03},
    std::byte{0xE2}, std::byte{0xE0}, std::byte{0xEC}, std::byte{0x34},
    std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};

[[nodiscard]] constexpr bool check(bool condition) noexcept { return condition; }

template <std::size_t N>
[[nodiscard]] laghu::core::Result<ByteView> bytes_from(
    const std::array<std::byte, N>& bytes) noexcept {
  return ByteView::from(std::span<const std::byte>{bytes});
}

template <std::size_t N>
[[nodiscard]] laghu::core::Result<MutableByteView> mutable_bytes_from(
    std::array<std::byte, N>& bytes) noexcept {
  return MutableByteView::from(std::span<std::byte>{bytes});
}

[[nodiscard]] bool same_envelope(const BinaryEnvelope& left,
                                  const BinaryEnvelope& right) noexcept {
  if (left.kind() != right.kind() || left.payload().size() != right.payload().size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.payload().size(); ++index) {
    if (left.payload().span()[index] != right.payload().span()[index]) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool check_crc32c_vectors() noexcept {
  constexpr std::array<std::byte, 9> standard{
      std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'},
      std::byte{'6'}, std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};
  const auto standard_view = bytes_from(standard);
  const auto header_view = bytes_from(golden_envelope);
  if (!standard_view.has_value() || !header_view.has_value()) {
    return false;
  }
  const auto envelope_header = header_view->slice(0, 12);
  const auto envelope_payload = header_view->slice(16, 3);
  return check(laghu::core::crc32c_castagnoli(*standard_view) == UINT32_C(0xE3069283) &&
               envelope_header.has_value() && envelope_payload.has_value() &&
               laghu::core::crc32c_castagnoli(*envelope_header, *envelope_payload) ==
                   UINT32_C(0xE2E0EC34));
}

[[nodiscard]] bool check_encoding_and_decoding() noexcept {
  const auto payload = bytes_from(payload_bytes);
  if (!payload.has_value()) {
    return false;
  }
  const auto envelope = BinaryEnvelope::from_parts(2, *payload);
  if (!envelope.has_value()) {
    return false;
  }
  std::array<std::byte, golden_envelope.size()> encoded{};
  std::array<std::byte, golden_envelope.size()> repeated{};
  const auto output = mutable_bytes_from(encoded);
  const auto repeated_output = mutable_bytes_from(repeated);
  if (!output.has_value() || !repeated_output.has_value()) {
    return false;
  }
  const auto first_encode = laghu::core::encode_binary_envelope(*envelope, *output);
  const auto second_encode = laghu::core::encode_binary_envelope(*envelope, *repeated_output);
  if (!check(first_encode.has_value() && *first_encode == golden_envelope.size() &&
             second_encode.has_value() && *second_encode == golden_envelope.size() &&
             encoded == golden_envelope && repeated == golden_envelope)) {
    return false;
  }

  const auto encoded_input = bytes_from(encoded);
  if (!encoded_input.has_value()) {
    return false;
  }
  BinaryEnvelope decoded;
  const auto decode = laghu::core::decode_binary_envelope(*encoded_input, payload_bytes.size(), decoded);
  return check(decode.has_value() && same_envelope(*envelope, decoded));
}

[[nodiscard]] bool check_empty_and_boundary_payloads() noexcept {
  constexpr std::array<std::byte, 0> empty_payload{};
  const auto empty = bytes_from(empty_payload);
  if (!empty.has_value()) {
    return false;
  }
  const auto envelope = BinaryEnvelope::from_parts(UINT16_MAX, *empty);
  if (!envelope.has_value()) {
    return false;
  }
  std::array<std::byte, 16> output{};
  const auto mutable_output = mutable_bytes_from(output);
  if (!mutable_output.has_value()) {
    return false;
  }
  const auto encode = laghu::core::encode_binary_envelope(*envelope, *mutable_output);
  const auto input = bytes_from(output);
  BinaryEnvelope decoded;
  const auto decode = input.has_value()
                          ? laghu::core::decode_binary_envelope(*input, 0, decoded)
                          : laghu::core::Result<void>{std::unexpected{input.error()}};
  return check(encode.has_value() && *encode == output.size() &&
               output[4] == std::byte{0x00} && output[5] == std::byte{0x01} &&
               output[6] == std::byte{0xFF} && output[7] == std::byte{0xFF} &&
               output[8] == std::byte{0x00} && output[9] == std::byte{0x00} &&
               output[10] == std::byte{0x00} && output[11] == std::byte{0x00} &&
               output[12] == std::byte{0x11} && output[13] == std::byte{0x90} &&
               output[14] == std::byte{0x01} && output[15] == std::byte{0x43} &&
               decode.has_value() && same_envelope(*envelope, decoded));
}

[[nodiscard]] bool check_rejections_and_failure_atomicity() noexcept {
  const auto payload = bytes_from(payload_bytes);
  if (!payload.has_value()) {
    return false;
  }
  const auto sentinel = BinaryEnvelope::from_parts(9, *payload);
  if (!sentinel.has_value()) {
    return false;
  }
  const auto golden = bytes_from(golden_envelope);
  if (!golden.has_value()) {
    return false;
  }

  auto expect_failure = [&](std::span<const std::byte> input, std::size_t maximum,
                            ErrorCode expected) noexcept {
    BinaryEnvelope output = *sentinel;
    const auto view = ByteView::from(input);
    if (!view.has_value()) {
      return false;
    }
    const auto decoded = laghu::core::decode_binary_envelope(*view, maximum, output);
    return !decoded.has_value() && decoded.error().code() == expected &&
           same_envelope(output, *sentinel);
  };

  std::array<std::byte, golden_envelope.size()> corrupt_magic = golden_envelope;
  std::array<std::byte, golden_envelope.size()> unknown_version = golden_envelope;
  std::array<std::byte, golden_envelope.size()> zero_kind = golden_envelope;
  std::array<std::byte, golden_envelope.size()> truncated_payload = golden_envelope;
  std::array<std::byte, golden_envelope.size()> corrupt_checksum = golden_envelope;
  std::array<std::byte, golden_envelope.size()> corrupt_payload = golden_envelope;
  std::array<std::byte, golden_envelope.size() + 1> trailing{};
  corrupt_magic[0] = std::byte{'X'};
  unknown_version[5] = std::byte{0x02};
  zero_kind[6] = std::byte{0x00};
  zero_kind[7] = std::byte{0x00};
  truncated_payload[11] = std::byte{0x04};
  corrupt_checksum[15] ^= std::byte{0x01};
  corrupt_payload[18] ^= std::byte{0x01};
  for (std::size_t index = 0; index < golden_envelope.size(); ++index) {
    trailing[index] = golden_envelope[index];
  }
  trailing.back() = std::byte{0x00};

  std::array<std::byte, 15> short_header{};
  std::array<std::byte, 18> short_payload{};
  for (std::size_t index = 0; index < short_payload.size(); ++index) {
    short_payload[index] = golden_envelope[index];
  }
  const auto zero_kind_construct = BinaryEnvelope::from_parts(0, *payload);
  return check(!zero_kind_construct.has_value() &&
               zero_kind_construct.error().code() == ErrorCode::invalid_input &&
               expect_failure(std::span<const std::byte>{short_header}, 3, ErrorCode::corrupt_data) &&
               expect_failure(std::span<const std::byte>{short_payload}, 3, ErrorCode::corrupt_data) &&
               expect_failure(std::span<const std::byte>{corrupt_magic}, 3, ErrorCode::corrupt_data) &&
               expect_failure(std::span<const std::byte>{unknown_version}, 3,
                              ErrorCode::unsupported_version) &&
               expect_failure(std::span<const std::byte>{zero_kind}, 3, ErrorCode::invalid_input) &&
               expect_failure(std::span<const std::byte>{truncated_payload}, 4,
                              ErrorCode::corrupt_data) &&
               expect_failure(std::span<const std::byte>{trailing}, 3, ErrorCode::corrupt_data) &&
               expect_failure(std::span<const std::byte>{corrupt_checksum}, 3,
                              ErrorCode::checksum) &&
               expect_failure(std::span<const std::byte>{corrupt_payload}, 3,
                              ErrorCode::checksum) &&
               expect_failure(golden->span(), 2, ErrorCode::invalid_range));
}

[[nodiscard]] bool check_encode_rejections() noexcept {
  const auto payload = bytes_from(payload_bytes);
  if (!payload.has_value()) {
    return false;
  }
  const auto envelope = BinaryEnvelope::from_parts(2, *payload);
  if (!envelope.has_value()) {
    return false;
  }
  std::array<std::byte, 18> undersized{};
  undersized.fill(std::byte{0xA5});
  const auto output = mutable_bytes_from(undersized);
  if (!output.has_value()) {
    return false;
  }
  const auto encode = laghu::core::encode_binary_envelope(*envelope, *output);
  const auto normal_size = laghu::core::binary_envelope_encoded_size(payload_bytes.size());
  if (!check(!encode.has_value() && encode.error().code() == ErrorCode::invalid_range &&
             normal_size.has_value() && *normal_size == golden_envelope.size())) {
    return false;
  }
  for (const std::byte byte : undersized) {
    if (byte != std::byte{0xA5}) {
      return false;
    }
  }
  if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
    const auto oversized = laghu::core::binary_envelope_encoded_size(
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1U);
    return check(!oversized.has_value() && oversized.error().code() == ErrorCode::overflow);
  }
  return true;
}

}  // namespace

int main() {
  const std::size_t allocation_attempts_before = allocation_attempts;
  if (!check(check_crc32c_vectors())) {
    return 1;
  }
  if (!check(check_encoding_and_decoding())) {
    return 2;
  }
  if (!check(check_empty_and_boundary_payloads())) {
    return 3;
  }
  if (!check(check_rejections_and_failure_atomicity())) {
    return 4;
  }
  if (!check(check_encode_rejections())) {
    return 5;
  }
  return allocation_attempts == allocation_attempts_before ? 0 : 6;
}
