// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <concepts>
#include <limits>
#include <type_traits>
#include <utility>

#include <laghu/core/contract.hpp>

namespace laghu::core {

template <class T>
concept CheckedIntegral = std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool>;

namespace detail {

template <CheckedIntegral T>
[[nodiscard]] constexpr Error arithmetic_overflow_error() noexcept {
  return Error{ErrorDomain::core, ErrorCode::overflow, 0, "checked arithmetic overflow"};
}

template <CheckedIntegral T>
[[nodiscard]] constexpr Error invalid_range_error() noexcept {
  return Error{ErrorDomain::core, ErrorCode::invalid_range, 0, "checked range is invalid"};
}

}  // namespace detail

template <CheckedIntegral T>
[[nodiscard]] constexpr Result<T> checked_add(T left, T right) noexcept {
  if constexpr (std::numeric_limits<T>::is_signed) {
    if ((right > 0 && left > std::numeric_limits<T>::max() - right) ||
        (right < 0 && left < std::numeric_limits<T>::min() - right)) {
      return std::unexpected{detail::arithmetic_overflow_error<T>()};
    }
  } else if (left > std::numeric_limits<T>::max() - right) {
    return std::unexpected{detail::arithmetic_overflow_error<T>()};
  }
  return static_cast<T>(left + right);
}

template <CheckedIntegral T>
[[nodiscard]] constexpr Result<T> checked_subtract(T left, T right) noexcept {
  if constexpr (std::numeric_limits<T>::is_signed) {
    if ((right > 0 && left < std::numeric_limits<T>::min() + right) ||
        (right < 0 && left > std::numeric_limits<T>::max() + right)) {
      return std::unexpected{detail::arithmetic_overflow_error<T>()};
    }
  } else if (left < right) {
    return std::unexpected{detail::arithmetic_overflow_error<T>()};
  }
  return static_cast<T>(left - right);
}

template <CheckedIntegral T>
[[nodiscard]] constexpr Result<T> checked_multiply(T left, T right) noexcept {
  if (left == 0 || right == 0) {
    return T{0};
  }

  if constexpr (std::numeric_limits<T>::is_signed) {
    if ((left == T{-1} && right == std::numeric_limits<T>::min()) ||
        (right == T{-1} && left == std::numeric_limits<T>::min())) {
      return std::unexpected{detail::arithmetic_overflow_error<T>()};
    }
    if ((left > 0 && right > 0 && left > std::numeric_limits<T>::max() / right) ||
        (left > 0 && right < 0 && right < std::numeric_limits<T>::min() / left) ||
        (left < 0 && right > 0 && left < std::numeric_limits<T>::min() / right) ||
        (left < 0 && right < 0 && left < std::numeric_limits<T>::max() / right)) {
      return std::unexpected{detail::arithmetic_overflow_error<T>()};
    }
  } else if (left > std::numeric_limits<T>::max() / right) {
    return std::unexpected{detail::arithmetic_overflow_error<T>()};
  }
  return static_cast<T>(left * right);
}

template <CheckedIntegral To, CheckedIntegral From>
[[nodiscard]] constexpr Result<To> checked_narrow(From value) noexcept {
  if (!std::in_range<To>(value)) {
    return std::unexpected{detail::arithmetic_overflow_error<To>()};
  }
  return static_cast<To>(value);
}

template <CheckedIntegral T>
[[nodiscard]] constexpr Result<void> checked_range(T offset, T length, T limit) noexcept {
  if constexpr (std::numeric_limits<T>::is_signed) {
    if (offset < 0 || length < 0 || limit < 0) {
      return std::unexpected{detail::invalid_range_error<T>()};
    }
  }
  if (offset > limit) {
    return std::unexpected{detail::invalid_range_error<T>()};
  }
  if (length > std::numeric_limits<T>::max() - offset) {
    return std::unexpected{detail::arithmetic_overflow_error<T>()};
  }
  if (length > limit - offset) {
    return std::unexpected{detail::invalid_range_error<T>()};
  }
  return {};
}

template <CheckedIntegral T>
[[nodiscard]] constexpr bool is_valid_range(T offset, T length, T limit) noexcept {
  return checked_range(offset, length, limit).has_value();
}

}  // namespace laghu::core
