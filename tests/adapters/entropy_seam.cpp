// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cerrno>
#include <cstddef>
#include <span>

#include "laghu_test_support.hpp"
#include "laghu_test_time_entropy.hpp"

#include <laghu/adapters/internal/entropy.hpp>

namespace {

template <std::size_t Size>
[[nodiscard]] laghu::core::MutableByteView mutable_view(
    std::array<std::byte, Size>& bytes) noexcept {
  return *laghu::core::MutableByteView::from(std::span<std::byte>{bytes});
}

[[nodiscard]] bool check_deterministic_entropy_seam() noexcept {
  std::array<std::byte, 600> sequence{};
  for (std::size_t index = 0; index < sequence.size(); ++index) {
    sequence[index] = static_cast<std::byte>(index);
  }
  std::array<std::byte, 600> output{};
  laghu::test::DeterministicEntropy entropy{std::span<const std::byte>{sequence}};
  const auto success = laghu::adapters::internal::fill_entropy_with(
      mutable_view(output), entropy.call(), entropy.context());
  if (!success.has_value() || entropy.calls() != 3 || entropy.consumed() != output.size() ||
      output[0] != sequence[0] || output[255] != sequence[255] ||
      output[256] != sequence[256] || output.back() != sequence.back()) {
    return false;
  }

  laghu::test::DeterministicEntropy failing{std::span<const std::byte>{sequence}};
  if (!failing.fail_on_call(2, EIO)) {
    return false;
  }
  const auto failure = laghu::adapters::internal::fill_entropy_with(
      mutable_view(output), failing.call(), failing.context());
  return !failure.has_value() && failing.calls() == 2 && failing.consumed() == 256 &&
         failure.error().domain() == laghu::core::ErrorDomain::posix &&
         failure.error().code() == laghu::core::ErrorCode::io && failure.error().native_code() == EIO;
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"adapters.entropy.deterministic_seam", check_deterministic_entropy_seam},
  };
  return laghu::test::run_tests(tests);
}
