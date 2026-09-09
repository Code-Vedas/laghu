// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <laghu/core/views.hpp>

namespace laghu::core {

template <class T>
concept BigEndianFixedWidth = std::same_as<std::remove_cv_t<T>, std::uint16_t> ||
                              std::same_as<std::remove_cv_t<T>, std::uint32_t> ||
                              std::same_as<std::remove_cv_t<T>, std::uint64_t>;

template <BigEndianFixedWidth T>
[[nodiscard]] constexpr std::array<std::byte, sizeof(T)> encode_big_endian(T value) noexcept {
  std::array<std::byte, sizeof(T)> encoded{};
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const std::size_t shift = (encoded.size() - 1 - index) * 8;
    encoded[index] = static_cast<std::byte>(value >> shift);
  }
  return encoded;
}

template <BigEndianFixedWidth T>
[[nodiscard]] constexpr Result<T> decode_big_endian(ByteView encoded) noexcept {
  if (encoded.size() != sizeof(T)) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_range, 0,
                                 "big-endian value has an invalid encoded size"}};
  }

  T value = 0;
  for (const std::byte byte : encoded.span()) {
    value = static_cast<T>((value << 8) | std::to_integer<std::uint8_t>(byte));
  }
  return value;
}

template <class Tag>
class SharedOffset final {
 public:
  constexpr SharedOffset() noexcept = default;
  SharedOffset(const SharedOffset&) noexcept = default;
  SharedOffset& operator=(const SharedOffset&) noexcept = default;
  SharedOffset(SharedOffset&&) noexcept = default;
  SharedOffset& operator=(SharedOffset&&) noexcept = default;
  ~SharedOffset() = default;

  [[nodiscard]] static constexpr SharedOffset from_raw(std::uint64_t value) noexcept {
    return SharedOffset{value};
  }

  [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_null() const noexcept { return value_ == 0; }

  template <class T>
  [[nodiscard]] constexpr Result<const T*> resolve(ByteView mapping) const noexcept
    requires (std::is_object_v<T> && std::is_standard_layout_v<T> &&
              std::is_trivially_copyable_v<T> && !std::is_polymorphic_v<T>)
  {
    if (is_null()) {
      return static_cast<const T*>(nullptr);
    }
    const auto offset = checked_narrow<std::size_t>(value_);
    if (!offset.has_value()) {
      return std::unexpected{offset.error()};
    }
    if (const auto range = checked_range(*offset, sizeof(T), mapping.size()); !range.has_value()) {
      return std::unexpected{range.error()};
    }

    const std::byte* const resolved = mapping.span().subspan(*offset, sizeof(T)).data();
    if (reinterpret_cast<std::uintptr_t>(resolved) % alignof(T) != 0) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_range, 0,
                                   "shared offset does not meet mapped object alignment"}};
    }
    return static_cast<const T*>(static_cast<const void*>(resolved));
  }

  template <class T>
  [[nodiscard]] constexpr Result<T*> resolve(MutableByteView mapping) const noexcept
    requires (std::is_object_v<T> && std::is_standard_layout_v<T> &&
              std::is_trivially_copyable_v<T> && !std::is_polymorphic_v<T>)
  {
    if (is_null()) {
      return static_cast<T*>(nullptr);
    }
    const auto offset = checked_narrow<std::size_t>(value_);
    if (!offset.has_value()) {
      return std::unexpected{offset.error()};
    }
    if (const auto range = checked_range(*offset, sizeof(T), mapping.size()); !range.has_value()) {
      return std::unexpected{range.error()};
    }

    std::byte* const resolved = mapping.span().subspan(*offset, sizeof(T)).data();
    if (reinterpret_cast<std::uintptr_t>(resolved) % alignof(T) != 0) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_range, 0,
                                   "shared offset does not meet mapped object alignment"}};
    }
    return static_cast<T*>(static_cast<void*>(resolved));
  }

 private:
  explicit constexpr SharedOffset(std::uint64_t value) noexcept : value_(value) {}

  std::uint64_t value_{};
};

template <class T>
struct is_shared_schema_field : std::bool_constant<
                                    BigEndianFixedWidth<T> ||
                                    (std::is_same_v<std::remove_cv_t<T>, std::uint8_t>)> {};

template <class Tag>
struct is_shared_schema_field<SharedOffset<Tag>> : std::true_type {};

template <class T>
concept SharedSchemaField = is_shared_schema_field<std::remove_cvref_t<T>>::value &&
                            !std::is_pointer_v<std::remove_cvref_t<T>> &&
                            !std::is_reference_v<T> &&
                            !is_borrowed_view_v<T> &&
                            !std::is_polymorphic_v<std::remove_cvref_t<T>>;

// This type-level declaration is the explicit admission gate for fields in a
// persistent or cross-process schema. Native C++ layout is never serialized.
template <SharedSchemaField... Fields>
struct SharedSchema final {
  static constexpr std::size_t field_count = sizeof...(Fields);
};

static_assert(sizeof(SharedOffset<struct SharedOffsetLayoutCheckTag>) == sizeof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<SharedOffset<SharedOffsetLayoutCheckTag>>);
static_assert(std::is_standard_layout_v<SharedOffset<SharedOffsetLayoutCheckTag>>);
static_assert(SharedSchemaField<std::uint8_t>);
static_assert(SharedSchemaField<std::uint16_t>);
static_assert(SharedSchemaField<std::uint32_t>);
static_assert(SharedSchemaField<std::uint64_t>);
static_assert(!SharedSchemaField<ByteView>);
static_assert(!SharedSchemaField<MutableByteView>);
static_assert(!SharedSchemaField<TextView>);

}  // namespace laghu::core
