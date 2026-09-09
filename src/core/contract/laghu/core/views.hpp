// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>

#include <laghu/core/checked_arithmetic.hpp>

namespace laghu::core {

class ByteView final {
 public:
  constexpr ByteView() noexcept = default;

  [[nodiscard]] static constexpr Result<ByteView> from(std::span<const std::byte> bytes) noexcept {
    if (bytes.data() == nullptr && !bytes.empty()) {
      return std::unexpected{invalid_view_error("byte view requires non-null data")};
    }
    return ByteView{bytes};
  }

  [[nodiscard]] constexpr std::size_t size() const noexcept { return bytes_.size(); }
  [[nodiscard]] constexpr bool empty() const noexcept { return bytes_.empty(); }
  [[nodiscard]] constexpr const std::byte* data() const noexcept { return bytes_.data(); }
  [[nodiscard]] constexpr std::span<const std::byte> span() const noexcept { return bytes_; }

  [[nodiscard]] constexpr Result<ByteView> slice(std::size_t offset,
                                                   std::size_t length) const noexcept {
    if (const auto range = checked_range(offset, length, bytes_.size()); !range.has_value()) {
      return std::unexpected{range.error()};
    }
    return ByteView{bytes_.subspan(offset, length)};
  }

 private:
  friend class MutableByteView;

  explicit constexpr ByteView(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] static constexpr Error invalid_view_error(std::string_view diagnostic) noexcept {
    return Error{ErrorDomain::core, ErrorCode::invalid_input, 0, diagnostic};
  }

  std::span<const std::byte> bytes_{};
};

class MutableByteView final {
 public:
  constexpr MutableByteView() noexcept = default;

  [[nodiscard]] static constexpr Result<MutableByteView> from(std::span<std::byte> bytes) noexcept {
    if (bytes.data() == nullptr && !bytes.empty()) {
      return std::unexpected{invalid_view_error("mutable byte view requires non-null data")};
    }
    return MutableByteView{bytes};
  }

  [[nodiscard]] constexpr std::size_t size() const noexcept { return bytes_.size(); }
  [[nodiscard]] constexpr bool empty() const noexcept { return bytes_.empty(); }
  [[nodiscard]] constexpr std::byte* data() const noexcept { return bytes_.data(); }
  [[nodiscard]] constexpr std::span<std::byte> span() const noexcept { return bytes_; }
  [[nodiscard]] constexpr ByteView as_const() const noexcept { return ByteView{bytes_}; }

  [[nodiscard]] constexpr Result<MutableByteView> slice(std::size_t offset,
                                                          std::size_t length) const noexcept {
    if (const auto range = checked_range(offset, length, bytes_.size()); !range.has_value()) {
      return std::unexpected{range.error()};
    }
    return MutableByteView{bytes_.subspan(offset, length)};
  }

 private:
  explicit constexpr MutableByteView(std::span<std::byte> bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] static constexpr Error invalid_view_error(std::string_view diagnostic) noexcept {
    return Error{ErrorDomain::core, ErrorCode::invalid_input, 0, diagnostic};
  }

  std::span<std::byte> bytes_{};
};

template <std::size_t Capacity>
class StaticCString final {
  static_assert(Capacity > 0, "StaticCString requires room for a terminator");

 public:
  constexpr StaticCString() noexcept = default;

  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] constexpr std::size_t capacity() const noexcept { return Capacity - 1; }
  [[nodiscard]] constexpr const char* c_str() const noexcept { return storage_.data(); }
  [[nodiscard]] constexpr std::string_view view() const noexcept {
    return {storage_.data(), size_};
  }

 private:
  friend class TextView;

  std::array<char, Capacity> storage_{};
  std::size_t size_{};
};

class TextView final {
 public:
  constexpr TextView() noexcept = default;

  [[nodiscard]] static constexpr Result<TextView> from(const char* data,
                                                         std::size_t size) noexcept {
    if (data == nullptr && size != 0) {
      return std::unexpected{invalid_view_error("text view requires non-null data")};
    }
    if (size == 0) {
      return TextView{};
    }
    return TextView{std::string_view{data, size}};
  }

  [[nodiscard]] static constexpr TextView from(std::string_view text) noexcept {
    return TextView{text};
  }

  [[nodiscard]] constexpr std::size_t size() const noexcept { return text_.size(); }
  [[nodiscard]] constexpr bool empty() const noexcept { return text_.empty(); }
  [[nodiscard]] constexpr const char* data() const noexcept { return text_.data(); }
  [[nodiscard]] constexpr std::string_view string_view() const noexcept { return text_; }

  [[nodiscard]] constexpr Result<TextView> slice(std::size_t offset,
                                                   std::size_t length) const noexcept {
    if (const auto range = checked_range(offset, length, text_.size()); !range.has_value()) {
      return std::unexpected{range.error()};
    }
    return TextView{text_.substr(offset, length)};
  }

  template <std::size_t Capacity>
  [[nodiscard]] constexpr Result<StaticCString<Capacity>> to_c_string() const noexcept {
    if (contains_nul()) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_input, 0,
                                   "text contains an embedded NUL"}};
    }
    if (text_.size() >= Capacity) {
      return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_range, 0,
                                   "text does not fit including terminator"}};
    }

    StaticCString<Capacity> output;
    for (std::size_t index = 0; index < text_.size(); ++index) {
      output.storage_[index] = text_[index];
    }
    output.size_ = text_.size();
    return output;
  }

 private:
  explicit constexpr TextView(std::string_view text) noexcept : text_(text) {}

  [[nodiscard]] constexpr bool contains_nul() const noexcept {
    for (char character : text_) {
      if (character == '\0') {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] static constexpr Error invalid_view_error(std::string_view diagnostic) noexcept {
    return Error{ErrorDomain::core, ErrorCode::invalid_input, 0, diagnostic};
  }

  std::string_view text_{};
};

template <class T>
struct is_borrowed_view : std::false_type {};

template <>
struct is_borrowed_view<ByteView> : std::true_type {};

template <>
struct is_borrowed_view<MutableByteView> : std::true_type {};

template <>
struct is_borrowed_view<TextView> : std::true_type {};

template <class T>
inline constexpr bool is_borrowed_view_v = is_borrowed_view<std::remove_cvref_t<T>>::value;

template <class T>
concept PersistentSchemaField = !is_borrowed_view_v<T>;

static_assert(PersistentSchemaField<StaticCString<1>>);
static_assert(!PersistentSchemaField<ByteView>);
static_assert(!PersistentSchemaField<MutableByteView>);
static_assert(!PersistentSchemaField<TextView>);

}  // namespace laghu::core
