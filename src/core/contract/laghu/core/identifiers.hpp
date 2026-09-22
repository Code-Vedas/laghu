// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include <laghu/core/contract.hpp>

namespace laghu::core {

namespace detail {

template <class Tag>
class StrongIdentifier final {
 public:
  static constexpr std::size_t encoded_size = 8;

  StrongIdentifier(const StrongIdentifier&) noexcept = default;
  StrongIdentifier& operator=(const StrongIdentifier&) noexcept = default;
  StrongIdentifier(StrongIdentifier&&) noexcept = default;
  StrongIdentifier& operator=(StrongIdentifier&&) noexcept = default;
  ~StrongIdentifier() = default;

  [[nodiscard]] static constexpr Result<StrongIdentifier> from_uint64(
      std::uint64_t value) noexcept {
    if (value == 0) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_input, 0,
                                   "strong identifier must be nonzero"}};
    }
    return StrongIdentifier{value};
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  [[nodiscard]] constexpr std::array<std::byte, encoded_size> encode_big_endian() const noexcept {
    std::array<std::byte, encoded_size> encoded{};
    for (std::size_t index = 0; index < encoded.size(); ++index) {
      const std::size_t shift = (encoded.size() - 1 - index) * CHAR_BIT;
      encoded[index] = static_cast<std::byte>(value_ >> shift);
    }
    return encoded;
  }

  [[nodiscard]] static constexpr Result<StrongIdentifier> decode_big_endian(
      std::span<const std::byte> encoded) noexcept {
    if (encoded.size() != encoded_size) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_range, 0,
                                   "strong identifier encoding must contain 8 bytes"}};
    }

    std::uint64_t value = 0;
    for (const std::byte byte : encoded) {
      value = (value << CHAR_BIT) | std::to_integer<std::uint8_t>(byte);
    }
    return from_uint64(value);
  }

 private:
  StrongIdentifier() = delete;
  explicit constexpr StrongIdentifier(std::uint64_t value) noexcept : value_(value) {}

  std::uint64_t value_;
};

}  // namespace detail

struct WorkerIdTag;
struct ConnectionIdTag;
struct RequestIdTag;
struct StreamIdTag;
struct RouteIdTag;
struct PolicyIdTag;
struct PeerIdTag;
struct GenerationIdTag;

using WorkerId = detail::StrongIdentifier<WorkerIdTag>;
using ConnectionId = detail::StrongIdentifier<ConnectionIdTag>;
using RequestId = detail::StrongIdentifier<RequestIdTag>;
using StreamId = detail::StrongIdentifier<StreamIdTag>;
using RouteId = detail::StrongIdentifier<RouteIdTag>;
using PolicyId = detail::StrongIdentifier<PolicyIdTag>;
using PeerId = detail::StrongIdentifier<PeerIdTag>;
using GenerationId = detail::StrongIdentifier<GenerationIdTag>;

template <class Identifier>
consteval bool has_strong_identifier_representation() noexcept {
  return sizeof(Identifier) == sizeof(std::uint64_t) && std::is_trivial_v<Identifier> &&
         std::is_trivially_copyable_v<Identifier> && std::is_standard_layout_v<Identifier>;
}

static_assert(CHAR_BIT == 8);
static_assert(has_strong_identifier_representation<WorkerId>());
static_assert(has_strong_identifier_representation<ConnectionId>());
static_assert(has_strong_identifier_representation<RequestId>());
static_assert(has_strong_identifier_representation<StreamId>());
static_assert(has_strong_identifier_representation<RouteId>());
static_assert(has_strong_identifier_representation<PolicyId>());
static_assert(has_strong_identifier_representation<PeerId>());
static_assert(has_strong_identifier_representation<GenerationId>());
static_assert(!std::is_convertible_v<std::uint64_t, WorkerId>);
static_assert(!std::is_convertible_v<WorkerId, std::uint64_t>);
static_assert(!std::is_convertible_v<WorkerId, ConnectionId>);

}  // namespace laghu::core
