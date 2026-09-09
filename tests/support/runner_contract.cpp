// SPDX-License-Identifier: AGPL-3.0-only
#include "laghu_test_support.hpp"

#include <array>

namespace {

[[nodiscard]] bool passing_test() noexcept { return true; }
[[nodiscard]] bool failing_test() noexcept { return false; }

[[nodiscard]] bool check_runner_exit_codes() noexcept {
  constexpr std::array passing{
      laghu::test::TestCase{"runner.passing", passing_test},
  };
  constexpr std::array failing{
      laghu::test::TestCase{"runner.failing", failing_test},
  };
  constexpr std::array invalid{
      laghu::test::TestCase{"invalid name", passing_test},
  };
  return laghu::test::run_tests(passing) == static_cast<int>(laghu::test::TestExitCode::success) &&
         laghu::test::run_tests(failing) == static_cast<int>(laghu::test::TestExitCode::test_failed) &&
         laghu::test::run_tests(invalid) == static_cast<int>(laghu::test::TestExitCode::invalid_suite);
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"runner.stable_exit_codes", check_runner_exit_codes},
  };
  return laghu::test::run_tests(tests);
}
