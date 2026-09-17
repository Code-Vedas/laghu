// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <idn2.h>

#include <laghu/adapters/idna.hpp>
#include <laghu/adapters/internal/idna.hpp>

namespace laghu::adapters {
namespace {

// Libidn2 bounds a lookup name to 255 octets.  Its UTF-8 input is separate
// from Laghu's 253-byte ASCII output contract and requires up to four octets
// per Unicode scalar value.
constexpr std::size_t libidn2_lookup_name_maximum_length = 255;
constexpr std::size_t maximum_utf8_octets_per_scalar = 4;
constexpr std::size_t maximum_utf8_input_length =
    libidn2_lookup_name_maximum_length * maximum_utf8_octets_per_scalar;
constexpr std::size_t utf8_input_storage_capacity = maximum_utf8_input_length + 1;
constexpr std::size_t native_output_storage_capacity =
    libidn2_lookup_name_maximum_length + 1;

[[nodiscard]] constexpr bool is_ascii_label_character(char character) noexcept {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '-';
}

[[nodiscard]] constexpr bool is_permitted_ascii_input_character(char character) noexcept {
  return is_ascii_label_character(character) || character == '.';
}

[[nodiscard]] core::Result<void> validate_input_label_structure(
    std::string_view hostname) noexcept {
  bool preceding_separator = true;
  for (const char character : hostname) {
    if (character == '.') {
      if (preceding_separator) {
        return std::unexpected{core::Error{core::ErrorDomain::core,
                                           core::ErrorCode::invalid_input, 0,
                                           "hostname contains an empty label"}};
      }
      preceding_separator = true;
    } else {
      preceding_separator = false;
    }
  }
  if (preceding_separator) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::invalid_input, 0,
                                       "hostname contains an empty label"}};
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_ascii_hostname(std::string_view hostname) noexcept {
  if (hostname.empty()) {
    return std::unexpected{core::Error{core::ErrorDomain::core, core::ErrorCode::invalid_input, 0,
                                       "hostname must not be empty"}};
  }
  if (hostname.size() > AsciiHostname::maximum_text_length) {
    return std::unexpected{core::Error{core::ErrorDomain::core, core::ErrorCode::invalid_range, 0,
                                       "hostname exceeds 253 bytes"}};
  }

  std::size_t label_start{};
  for (std::size_t index = 0; index <= hostname.size(); ++index) {
    if (index == hostname.size() || hostname[index] == '.') {
      const std::size_t label_size = index - label_start;
      if (label_size == 0 || label_size > 63 || hostname[label_start] == '-' ||
          hostname[index - 1] == '-') {
        return std::unexpected{core::Error{core::ErrorDomain::core,
                                           core::ErrorCode::invalid_input, 0,
                                           "hostname contains an invalid label"}};
      }
      label_start = index + 1;
      continue;
    }
    if (!is_ascii_label_character(hostname[index])) {
      return std::unexpected{core::Error{core::ErrorDomain::core, core::ErrorCode::invalid_input,
                                         0, "hostname is not bounded ASCII"}};
    }
  }
  return {};
}

}  // namespace

core::DependencyStatus internal::idn2_status(int status) noexcept {
  if (status == IDN2_MALLOC) {
    return core::DependencyStatus::exhaustion;
  }
  if (status == IDN2_NO_CODESET) {
    return core::DependencyStatus::unavailable;
  }
  if (status == IDN2_TOO_BIG_DOMAIN || status == IDN2_TOO_BIG_LABEL ||
      status == IDN2_PUNYCODE_BIG_OUTPUT) {
    return core::DependencyStatus::invalid_range;
  }
  if (status == IDN2_ENCODING_ERROR || status == IDN2_PUNYCODE_BAD_INPUT ||
      status == IDN2_PUNYCODE_OVERFLOW ||
      status == IDN2_INVALID_ALABEL || status == IDN2_UALABEL_MISMATCH ||
      status == IDN2_NOT_NFC || status == IDN2_2HYPHEN ||
      status == IDN2_HYPHEN_STARTEND || status == IDN2_LEADING_COMBINING ||
      status == IDN2_DISALLOWED || status == IDN2_CONTEXTJ ||
      status == IDN2_CONTEXTJ_NO_RULE || status == IDN2_CONTEXTO ||
      status == IDN2_CONTEXTO_NO_RULE || status == IDN2_UNASSIGNED ||
      status == IDN2_BIDI || status == IDN2_DOT_IN_LABEL ||
      status == IDN2_INVALID_TRANSITIONAL || status == IDN2_INVALID_NONTRANSITIONAL ||
      status == IDN2_ALABEL_ROUNDTRIP_FAILED) {
    return core::DependencyStatus::invalid_input;
  }
  return core::DependencyStatus::corrupt_data;
}

