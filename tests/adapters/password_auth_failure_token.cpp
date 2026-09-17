// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cerrno>
#include <string_view>

#include "laghu_test_support.hpp"

#include <laghu/adapters/password_auth.hpp>
#include <laghu/core/contract.hpp>
#include <laghu/core/identifiers.hpp>
#include <laghu/core/views.hpp>

struct crypt_data;

extern "C" char* crypt_r(const char*, const char*, crypt_data*) {
  static char failure_token[]{'*', '0', '\0'};
  errno = EIO;
  return failure_token;
}

namespace {

using laghu::adapters::PasswordAuthWorker;
using laghu::adapters::PasswordVerificationLimits;
using laghu::adapters::verify_password;
using laghu::core::DependencyId;
using laghu::core::DependencyOperation;
using laghu::core::DependencyStatus;
using laghu::core::ErrorCode;
using laghu::core::TextView;
using laghu::core::WorkerId;

constexpr std::string_view bcrypt_hash =
    "$2a$05$qbhv2IDjd3x6K2efWs.UqOcGKsAgiUNDp3XlCvRs3U2WndQqUpInu";

[[nodiscard]] constexpr PasswordVerificationLimits limits() noexcept {
  return PasswordVerificationLimits{128, 4, 5, 1000, 5000};
}

[[nodiscard]] bool check_failure_token_normalization() noexcept {
  PasswordAuthWorker worker{*WorkerId::from_uint64(7), 1};
  const auto result = verify_password(worker, *WorkerId::from_uint64(7),
                                      TextView::from("password"), TextView::from(bcrypt_hash),
                                      limits());
  return !result.has_value() && result.error().code() == ErrorCode::io &&
         result.error().dependency_id() == DependencyId::libxcrypt &&
         result.error().dependency_operation() == DependencyOperation::password_verify &&
         result.error().dependency_status() == DependencyStatus::io &&
         result.error().native_code() == EIO;
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"adapters.password_auth.failure_token",
                            check_failure_token_normalization},
  };
  return laghu::test::run_tests(tests);
}
