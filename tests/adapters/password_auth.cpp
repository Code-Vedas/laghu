// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "laghu_test_support.hpp"

#include <laghu/adapters/password_auth.hpp>
#include <laghu/adapters/internal/password_auth.hpp>
#include <laghu/core/contract.hpp>
#include <laghu/core/identifiers.hpp>
#include <laghu/core/views.hpp>

namespace {

using laghu::adapters::PasswordAuthWorker;
using laghu::adapters::PasswordVerificationLimits;
using laghu::adapters::verify_password;
using laghu::core::ErrorCode;
using laghu::core::DependencyStatus;
using laghu::core::Retryability;
using laghu::core::Result;
using laghu::core::TextView;
using laghu::core::WorkerId;

constexpr std::string_view bcrypt_hash =
    "$2a$05$qbhv2IDjd3x6K2efWs.UqOcGKsAgiUNDp3XlCvRs3U2WndQqUpInu";
constexpr std::string_view sha512_hash =
    "$6$saltsalt$qFmFH.bQmmtXzyBY0s9v7Oicd2z4XSIecDzlB5KiA2/jctKu9YterLp8wwnSq.qc.eoxqOmSuNp2xS0ktL3nh/";
constexpr std::string_view long_sha512_hash =
    "$6$longsalt$iCqLcLfze8LYpgaxE2GViwKlWUNXE67wDaSlH85ygQWiQkBGt8PkZj.0CloHIKttM8iUEY423PzVQQtGPB0zA0";

[[nodiscard]] constexpr PasswordVerificationLimits limits() noexcept {
  return PasswordVerificationLimits{128, 4, 5, 1000, 5000};
}

[[nodiscard]] WorkerId worker_id() noexcept {
  return *WorkerId::from_uint64(7);
}

[[nodiscard]] Result<bool> verify(PasswordAuthWorker& worker, std::string_view password,
                                  std::string_view encoded_hash,
                                  PasswordVerificationLimits supplied_limits = limits()) noexcept {
  return verify_password(worker, worker_id(), TextView::from(password), TextView::from(encoded_hash),
                         supplied_limits);
}

[[nodiscard]] bool has_error(const Result<bool>& result, ErrorCode code) noexcept {
  return !result.has_value() && result.error().code() == code;
}

[[nodiscard]] bool check_known_answer_vectors() noexcept {
  PasswordAuthWorker worker{worker_id(), 1};
  const auto bcrypt = verify(worker, "password", bcrypt_hash);
  const auto sha512 = verify(worker, "password", sha512_hash);
  const auto wrong_password = verify(worker, "wrong-password", bcrypt_hash);
  return bcrypt.has_value() && *bcrypt && sha512.has_value() && *sha512 &&
         wrong_password.has_value() && !*wrong_password;
}

[[nodiscard]] bool check_permitted_bcrypt_prefixes() noexcept {
  PasswordAuthWorker worker{worker_id(), 1};
  std::array<char, 60> candidate{};
  for (std::size_t index = 0; index < candidate.size(); ++index) {
    candidate[index] = bcrypt_hash[index];
  }
  for (const char version : std::array{'a', 'b', 'y'}) {
    candidate[2] = version;
    const auto result = verify(worker, "password", {candidate.data(), candidate.size()});
    if (!result.has_value() || !*result) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool check_rejected_schemes_and_malformed_hashes() noexcept {
  constexpr std::array<std::string_view, 8> rejected{
      "abCDEFGHIJKLM", "$1$salt$checksum", "$apr1$salt$checksum", "$5$salt$checksum",
      "$y$j9T$salt$checksum", "$2x$05$qbhv2IDjd3x6K2efWs.UqOcGKsAgiUNDp3XlCvRs3U2WndQqUpInu",
      "$2a$03$qbhv2IDjd3x6K2efWs.UqOcGKsAgiUNDp3XlCvRs3U2WndQqUpInu", "$6$bad*salt$checksum"};
  PasswordAuthWorker worker{worker_id(), 1};
  for (const std::string_view encoded_hash : rejected) {
    if (!has_error(verify(worker, "password", encoded_hash), ErrorCode::invalid_input)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool check_caller_limits_and_boundaries() noexcept {
  PasswordAuthWorker worker{worker_id(), 1};
  auto bcrypt_limits = limits();
  bcrypt_limits.minimum_bcrypt_cost = 6;
  bcrypt_limits.maximum_bcrypt_cost = 7;
  auto sha512_limits = limits();
  sha512_limits.maximum_sha512_rounds = 4999;
  auto bad_limits = limits();
  bad_limits.maximum_password_bytes = 0;
  auto libxcrypt_exceeding_limits = limits();
  libxcrypt_exceeding_limits.maximum_password_bytes =
      PasswordVerificationLimits::maximum_supported_password_bytes + 1;
  std::array<char, 73> oversized_bcrypt_password{};
  oversized_bcrypt_password.fill('p');
  constexpr std::array<char, 3> embedded_nul{'p', '\0', 'w'};
  const auto embedded = TextView::from(embedded_nul.data(), embedded_nul.size());
  const auto wrong_worker = verify_password(
      worker, *WorkerId::from_uint64(8), TextView::from("password"), TextView::from(bcrypt_hash),
      limits());
  const auto long_sha512 = verify(
      worker,
      std::string_view{oversized_bcrypt_password.data(), oversized_bcrypt_password.size()},
      long_sha512_hash);
  return has_error(verify(worker, "password", bcrypt_hash, bcrypt_limits),
                   ErrorCode::invalid_range) &&
         has_error(verify(worker, "password", sha512_hash, sha512_limits),
                   ErrorCode::invalid_range) &&
         has_error(verify(worker, std::string_view{oversized_bcrypt_password.data(),
                                                   oversized_bcrypt_password.size()},
                          bcrypt_hash),
                   ErrorCode::invalid_range) &&
         long_sha512.has_value() && *long_sha512 &&
         has_error(verify(worker, "password", bcrypt_hash, bad_limits), ErrorCode::invalid_input) &&
         has_error(verify(worker, "password", bcrypt_hash, libxcrypt_exceeding_limits),
                   ErrorCode::invalid_input) &&
         embedded.has_value() &&
         has_error(verify_password(worker, worker_id(), *embedded, TextView::from(bcrypt_hash), limits()),
                   ErrorCode::invalid_input) &&
         has_error(wrong_worker, ErrorCode::invalid_state);
}

[[nodiscard]] bool check_native_error_normalization() noexcept {
  struct ExpectedError final {
    std::int32_t native_code;
    ErrorCode code;
    DependencyStatus status;
    Retryability retryability;
  };
  constexpr std::array expected{
      ExpectedError{EINVAL, ErrorCode::invalid_input, DependencyStatus::invalid_input,
                    Retryability::never},
      ExpectedError{ERANGE, ErrorCode::invalid_range, DependencyStatus::invalid_range,
                    Retryability::never},
      ExpectedError{ENOMEM, ErrorCode::exhaustion, DependencyStatus::exhaustion,
                    Retryability::may_retry},
      ExpectedError{ENOSYS, ErrorCode::unavailable_capability, DependencyStatus::unavailable,
                    Retryability::never},
      ExpectedError{EOPNOTSUPP, ErrorCode::unavailable_capability,
                    DependencyStatus::unavailable, Retryability::never},
  };
  for (const ExpectedError& expected_error : expected) {
    const auto error = laghu::adapters::internal::password_auth_native_error(
        expected_error.native_code, {});
    if (error.code() != expected_error.code ||
        error.dependency_status() != expected_error.status ||
        error.retryability() != expected_error.retryability ||
        error.native_code() != expected_error.native_code) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool check_constant_time_mismatch_positions() noexcept {
  PasswordAuthWorker worker{worker_id(), 1};
  constexpr std::array<std::size_t, 3> positions{7, 33, 59};
  for (const std::size_t position : positions) {
    std::array<char, 60> candidate{};
    for (std::size_t index = 0; index < candidate.size(); ++index) {
      candidate[index] = bcrypt_hash[index];
    }
    candidate[position] = candidate[position] == 'A' ? 'B' : 'A';
    const auto result = verify(worker, "password", {candidate.data(), candidate.size()});
    if (!result.has_value() || *result) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"adapters.password_auth.known_answers", check_known_answer_vectors},
      laghu::test::TestCase{"adapters.password_auth.bcrypt_prefixes", check_permitted_bcrypt_prefixes},
      laghu::test::TestCase{"adapters.password_auth.rejected_schemes", check_rejected_schemes_and_malformed_hashes},
      laghu::test::TestCase{"adapters.password_auth.caller_limits", check_caller_limits_and_boundaries},
      laghu::test::TestCase{"adapters.password_auth.native_error_normalization",
                            check_native_error_normalization},
      laghu::test::TestCase{"adapters.password_auth.timing_positions", check_constant_time_mismatch_positions},
  };
  return laghu::test::run_tests(tests);
}