namespace {

[[nodiscard]] core::Error idn2_failure(int status, const DependencyLogSink& log_sink) noexcept {
  const core::Error error = normalize_dependency_error(
      core::DependencyId::libidn2, core::DependencyOperation::idna_lookup,
      internal::idn2_status(status), static_cast<std::int32_t>(status));
  log_dependency_error(log_sink, error);
  return error;
}

class NativeIdn2Output final {
 public:
  explicit constexpr NativeIdn2Output(std::uint8_t* output) noexcept : output_(output) {}

  NativeIdn2Output(const NativeIdn2Output&) = delete;
  NativeIdn2Output& operator=(const NativeIdn2Output&) = delete;

  ~NativeIdn2Output() {
    if (output_ != nullptr) {
      idn2_free(output_);
    }
  }

  [[nodiscard]] constexpr const char* characters() const noexcept {
    return reinterpret_cast<const char*>(output_);
  }

 private:
  std::uint8_t* output_{};
};

[[nodiscard]] core::Result<std::string_view> bounded_native_string(const char* value) noexcept {
  if (value == nullptr) {
    return std::unexpected{core::Error{core::ErrorDomain::dependency,
                                       core::ErrorCode::corrupt_data, 0,
                                       "libidn2 returned no hostname"}};
  }
  for (std::size_t index = 0; index < native_output_storage_capacity; ++index) {
    if (value[index] == '\0') {
      return std::string_view{value, index};
    }
  }
  return std::unexpected{core::Error{core::ErrorDomain::dependency,
                                     core::ErrorCode::corrupt_data, 0,
                                     "libidn2 output exceeded hostname capacity"}};
}

}  // namespace

core::Result<AsciiHostname> AsciiHostname::from_ascii(std::string_view hostname) noexcept {
  if (const auto valid = validate_ascii_hostname(hostname); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  std::array<char, storage_capacity> bytes{};
  for (std::size_t index = 0; index < hostname.size(); ++index) {
    bytes[index] = hostname[index];
  }
  return AsciiHostname{bytes, static_cast<std::uint16_t>(hostname.size())};
}

core::Result<AsciiHostname> idna_to_ascii(core::TextView hostname,
                                          DependencyLogSink log_sink) noexcept {
  if (hostname.empty()) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::invalid_input, 0,
                                       "hostname must not be empty"}};
  }
  const auto input = hostname.to_c_string<utf8_input_storage_capacity>();
  if (!input.has_value()) {
    return std::unexpected{input.error()};
  }
  for (const char character : input->view()) {
    if (static_cast<unsigned char>(character) < 0x80U &&
        !is_permitted_ascii_input_character(character)) {
      return std::unexpected{core::Error{core::ErrorDomain::core,
                                         core::ErrorCode::invalid_input, 0,
                                         "hostname contains an invalid ASCII character"}};
    }
  }
  if (const auto valid = validate_input_label_structure(input->view()); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }

  std::uint8_t* native_output{};
  constexpr int idna_flags = IDN2_NFC_INPUT | IDN2_NONTRANSITIONAL | IDN2_USE_STD3_ASCII_RULES;
  const int status = idn2_lookup_u8(
      reinterpret_cast<const std::uint8_t*>(input->c_str()), &native_output, idna_flags);
  if (status != IDN2_OK) {
    return std::unexpected{idn2_failure(status, log_sink)};
  }
  NativeIdn2Output output{native_output};
  const auto native_hostname = bounded_native_string(output.characters());
  if (!native_hostname.has_value()) {
    const core::Error error = normalize_dependency_error(
        core::DependencyId::libidn2, core::DependencyOperation::idna_lookup,
        core::DependencyStatus::corrupt_data, 0);
    log_dependency_error(log_sink, error);
    return std::unexpected{error};
  }
  if (native_hostname->size() > AsciiHostname::maximum_text_length) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::invalid_range, 0,
                                       "IDNA output exceeds 253 bytes"}};
  }
  const auto bounded_hostname = AsciiHostname::from_ascii(*native_hostname);
  if (!bounded_hostname.has_value()) {
    const core::Error error = normalize_dependency_error(
        core::DependencyId::libidn2, core::DependencyOperation::idna_lookup,
        core::DependencyStatus::corrupt_data, 0);
    log_dependency_error(log_sink, error);
    return std::unexpected{error};
  }
  return *bounded_hostname;
}

}  // namespace laghu::adapters
