// SPDX-License-Identifier: AGPL-3.0-only
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <type_traits>

#include <laghu/core/checked_arithmetic.hpp>

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

template <class T>
constexpr bool has_checked_arithmetic = requires(T value) {
  laghu::core::checked_add(value, value);
  laghu::core::checked_subtract(value, value);
  laghu::core::checked_multiply(value, value);
  laghu::core::checked_narrow<T>(value);
  laghu::core::checked_range(value, value, value);
  laghu::core::is_valid_range(value, value, value);
};

template <class T>
constexpr bool supports_common_integral_operations() {
  const T one{1};
  const T two{2};
  const auto sum = laghu::core::checked_add(one, two);
  const auto difference = laghu::core::checked_subtract(two, one);
  const auto product = laghu::core::checked_multiply(two, two);
  const auto range = laghu::core::checked_range(one, one, two);
  return sum.has_value() && *sum == T{3} && difference.has_value() && *difference == one &&
         product.has_value() && *product == T{4} && range.has_value() &&
         laghu::core::is_valid_range(one, one, two);
}

template <class T>
bool check_signed_boundaries() {
  const auto maximum_add = laghu::core::checked_add(std::numeric_limits<T>::max(), T{1});
  const auto minimum_add = laghu::core::checked_add(std::numeric_limits<T>::min(), T{-1});
  const auto minimum_subtract =
      laghu::core::checked_subtract(std::numeric_limits<T>::min(), T{1});
  const auto maximum_subtract =
      laghu::core::checked_subtract(std::numeric_limits<T>::max(), T{-1});
  const auto positive_product = laghu::core::checked_multiply(std::numeric_limits<T>::max(), T{2});
  const auto min_times_negative_one =
      laghu::core::checked_multiply(std::numeric_limits<T>::min(), T{-1});
  const auto negative_one_times_minimum =
      laghu::core::checked_multiply(T{-1}, std::numeric_limits<T>::min());
  const auto minimum_times_one =
      laghu::core::checked_multiply(std::numeric_limits<T>::min(), T{1});
  return !maximum_add.has_value() && !minimum_add.has_value() && !minimum_subtract.has_value() &&
         !maximum_subtract.has_value() && !positive_product.has_value() &&
         !min_times_negative_one.has_value() && !negative_one_times_minimum.has_value() &&
         minimum_times_one.has_value() && *minimum_times_one == std::numeric_limits<T>::min() &&
         maximum_add.error().code() == laghu::core::ErrorCode::overflow &&
         min_times_negative_one.error().code() == laghu::core::ErrorCode::overflow;
}

template <class T>
bool check_unsigned_boundaries() {
  const auto add = laghu::core::checked_add(std::numeric_limits<T>::max(), T{1});
  const auto subtract = laghu::core::checked_subtract(T{0}, T{1});
  const auto multiply = laghu::core::checked_multiply(std::numeric_limits<T>::max(), T{2});
  return !add.has_value() && !subtract.has_value() && !multiply.has_value() &&
         add.error().code() == laghu::core::ErrorCode::overflow &&
         subtract.error().code() == laghu::core::ErrorCode::overflow &&
         multiply.error().code() == laghu::core::ErrorCode::overflow;
}

template <class T>
bool check_standard_boundaries() {
  if constexpr (std::numeric_limits<T>::is_signed) {
    return check_signed_boundaries<T>();
  }
  return check_unsigned_boundaries<T>();
}

bool check_int8_exhaustively() {
  for (int left = std::numeric_limits<std::int8_t>::min();
       left <= std::numeric_limits<std::int8_t>::max(); ++left) {
    for (int right = std::numeric_limits<std::int8_t>::min();
         right <= std::numeric_limits<std::int8_t>::max(); ++right) {
      const auto add = laghu::core::checked_add(static_cast<std::int8_t>(left),
                                                static_cast<std::int8_t>(right));
      const auto subtract = laghu::core::checked_subtract(static_cast<std::int8_t>(left),
                                                           static_cast<std::int8_t>(right));
      const auto multiply = laghu::core::checked_multiply(static_cast<std::int8_t>(left),
                                                           static_cast<std::int8_t>(right));
      const int add_reference = left + right;
      const int subtract_reference = left - right;
      const int multiply_reference = left * right;
      const bool add_fits = add_reference >= std::numeric_limits<std::int8_t>::min() &&
                            add_reference <= std::numeric_limits<std::int8_t>::max();
      const bool subtract_fits = subtract_reference >= std::numeric_limits<std::int8_t>::min() &&
                                 subtract_reference <= std::numeric_limits<std::int8_t>::max();
      const bool multiply_fits = multiply_reference >= std::numeric_limits<std::int8_t>::min() &&
                                 multiply_reference <= std::numeric_limits<std::int8_t>::max();
      if (add.has_value() != add_fits || subtract.has_value() != subtract_fits ||
          multiply.has_value() != multiply_fits ||
          (add_fits && *add != static_cast<std::int8_t>(add_reference)) ||
          (subtract_fits && *subtract != static_cast<std::int8_t>(subtract_reference)) ||
          (multiply_fits && *multiply != static_cast<std::int8_t>(multiply_reference))) {
        return false;
      }
    }
  }
  return true;
}

