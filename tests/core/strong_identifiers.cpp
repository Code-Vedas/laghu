// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <span>
#include <type_traits>

#include <laghu/core/identifiers.hpp>

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

[[nodiscard]] bool check(bool condition) noexcept { return condition; }

template <class Identifier>
[[nodiscard]] bool check_boundaries() noexcept {
  const auto minimum = Identifier::from_uint64(1);
  const auto maximum = Identifier::from_uint64(UINT64_MAX);
  if (!minimum.has_value() || !maximum.has_value() || minimum->value() != 1 ||
      maximum->value() != UINT64_MAX) {
    return false;
  }

  const auto decoded_minimum = Identifier::decode_big_endian(minimum->encode_big_endian());
  const auto decoded_maximum = Identifier::decode_big_endian(maximum->encode_big_endian());
  return decoded_minimum.has_value() && decoded_maximum.has_value() &&
         decoded_minimum->value() == 1 && decoded_maximum->value() == UINT64_MAX;
}

static_assert(std::is_trivial_v<laghu::core::WorkerId>);
static_assert(std::is_trivially_copyable_v<laghu::core::WorkerId>);
static_assert(std::is_standard_layout_v<laghu::core::WorkerId>);
static_assert(sizeof(laghu::core::WorkerId) == sizeof(std::uint64_t));
static_assert(!std::is_constructible_v<laghu::core::WorkerId, std::uint64_t>);
static_assert(!std::is_convertible_v<std::uint64_t, laghu::core::WorkerId>);
static_assert(!std::is_convertible_v<laghu::core::WorkerId, std::uint64_t>);
static_assert(!std::is_convertible_v<laghu::core::WorkerId, laghu::core::ConnectionId>);
static_assert(!std::is_convertible_v<laghu::core::ConnectionId, laghu::core::WorkerId>);

}  // namespace

int main() {
  const std::size_t allocation_attempts_before = allocation_attempts;
  const auto worker = laghu::core::WorkerId::from_uint64(UINT64_C(0x0102030405060708));
  if (!check(worker.has_value())) {
    return 1;
  }

  constexpr std::array<std::byte, 8> golden_bytes{
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
      std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};
  if (!check(worker->encode_big_endian() == golden_bytes)) {
    return 2;
  }

  const auto decoded = laghu::core::WorkerId::decode_big_endian(golden_bytes);
  if (!check(decoded.has_value() && decoded->value() == worker->value())) {
    return 3;
  }

  constexpr std::array<std::byte, 8> zero_bytes{};
  const auto zero_raw = laghu::core::WorkerId::from_uint64(0);
  const auto zero_encoded = laghu::core::WorkerId::decode_big_endian(zero_bytes);
  if (!check(!zero_raw.has_value() && !zero_encoded.has_value() &&
             zero_raw.error().code() == laghu::core::ErrorCode::invalid_input &&
             zero_encoded.error().code() == laghu::core::ErrorCode::invalid_input)) {
    return 4;
  }

  constexpr std::array<std::byte, 7> short_bytes{};
  constexpr std::array<std::byte, 9> long_bytes{};
  const auto short_decode = laghu::core::WorkerId::decode_big_endian(short_bytes);
  const auto long_decode = laghu::core::WorkerId::decode_big_endian(long_bytes);
  if (!check(!short_decode.has_value() && !long_decode.has_value() &&
             short_decode.error().code() == laghu::core::ErrorCode::invalid_range &&
             long_decode.error().code() == laghu::core::ErrorCode::invalid_range)) {
    return 5;
  }

  if (!check(check_boundaries<laghu::core::WorkerId>() &&
             check_boundaries<laghu::core::ConnectionId>() &&
             check_boundaries<laghu::core::RequestId>() &&
             check_boundaries<laghu::core::StreamId>() &&
             check_boundaries<laghu::core::RouteId>() &&
             check_boundaries<laghu::core::PolicyId>() &&
             check_boundaries<laghu::core::PeerId>() &&
             check_boundaries<laghu::core::GenerationId>())) {
    return 6;
  }

  if (!check(allocation_attempts == allocation_attempts_before)) {
    return 7;
  }

  return 0;
}
