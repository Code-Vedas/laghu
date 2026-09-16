// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <idn2.h>

#include <laghu/adapters/idna.hpp>

namespace laghu::adapters {
namespace {

[[nodiscard]] constexpr bool is_ascii_label_character(char character) noexcept {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '-';
}

[[nodiscard]] constexpr bool is_permitted_ascii_input_character(char character) noexcept {
  return is_ascii_label_character(character) || character == '.';
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

[[nodiscard]] core::DependencyStatus idn2_status(int status) noexcept {
  if (status == IDN2_MALLOC) {
    return core::DependencyStatus::exhaustion;
  }
  if (status == IDN2_NO_CODESET) {
    return core::DependencyStatus::unavailable;
  }
  return core::DependencyStatus::corrupt_data;
}

[[nodiscard]] core::Error idn2_failure(int status, const DependencyLogSink& log_sink) noexcept {
  const core::Error error = normalize_dependency_error(
      core::DependencyId::libidn2, core::DependencyOperation::idna_lookup,
      idn2_status(status), static_cast<std::int32_t>(status));
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
  for (std::size_t index = 0; index < AsciiHostname::storage_capacity; ++index) {
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
  const auto input = hostname.to_c_string<AsciiHostname::storage_capacity>();
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
