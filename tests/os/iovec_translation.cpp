// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>

#include <sys/uio.h>

#include "laghu_test_support.hpp"

#include <laghu/core/bounded_buffer.hpp>
#include <laghu/os/internal/io_slices.hpp>

namespace {

using laghu::core::ByteView;
using laghu::core::ErrorCode;
using laghu::core::IoSlice;
using laghu::core::IoSliceList;

[[nodiscard]] constexpr bool check(bool condition) noexcept { return condition; }

[[nodiscard]] bool check_translation_and_partial_consumption() noexcept {
  std::array<std::byte, 2> first{std::byte{1}, std::byte{2}};
  std::array<std::byte, 3> second{std::byte{3}, std::byte{4}, std::byte{5}};
  const auto first_view = ByteView::from(first);
  const auto second_view = ByteView::from(second);
  if (!check(first_view.has_value() && second_view.has_value())) {
    return false;
  }

  std::array<IoSlice, 2> slices_storage{};
  IoSliceList slices{slices_storage};
  if (!check(slices.append(ByteView{}).has_value() && slices.append(*first_view).has_value() &&
             slices.append(*second_view).has_value() && slices.size() == 2 && slices.bytes() == 5)) {
    return false;
  }

  std::array<iovec, 2> iovecs{};
  const auto translated = laghu::os::internal::translate_iovecs(slices, iovecs);
  if (!check(translated.has_value() && *translated == 2 && iovecs[0].iov_base == first.data() &&
             iovecs[0].iov_len == 2 && iovecs[1].iov_base == second.data() && iovecs[1].iov_len == 3)) {
    return false;
  }

  if (!check(laghu::os::internal::consume_written(slices, 3).has_value() && slices.size() == 1 &&
             slices.bytes() == 2)) {
    return false;
  }
  const auto partially_translated = laghu::os::internal::translate_iovecs(slices, iovecs);
  const auto expected_remaining = second_view->slice(1, 2);
  if (!check(partially_translated.has_value() && *partially_translated == 1 &&
             expected_remaining.has_value() && iovecs[0].iov_base == expected_remaining->data() &&
             iovecs[0].iov_len == 2)) {
    return false;
  }
  if (!check(laghu::os::internal::consume_written(slices, 2).has_value() && slices.empty() &&
             slices.bytes() == 0)) {
    return false;
  }
  const auto invalid_consume = laghu::os::internal::consume_written(slices, 1);
  return check(!invalid_consume.has_value() && invalid_consume.error().code() == ErrorCode::invalid_range);
}

[[nodiscard]] bool check_capacity_and_zero_slices() noexcept {
  std::array<std::byte, 1> byte{std::byte{9}};
  const auto view = ByteView::from(byte);
  if (!check(view.has_value())) {
    return false;
  }
  std::array<IoSlice, 0> empty_storage{};
  IoSliceList empty{empty_storage};
  const auto empty_append = empty.append(ByteView{});
  const auto exhausted_append = empty.append(*view);
  if (!check(empty_append.has_value() && !exhausted_append.has_value() &&
             exhausted_append.error().code() == ErrorCode::exhaustion)) {
    return false;
  }

  std::array<IoSlice, 2> list_storage{};
  IoSliceList list{list_storage};
  if (!check(list.append(*view).has_value() && list.append(*view).has_value())) {
    return false;
  }
  std::array<iovec, 1> too_small{};
  too_small[0].iov_base = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
  too_small[0].iov_len = 99;
  const auto translated = laghu::os::internal::translate_iovecs(list, too_small);
  return check(!translated.has_value() && translated.error().code() == ErrorCode::exhaustion &&
               too_small[0].iov_base == reinterpret_cast<void*>(static_cast<std::uintptr_t>(1)) &&
               too_small[0].iov_len == 99);
}

}  // namespace

[[nodiscard]] bool check_iovec_translation() noexcept {
  return check(check_translation_and_partial_consumption()) &&
         check(check_capacity_and_zero_slices());
}

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"os.iovec_translation.contract", check_iovec_translation},
  };
  return laghu::test::run_tests(tests);
}