bool check_uint8_exhaustively() {
  for (unsigned int left = 0; left <= std::numeric_limits<std::uint8_t>::max(); ++left) {
    for (unsigned int right = 0; right <= std::numeric_limits<std::uint8_t>::max(); ++right) {
      const auto add = laghu::core::checked_add(static_cast<std::uint8_t>(left),
                                                static_cast<std::uint8_t>(right));
      const auto subtract = laghu::core::checked_subtract(static_cast<std::uint8_t>(left),
                                                           static_cast<std::uint8_t>(right));
      const auto multiply = laghu::core::checked_multiply(static_cast<std::uint8_t>(left),
                                                           static_cast<std::uint8_t>(right));
      const unsigned int add_reference = left + right;
      const unsigned int multiply_reference = left * right;
      const bool add_fits = add_reference <= std::numeric_limits<std::uint8_t>::max();
      const bool subtract_fits = left >= right;
      const bool multiply_fits = multiply_reference <= std::numeric_limits<std::uint8_t>::max();
      if (add.has_value() != add_fits || subtract.has_value() != subtract_fits ||
          multiply.has_value() != multiply_fits ||
          (add_fits && *add != static_cast<std::uint8_t>(add_reference)) ||
          (subtract_fits && *subtract != static_cast<std::uint8_t>(left - right)) ||
          (multiply_fits && *multiply != static_cast<std::uint8_t>(multiply_reference))) {
        return false;
      }
    }
  }
  return true;
}

std::uint64_t next_random(std::uint64_t& state) noexcept {
  state ^= state << 13U;
  state ^= state >> 7U;
  state ^= state << 17U;
  return state;
}

bool check_uint64_randomized() {
  std::uint64_t state = UINT64_C(0x14DCE9A32B7865F1);
  for (std::size_t iteration = 0; iteration < 10'000; ++iteration) {
    const std::uint64_t left = next_random(state);
    const std::uint64_t right = next_random(state);
    const auto add = laghu::core::checked_add(left, right);
    const auto subtract = laghu::core::checked_subtract(left, right);
    const auto multiply = laghu::core::checked_multiply(left, right);
    const bool add_fits = right <= std::numeric_limits<std::uint64_t>::max() - left;
    const bool subtract_fits = left >= right;
    const bool multiply_fits =
        right == 0 || left <= std::numeric_limits<std::uint64_t>::max() / right;
    if (add.has_value() != add_fits || subtract.has_value() != subtract_fits ||
        multiply.has_value() != multiply_fits || (add_fits && *add != left + right) ||
        (subtract_fits && *subtract != left - right) ||
        (multiply_fits && *multiply != left * right)) {
      return false;
    }
  }
  return true;
}

constexpr auto constexpr_sum = laghu::core::checked_add(std::int32_t{20}, std::int32_t{22});
constexpr auto constexpr_underflow = laghu::core::checked_subtract(std::uint32_t{0}, std::uint32_t{1});
constexpr auto constexpr_range = laghu::core::checked_range(std::uint64_t{4}, std::uint64_t{6}, std::uint64_t{10});
static_assert(constexpr_sum.has_value() && *constexpr_sum == 42);
static_assert(!constexpr_underflow.has_value() &&
              constexpr_underflow.error().code() == laghu::core::ErrorCode::overflow);
