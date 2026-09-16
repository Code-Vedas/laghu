// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <string_view>

#include "laghu_test_support.hpp"

#include <laghu/adapters/idna.hpp>
#include <laghu/core/contract.hpp>
#include <laghu/core/views.hpp>

namespace {

using laghu::adapters::AsciiHostname;
using laghu::adapters::idna_to_ascii;
using laghu::core::DependencyId;
using laghu::core::DependencyOperation;
using laghu::core::DependencyStatus;
using laghu::core::ErrorCode;
using laghu::core::TextView;

[[nodiscard]] bool has_error(const laghu::core::Result<AsciiHostname>& result,
                             ErrorCode code) noexcept {
  return !result.has_value() && result.error().code() == code;
}

[[nodiscard]] bool has_idn2_error(const laghu::core::Result<AsciiHostname>& result) noexcept {
  return !result.has_value() && result.error().code() == ErrorCode::corrupt_data &&
         result.error().dependency_id() == DependencyId::libidn2 &&
         result.error().dependency_operation() == DependencyOperation::idna_lookup &&
         result.error().dependency_status() == DependencyStatus::corrupt_data;
}

[[nodiscard]] bool check_unicode_golden_vectors() noexcept {
  const auto german = idna_to_ascii(TextView::from("b\xC3\xBC""cher.example"));
  const auto japanese = idna_to_ascii(
      TextView::from("\xE4\xBE\x8B\xE3\x81\x88.\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88"));
  return german.has_value() && german->value() == "xn--bcher-kva.example" &&
         japanese.has_value() && japanese->value() == "xn--r8jz45g.xn--zckzah";
}

[[nodiscard]] bool check_input_and_hostname_limits() noexcept {
  constexpr std::array<char, 4> embedded_nul{'a', '\0', 'b', 'c'};
  constexpr std::array<char, 2> invalid_utf8{static_cast<char>(0xC0),
                                              static_cast<char>(0xAF)};
  std::array<char, 64> long_label{};
  long_label.fill('a');
  std::array<char, 254> long_hostname{};
  long_hostname.fill('a');

  const auto embedded = TextView::from(embedded_nul.data(), embedded_nul.size());
  const auto invalid = TextView::from(invalid_utf8.data(), invalid_utf8.size());
  const auto label = TextView::from(long_label.data(), long_label.size());
  const auto hostname = TextView::from(long_hostname.data(), long_hostname.size());
  const auto invalid_result = idna_to_ascii(*invalid);
  return embedded.has_value() && invalid.has_value() && label.has_value() &&
         hostname.has_value() && has_error(idna_to_ascii(*embedded), ErrorCode::invalid_input) &&
         has_idn2_error(invalid_result) &&
         !idna_to_ascii(*label).has_value() &&
         has_error(idna_to_ascii(*hostname), ErrorCode::invalid_range) &&
         has_error(idna_to_ascii(TextView::from("bad_label.example")),
                   ErrorCode::invalid_input);
}

[[nodiscard]] bool check_malformed_fuzz_vectors() noexcept {
  constexpr std::array<std::string_view, 6> vectors{
      "-leading.example", "trailing-.example", "two..labels.example", " space.example",
      "example.", "\xF0\x28\x8C\xBC.example"};
  for (const std::string_view vector : vectors) {
    if (idna_to_ascii(TextView::from(vector)).has_value()) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool check_bounded_ascii_value() noexcept {
  const auto accepted = AsciiHostname::from_ascii("www.example-123.test");
  const auto empty = AsciiHostname::from_ascii("");
  const auto non_ascii = AsciiHostname::from_ascii("b\xC3\xBC""cher.example");
  return accepted.has_value() && accepted->value() == "www.example-123.test" &&
         has_error(empty, ErrorCode::invalid_input) &&
         has_error(non_ascii, ErrorCode::invalid_input);
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"adapters.idna.unicode_golden", check_unicode_golden_vectors},
      laghu::test::TestCase{"adapters.idna.input_and_limits", check_input_and_hostname_limits},
      laghu::test::TestCase{"adapters.idna.malformed_fuzz_vectors", check_malformed_fuzz_vectors},
      laghu::test::TestCase{"adapters.idna.bounded_ascii", check_bounded_ascii_value},
  };
  return laghu::test::run_tests(tests);
}
