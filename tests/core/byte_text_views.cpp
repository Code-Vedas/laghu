// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <type_traits>

#include <laghu/core/views.hpp>

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

[[nodiscard]] constexpr bool check(bool condition) noexcept { return condition; }

static_assert(std::is_trivially_copyable_v<laghu::core::ByteView>);
static_assert(std::is_trivially_copyable_v<laghu::core::MutableByteView>);
static_assert(std::is_trivially_copyable_v<laghu::core::TextView>);
static_assert(std::is_standard_layout_v<laghu::core::ByteView>);
static_assert(std::is_standard_layout_v<laghu::core::MutableByteView>);
static_assert(std::is_standard_layout_v<laghu::core::TextView>);
static_assert(laghu::core::is_borrowed_view_v<laghu::core::ByteView>);
static_assert(laghu::core::is_borrowed_view_v<const laghu::core::MutableByteView&>);
static_assert(laghu::core::is_borrowed_view_v<laghu::core::TextView>);
static_assert(!laghu::core::is_borrowed_view_v<laghu::core::StaticCString<8>>);
static_assert(!laghu::core::PersistentSchemaField<laghu::core::ByteView>);
static_assert(!laghu::core::PersistentSchemaField<laghu::core::MutableByteView>);
static_assert(!laghu::core::PersistentSchemaField<laghu::core::TextView>);
static_assert(laghu::core::PersistentSchemaField<laghu::core::StaticCString<8>>);

constexpr std::array<std::byte, 3> constexpr_bytes{std::byte{0x10}, std::byte{0x20},
                                                     std::byte{0x30}};
constexpr auto constexpr_byte_view = laghu::core::ByteView::from(constexpr_bytes);
constexpr auto constexpr_byte_slice = constexpr_byte_view->slice(1, 2);
constexpr auto constexpr_text_view = laghu::core::TextView::from("abc");
constexpr auto constexpr_text_slice = constexpr_text_view.slice(1, 2);
constexpr auto constexpr_c_string = constexpr_text_view.to_c_string<4>();
static_assert(constexpr_byte_slice.has_value() && constexpr_byte_slice->size() == 2 &&
              constexpr_byte_slice->span()[0] == std::byte{0x20});
static_assert(constexpr_text_slice.has_value() && constexpr_text_slice->string_view() == "bc");
static_assert(constexpr_c_string.has_value() && constexpr_c_string->view() == "abc");

}  // namespace

int main() {
  const std::size_t allocation_attempts_before = allocation_attempts;
  std::array<std::byte, 4> bytes{std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
                                 std::byte{0x40}};
  const auto byte_view = laghu::core::ByteView::from(bytes);
  const auto mutable_view = laghu::core::MutableByteView::from(bytes);
  if (!check(byte_view.has_value() && mutable_view.has_value() &&
             byte_view->size() == bytes.size() && !byte_view->empty() &&
             mutable_view->size() == bytes.size())) {
    return 1;
  }

  const auto byte_slice = byte_view->slice(1, 2);
  const auto mutable_slice = mutable_view->slice(1, 2);
  if (!check(byte_slice.has_value() && mutable_slice.has_value() &&
             byte_slice->span()[0] == std::byte{0x20} && mutable_slice->span()[1] == std::byte{0x30})) {
    return 2;
  }
  mutable_slice->span()[0] = std::byte{0x7f};
  if (!check(bytes[1] == std::byte{0x7f} && mutable_view->as_const().span()[1] == std::byte{0x7f})) {
    return 3;
  }

  const auto empty_bytes = laghu::core::ByteView::from(std::span<const std::byte>{});
  const auto empty_mutable_bytes = laghu::core::MutableByteView::from(std::span<std::byte>{});
  const auto empty_byte_slice = empty_bytes->slice(0, 0);
  if (!check(empty_bytes.has_value() && empty_mutable_bytes.has_value() && empty_bytes->empty() &&
             empty_mutable_bytes->empty() && empty_byte_slice.has_value() &&
             empty_byte_slice->empty())) {
    return 4;
  }

  const auto byte_oob = byte_view->slice(3, 2);
  const auto byte_overflow = byte_view->slice(std::numeric_limits<std::size_t>::max(), 1);
  if (!check(!byte_oob.has_value() && !byte_overflow.has_value() &&
             byte_oob.error().code() == laghu::core::ErrorCode::invalid_range &&
             byte_overflow.error().code() == laghu::core::ErrorCode::invalid_range)) {
    return 5;
  }

  const auto text_view = laghu::core::TextView::from(std::string_view{"hello"});
  const auto text_slice = text_view.slice(1, 3);
  const auto text_c_string = text_slice->to_c_string<4>();
  if (!check(text_slice.has_value() && text_slice->string_view() == "ell" && text_c_string.has_value() &&
             text_c_string->view() == "ell" && text_c_string->capacity() == 3 &&
             std::char_traits<char>::length(text_c_string->c_str()) == 3)) {
    return 6;
  }

  const std::array<char, 3> embedded_nul{'a', '\0', 'b'};
  const auto nul_text = laghu::core::TextView::from(std::string_view{embedded_nul.data(), embedded_nul.size()});
  const auto nul_c_string = nul_text.to_c_string<4>();
  const auto insufficient_capacity = text_view.to_c_string<5>();
  const auto empty_c_string = laghu::core::TextView{}.to_c_string<1>();
  if (!check(!nul_c_string.has_value() && !insufficient_capacity.has_value() && empty_c_string.has_value() &&
             nul_c_string.error().code() == laghu::core::ErrorCode::invalid_input &&
             insufficient_capacity.error().code() == laghu::core::ErrorCode::invalid_range &&
             empty_c_string->size() == 0 && empty_c_string->view().empty() &&
             std::char_traits<char>::length(empty_c_string->c_str()) == 0)) {
    return 7;
  }

  const auto null_text = laghu::core::TextView::from(nullptr, 1);
  const auto text_oob = text_view.slice(5, 1);
  const auto text_overflow = text_view.slice(std::numeric_limits<std::size_t>::max(), 1);
  if (!check(!null_text.has_value() && !text_oob.has_value() && !text_overflow.has_value() &&
             null_text.error().code() == laghu::core::ErrorCode::invalid_input &&
             text_oob.error().code() == laghu::core::ErrorCode::invalid_range &&
             text_overflow.error().code() == laghu::core::ErrorCode::invalid_range)) {
    return 8;
  }

  if (!check(allocation_attempts == allocation_attempts_before)) {
    return 9;
  }
  return 0;
}