static_assert(constexpr_range.has_value());
static_assert(laghu::core::is_valid_range(std::uint64_t{0}, std::uint64_t{0}, std::uint64_t{0}));
static_assert(!laghu::core::is_valid_range(std::uint64_t{10}, std::uint64_t{1}, std::uint64_t{10}));
static_assert(has_checked_arithmetic<signed char>);
static_assert(has_checked_arithmetic<unsigned char>);
static_assert(has_checked_arithmetic<char>);
static_assert(has_checked_arithmetic<wchar_t>);
static_assert(has_checked_arithmetic<char8_t>);
static_assert(has_checked_arithmetic<char16_t>);
static_assert(has_checked_arithmetic<char32_t>);
static_assert(has_checked_arithmetic<short>);
static_assert(has_checked_arithmetic<unsigned short>);
static_assert(has_checked_arithmetic<int>);
static_assert(has_checked_arithmetic<unsigned int>);
static_assert(has_checked_arithmetic<long>);
static_assert(has_checked_arithmetic<unsigned long>);
static_assert(has_checked_arithmetic<long long>);
static_assert(has_checked_arithmetic<unsigned long long>);
static_assert(!has_checked_arithmetic<bool>);
static_assert(supports_common_integral_operations<signed char>());
static_assert(supports_common_integral_operations<unsigned char>());
static_assert(supports_common_integral_operations<char>());
static_assert(supports_common_integral_operations<wchar_t>());
static_assert(supports_common_integral_operations<char8_t>());
static_assert(supports_common_integral_operations<char16_t>());
static_assert(supports_common_integral_operations<char32_t>());
static_assert(supports_common_integral_operations<short>());
static_assert(supports_common_integral_operations<unsigned short>());
static_assert(supports_common_integral_operations<int>());
static_assert(supports_common_integral_operations<unsigned int>());
static_assert(supports_common_integral_operations<long>());
static_assert(supports_common_integral_operations<unsigned long>());
static_assert(supports_common_integral_operations<long long>());
static_assert(supports_common_integral_operations<unsigned long long>());
static_assert(noexcept(laghu::core::checked_add(std::int64_t{1}, std::int64_t{1})));
static_assert(noexcept(laghu::core::checked_subtract(std::int64_t{1}, std::int64_t{1})));
static_assert(noexcept(laghu::core::checked_multiply(std::int64_t{1}, std::int64_t{1})));
static_assert(noexcept(laghu::core::checked_narrow<std::int32_t>(std::int64_t{1})));
static_assert(noexcept(laghu::core::checked_range(std::uint64_t{1}, std::uint64_t{1}, std::uint64_t{2})));

}  // namespace

int main() {
  const std::size_t allocation_attempts_before = allocation_attempts;
  if (!check_standard_boundaries<signed char>() || !check_standard_boundaries<unsigned char>() ||
      !check_standard_boundaries<char>() || !check_standard_boundaries<wchar_t>() ||
      !check_standard_boundaries<char8_t>() || !check_standard_boundaries<char16_t>() ||
      !check_standard_boundaries<char32_t>() || !check_standard_boundaries<short>() ||
      !check_standard_boundaries<unsigned short>() || !check_standard_boundaries<int>() ||
      !check_standard_boundaries<unsigned int>() || !check_standard_boundaries<long>() ||
      !check_standard_boundaries<unsigned long>() || !check_standard_boundaries<long long>() ||
      !check_standard_boundaries<unsigned long long>() || !check_int8_exhaustively() ||
      !check_uint8_exhaustively() || !check_uint64_randomized()) {
    return 1;
  }

  const auto signed_to_unsigned = laghu::core::checked_narrow<std::uint32_t>(std::int64_t{-1});
  const auto wide_to_narrow =
      laghu::core::checked_narrow<std::int8_t>(std::int64_t{std::numeric_limits<std::int8_t>::max()} + 1);
  const auto unsigned_to_signed =
      laghu::core::checked_narrow<std::int32_t>(std::numeric_limits<std::uint32_t>::max());
  const auto exact_narrow = laghu::core::checked_narrow<std::int8_t>(std::int64_t{-128});
  const auto widening = laghu::core::checked_narrow<std::uint64_t>(std::uint8_t{255});
  if (signed_to_unsigned.has_value() || wide_to_narrow.has_value() || unsigned_to_signed.has_value() ||
      !exact_narrow.has_value() || *exact_narrow != -128 || !widening.has_value() ||
      *widening != 255 || signed_to_unsigned.error().code() != laghu::core::ErrorCode::overflow ||
      wide_to_narrow.error().code() != laghu::core::ErrorCode::overflow ||
      unsigned_to_signed.error().code() != laghu::core::ErrorCode::overflow) {
    return 2;
  }

  const auto valid_range = laghu::core::checked_range(std::uint64_t{4}, std::uint64_t{6}, std::uint64_t{10});
  const auto out_of_bounds = laghu::core::checked_range(std::uint64_t{8}, std::uint64_t{3}, std::uint64_t{10});
  const auto wrapping = laghu::core::checked_range(
      std::numeric_limits<std::uint64_t>::max(), std::uint64_t{1}, std::numeric_limits<std::uint64_t>::max());
  const auto negative = laghu::core::checked_range(std::int64_t{-1}, std::int64_t{0}, std::int64_t{1});
  if (!valid_range.has_value() || out_of_bounds.has_value() || wrapping.has_value() || negative.has_value() ||
      out_of_bounds.error().code() != laghu::core::ErrorCode::invalid_range ||
      wrapping.error().code() != laghu::core::ErrorCode::overflow ||
      negative.error().code() != laghu::core::ErrorCode::invalid_range ||
      laghu::core::is_valid_range(std::uint64_t{8}, std::uint64_t{3}, std::uint64_t{10})) {
    return 3;
  }

  if (allocation_attempts != allocation_attempts_before) {
    return 4;
  }
  return 0;
}
