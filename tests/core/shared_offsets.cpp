// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <type_traits>

#include <laghu/core/shared_offsets.hpp>

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

using laghu::core::ByteView;
using laghu::core::ErrorCode;
using laghu::core::MutableByteView;
using laghu::core::SharedOffset;
using laghu::core::SharedSchema;

struct NodeTag;
struct OtherNodeTag;
using NodeOffset = SharedOffset<NodeTag>;
using OtherNodeOffset = SharedOffset<OtherNodeTag>;
using NodeSchema = SharedSchema<std::uint32_t, NodeOffset>;

struct Node final {
  std::uint32_t value{};
  NodeOffset next{};
};

struct Mapping final {
  std::array<std::byte, 16> prefix{};
  alignas(Node) Node node{};
  std::array<std::byte, 16> suffix{};
};

[[nodiscard]] constexpr bool check(bool condition) noexcept { return condition; }

template <laghu::core::BigEndianFixedWidth T, std::size_t N>
[[nodiscard]] bool check_codec(T value, const std::array<std::byte, N>& golden) noexcept {
  static_assert(N == sizeof(T));
  const auto encoded = laghu::core::encode_big_endian(value);
  if (!check(encoded == golden)) {
    return false;
  }
  const auto bytes = ByteView::from(std::span<const std::byte>{encoded});
  if (!bytes.has_value()) {
    return false;
  }
  const auto decoded = laghu::core::decode_big_endian<T>(*bytes);
  return decoded.has_value() && *decoded == value;
}

[[nodiscard]] bool check_codecs() noexcept {
  constexpr std::array<std::byte, 2> u16_golden{std::byte{0x01}, std::byte{0x02}};
  constexpr std::array<std::byte, 4> u32_golden{
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
  constexpr std::array<std::byte, 8> u64_golden{
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
      std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};
  if (!check(check_codec<std::uint16_t>(UINT16_C(0x0102), u16_golden)) ||
      !check(check_codec<std::uint32_t>(UINT32_C(0x01020304), u32_golden)) ||
      !check(check_codec<std::uint64_t>(UINT64_C(0x0102030405060708), u64_golden))) {
    return false;
  }

  const auto min_encoded = laghu::core::encode_big_endian(std::uint64_t{0});
  const auto max_encoded = laghu::core::encode_big_endian(UINT64_MAX);
  const auto min_view = ByteView::from(std::span<const std::byte>{min_encoded});
  const auto max_view = ByteView::from(std::span<const std::byte>{max_encoded});
  if (!check(min_view.has_value() && max_view.has_value())) {
    return false;
  }
  const auto minimum = laghu::core::decode_big_endian<std::uint64_t>(*min_view);
  const auto maximum = laghu::core::decode_big_endian<std::uint64_t>(*max_view);
  if (!check(minimum.has_value() && *minimum == 0 && maximum.has_value() && *maximum == UINT64_MAX)) {
    return false;
  }

  constexpr std::array<std::byte, 1> short_u16{};
  constexpr std::array<std::byte, 3> short_u32{};
  constexpr std::array<std::byte, 5> long_u32{};
  constexpr std::array<std::byte, 7> short_u64{};
  const auto u16_view = ByteView::from(std::span<const std::byte>{short_u16});
  const auto u32_view = ByteView::from(std::span<const std::byte>{short_u32});
  const auto long_u32_view = ByteView::from(std::span<const std::byte>{long_u32});
  const auto u64_view = ByteView::from(std::span<const std::byte>{short_u64});
  const auto u16_decode = laghu::core::decode_big_endian<std::uint16_t>(*u16_view);
  const auto u32_decode = laghu::core::decode_big_endian<std::uint32_t>(*u32_view);
  const auto long_u32_decode = laghu::core::decode_big_endian<std::uint32_t>(*long_u32_view);
  const auto u64_decode = laghu::core::decode_big_endian<std::uint64_t>(*u64_view);
  return check(!u16_decode.has_value() && u16_decode.error().code() == ErrorCode::invalid_range &&
               !u32_decode.has_value() && u32_decode.error().code() == ErrorCode::invalid_range &&
               !long_u32_decode.has_value() &&
               long_u32_decode.error().code() == ErrorCode::invalid_range &&
               !u64_decode.has_value() && u64_decode.error().code() == ErrorCode::invalid_range);
}

[[nodiscard]] bool check_offsets() noexcept {
  Mapping first{};
  Mapping second{};
  first.node.value = 7;
  second.node.value = 11;
  const auto first_mapping = MutableByteView::from(std::as_writable_bytes(std::span{&first, 1}));
  const auto second_mapping = MutableByteView::from(std::as_writable_bytes(std::span{&second, 1}));
  if (!check(first_mapping.has_value() && second_mapping.has_value())) {
    return false;
  }
  if (!check(first_mapping->data() != second_mapping->data())) {
    return false;
  }

  const auto node_offset = NodeOffset::from_raw(
      static_cast<std::uint64_t>(reinterpret_cast<std::byte*>(&first.node) -
                                 reinterpret_cast<std::byte*>(&first)));
  if (!check(!node_offset.is_null() && node_offset.raw() == 16)) {
    return false;
  }
  const auto const_node = node_offset.resolve<Node>(first_mapping->as_const());
  const auto mutable_node = node_offset.resolve<Node>(*second_mapping);
  if (!check(const_node.has_value() && *const_node == &first.node && (*const_node)->value == 7 &&
             mutable_node.has_value() && *mutable_node == &second.node && (*mutable_node)->value == 11)) {
    return false;
  }
  (*mutable_node)->value = 19;
  if (!check(second.node.value == 19)) {
    return false;
  }

  const auto null_offset = NodeOffset::from_raw(0);
  const auto null_node = null_offset.resolve<Node>(first_mapping->as_const());
  if (!check(null_offset.is_null() && null_node.has_value() && *null_node == nullptr)) {
    return false;
  }

  const auto out_of_bounds = NodeOffset::from_raw(static_cast<std::uint64_t>(first_mapping->size()));
  const auto overflow = NodeOffset::from_raw(UINT64_MAX);
  const auto misaligned = NodeOffset::from_raw(17);
  const auto out_of_bounds_node = out_of_bounds.resolve<Node>(first_mapping->as_const());
  const auto overflow_node = overflow.resolve<Node>(first_mapping->as_const());
  const auto misaligned_node = misaligned.resolve<Node>(first_mapping->as_const());
  return check(!out_of_bounds_node.has_value() &&
               out_of_bounds_node.error().code() == ErrorCode::invalid_range &&
               !overflow_node.has_value() &&
               (overflow_node.error().code() == ErrorCode::overflow ||
                overflow_node.error().code() == ErrorCode::invalid_range) &&
               !misaligned_node.has_value() && misaligned_node.error().code() == ErrorCode::invalid_range);
}

static_assert(sizeof(NodeOffset) == sizeof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<NodeOffset>);
static_assert(std::is_standard_layout_v<NodeOffset>);
static_assert(!std::is_convertible_v<NodeOffset, OtherNodeOffset>);
static_assert(!std::is_convertible_v<std::uint64_t, NodeOffset>);
static_assert(NodeSchema::field_count == 2);

}  // namespace

int main() {
  const std::size_t allocation_attempts_before = allocation_attempts;
  if (!check(check_codecs())) {
    return 1;
  }
  if (!check(check_offsets())) {
    return 2;
  }
  return allocation_attempts == allocation_attempts_before ? 0 : 3;
}
