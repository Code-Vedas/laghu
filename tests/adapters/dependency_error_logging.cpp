// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

#include "laghu_test_support.hpp"

#include <laghu/adapters/dependency.hpp>
#include <laghu/adapters/internal/dependency.hpp>
#include <laghu/core/contract.hpp>

namespace {

struct LogCapture final {
  std::array<laghu::adapters::DependencyLogRecord, 2> records{};
  std::size_t count{};
};

void capture_log(void* context, const laghu::adapters::DependencyLogRecord& record) noexcept {
  if (context == nullptr) {
    return;
  }
  auto& capture = *static_cast<LogCapture*>(context);
  if (capture.count < capture.records.size()) {
    capture.records[capture.count] = record;
  }
  ++capture.count;
}

void fixture_library_failure(void (*callback)(void*, std::int32_t) noexcept,
                             void* context, std::int32_t native_code) noexcept {
  callback(context, native_code);
}

[[nodiscard]] bool check_normalization_and_log_record() noexcept {
  constexpr std::string_view raw_request_data = "authorization=very-secret-request-data";
  const laghu::core::Error error = laghu::adapters::normalize_dependency_error(
      laghu::core::DependencyId::openssl, laghu::core::DependencyOperation::spki_decode,
      laghu::core::DependencyStatus::crypto, -91);
  if (error.domain() != laghu::core::ErrorDomain::dependency ||
      error.code() != laghu::core::ErrorCode::crypto ||
      error.dependency_id() != laghu::core::DependencyId::openssl ||
      error.dependency_operation() != laghu::core::DependencyOperation::spki_decode ||
      error.dependency_status() != laghu::core::DependencyStatus::crypto ||
      error.native_code() != -91 ||
      error.diagnostic_context() !=
          "dependency=openssl operation=spki_decode status=crypto" ||
      error.diagnostic_context().find(raw_request_data) != std::string_view::npos ||
      error.diagnostic_context().size() > laghu::core::Error::diagnostic_context_capacity ||
      error.diagnostic_truncated()) {
    return false;
  }

  LogCapture capture{};
  const laghu::adapters::DependencyLogSink sink{&capture, capture_log};
  laghu::adapters::log_dependency_error(sink, error);
  if (capture.count != 1) {
    return false;
  }
  const auto& record = capture.records.front();
  return record.level == laghu::adapters::DependencyLogLevel::error &&
         record.dependency == laghu::core::DependencyId::openssl &&
         record.operation == laghu::core::DependencyOperation::spki_decode &&
         record.status == laghu::core::DependencyStatus::crypto &&
         record.native_code == -91 && record.component() == "openssl" &&
         record.message() == "operation=spki_decode status=crypto" &&
         record.component().find(raw_request_data) == std::string_view::npos &&
         record.message().find(raw_request_data) == std::string_view::npos &&
         record.component().size() <= laghu::adapters::DependencyLogRecord::component_capacity &&
         record.message().size() <= laghu::adapters::DependencyLogRecord::message_capacity;
}

[[nodiscard]] bool check_invalid_values_and_disabled_sink() noexcept {
  const laghu::core::Error error = laghu::adapters::normalize_dependency_error(
      static_cast<laghu::core::DependencyId>(255U),
      static_cast<laghu::core::DependencyOperation>(255U),
      static_cast<laghu::core::DependencyStatus>(255U),
      std::numeric_limits<std::int32_t>::min());
  if (error.code() != laghu::core::ErrorCode::dependency ||
      error.dependency_id() != laghu::core::DependencyId::none ||
      error.dependency_operation() != laghu::core::DependencyOperation::none ||
      error.dependency_status() != laghu::core::DependencyStatus::unknown ||
      error.native_code() != std::numeric_limits<std::int32_t>::min() ||
      error.diagnostic_context() != "dependency=none operation=none status=unknown") {
    return false;
  }

  laghu::adapters::log_dependency_error({}, error);
  const laghu::core::Error non_dependency{laghu::core::ErrorDomain::core,
                                          laghu::core::ErrorCode::invalid_state};
  LogCapture capture{};
  laghu::adapters::log_dependency_error({&capture, capture_log}, non_dependency);
  return capture.count == 0;
}

[[nodiscard]] bool check_retry_and_security_classifications() noexcept {
  const laghu::core::Error error = laghu::adapters::normalize_dependency_error(
      laghu::core::DependencyId::openssl, laghu::core::DependencyOperation::sha256,
      laghu::core::DependencyStatus::io, -31);
  if (error.retryability() != laghu::core::Retryability::may_retry ||
      error.security_relevance() != laghu::core::SecurityRelevance::ordinary) {
    return false;
  }
  LogCapture capture{};
  laghu::adapters::log_dependency_error({&capture, capture_log}, error);
  return capture.count == 1 &&
         capture.records.front().level == laghu::adapters::DependencyLogLevel::warning &&
         capture.records.front().message() == "operation=sha256 status=io";
}

[[nodiscard]] bool check_truncated_diagnostic_is_not_logged() noexcept {
  constexpr std::string_view raw_request_marker = "authorization=very-secret-request";
  std::array<char, laghu::core::Error::diagnostic_context_capacity + 8> diagnostic{};
  for (std::size_t index = 0; index < raw_request_marker.size(); ++index) {
    diagnostic[index] = raw_request_marker[index];
  }
  for (std::size_t index = raw_request_marker.size(); index < diagnostic.size(); ++index) {
    diagnostic[index] = 'x';
  }
  const laghu::core::Error error = laghu::core::Error::from_dependency(
      laghu::core::DependencyId::openssl, laghu::core::DependencyOperation::sha256,
      laghu::core::DependencyStatus::crypto, -66,
      {diagnostic.data(), diagnostic.size()});
  if (!error.diagnostic_truncated() ||
      error.diagnostic_context().size() != laghu::core::Error::diagnostic_context_capacity ||
      error.diagnostic_context().find(raw_request_marker) == std::string_view::npos) {
    return false;
  }

  LogCapture capture{};
  laghu::adapters::log_dependency_error({&capture, capture_log}, error);
  return capture.count == 1 &&
         capture.records.front().message() == "operation=sha256 status=crypto" &&
         capture.records.front().message().find(raw_request_marker) == std::string_view::npos &&
         capture.records.front().message().size() <=
             laghu::adapters::DependencyLogRecord::message_capacity;
}

[[nodiscard]] bool check_fixture_callback_containment() noexcept {
  LogCapture capture{};
  laghu::adapters::internal::DependencyCallbackState state{};
  state.log_sink = {&capture, capture_log};
  state.dependency = laghu::core::DependencyId::libressl;
  state.operation = laghu::core::DependencyOperation::hmac_sha256;
  state.status = laghu::core::DependencyStatus::crypto;

  fixture_library_failure(laghu::adapters::internal::dependency_failure_callback, &state, -203);
  laghu::adapters::internal::dependency_failure_callback(nullptr, -204);

  return state.failed && state.error.domain() == laghu::core::ErrorDomain::dependency &&
         state.error.code() == laghu::core::ErrorCode::crypto &&
         state.error.dependency_id() == laghu::core::DependencyId::libressl &&
         state.error.dependency_operation() == laghu::core::DependencyOperation::hmac_sha256 &&
         state.error.native_code() == -203 && capture.count == 1 &&
         capture.records.front().component() == "libressl" &&
         capture.records.front().message() == "operation=hmac_sha256 status=crypto";
}

static_assert(std::is_nothrow_invocable_v<laghu::adapters::DependencyLogWrite, void*,
                                           const laghu::adapters::DependencyLogRecord&>);
static_assert(std::is_nothrow_invocable_v<decltype(laghu::adapters::internal::dependency_failure_callback),
                                           void*, std::int32_t>);

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"adapters.dependency_error_logging.normalization",
                            check_normalization_and_log_record},
      laghu::test::TestCase{"adapters.dependency_error_logging.invalid_values",
                            check_invalid_values_and_disabled_sink},
      laghu::test::TestCase{"adapters.dependency_error_logging.classifications",
                            check_retry_and_security_classifications},
      laghu::test::TestCase{"adapters.dependency_error_logging.truncation",
                            check_truncated_diagnostic_is_not_logged},
      laghu::test::TestCase{"adapters.dependency_error_logging.callback",
                            check_fixture_callback_containment},
  };
  return laghu::test::run_tests(tests);
}
