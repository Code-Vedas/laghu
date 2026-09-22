// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <type_traits>

namespace laghu::core {

enum class ErrorDomain : std::uint8_t {
  core,
  posix,
  dependency,
  crypto,
};

enum class ErrorCode : std::uint8_t {
  invalid_input,
  invalid_range,
  overflow,
  exhaustion,
  unavailable_capability,
  invalid_state,
  cancellation,
  deadline,
  io,
  corrupt_data,
  unsupported_version,
  checksum,
  dependency,
  crypto,
};

enum class Retryability : std::uint8_t {
  never,
  may_retry,
};

enum class SecurityRelevance : std::uint8_t {
  ordinary,
  security_relevant,
};

enum class DependencyStatus : std::uint8_t {
  invalid_input,
  invalid_range,
  unavailable,
  exhaustion,
  unsupported_version,
  corrupt_data,
  checksum,
  crypto,
  io,
  unknown,
};

// These values identify an external boundary without importing or exposing a
// dependency type. `none` preserves the legacy dependency-status factory for
// callers that do not have a more specific adapter identity.
enum class DependencyId : std::uint8_t {
  none,
  openssl,
  libressl,
  yyjson,
  nghttp2,
  ngtcp2,
  nghttp3,
  c_ares,
  pcre2_8bit,
  zlib_ng,
  brotli,
  zstd,
  libmaxminddb,
  libidn2,
  libxcrypt,
  protobuf_c,
};

[[nodiscard]] constexpr bool is_known_dependency_id(DependencyId value) noexcept {
  switch (value) {
    case DependencyId::none:
    case DependencyId::openssl:
    case DependencyId::libressl:
    case DependencyId::yyjson:
    case DependencyId::nghttp2:
    case DependencyId::ngtcp2:
    case DependencyId::nghttp3:
    case DependencyId::c_ares:
    case DependencyId::pcre2_8bit:
    case DependencyId::zlib_ng:
    case DependencyId::brotli:
    case DependencyId::zstd:
    case DependencyId::libmaxminddb:
    case DependencyId::libidn2:
    case DependencyId::libxcrypt:
    case DependencyId::protobuf_c:
      return true;
  }
  return false;
}

enum class DependencyOperation : std::uint8_t {
  none,
  sha256,
  hmac_sha256,
  ed25519_private_key,
  ed25519_sign,
  ed25519_verify,
  spki_decode,
  idna_lookup,
  password_verify,
  json_parse,
  http2_session,
  http2_receive,
  http2_send,
  http2_submit,
  quic_session,
  quic_receive,
  quic_send,
  quic_stream,
  quic_expiry,
  http3_session,
  http3_receive,
  http3_send,
  http3_submit,
  dns_session,
  dns_query,
  regex_compile,
  regex_match,
  codec_initialize,
  codec_process,
  geoip_open,
  geoip_lookup,
};

class Error final {
 public:
  static constexpr std::size_t diagnostic_context_capacity = 96;

  constexpr Error(ErrorDomain domain, ErrorCode code, std::int32_t native_code = 0,
                  std::string_view diagnostic = {}) noexcept
      : domain_(domain), code_(code), native_code_(native_code),
        retryability_(retryability_for(code)), security_relevance_(security_relevance_for(code)) {
    set_diagnostic_context(diagnostic);
  }

  [[nodiscard]] static constexpr Error from_errno(int native_code,
                                                   std::string_view diagnostic = {}) noexcept {
    return Error{ErrorDomain::posix, normalize_errno(native_code),
                 static_cast<std::int32_t>(native_code), diagnostic};
  }

  [[nodiscard]] static constexpr Error from_dependency(
      DependencyStatus status, std::int32_t native_code = 0,
      std::string_view diagnostic = {}) noexcept {
    return from_dependency(DependencyId::none, DependencyOperation::none, status,
                           native_code, diagnostic);
  }

  [[nodiscard]] static constexpr Error from_dependency(
      DependencyId dependency_id, DependencyOperation dependency_operation,
      DependencyStatus status, std::int32_t native_code = 0,
      std::string_view diagnostic = {}) noexcept {
    Error error{ErrorDomain::dependency, normalize_dependency(status), native_code, diagnostic};
    error.dependency_id_ = dependency_id;
    error.dependency_operation_ = dependency_operation;
    error.dependency_status_ = status;
    return error;
  }

  [[nodiscard]] constexpr ErrorDomain domain() const noexcept { return domain_; }
  [[nodiscard]] constexpr ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] constexpr std::int32_t native_code() const noexcept { return native_code_; }
  [[nodiscard]] constexpr Retryability retryability() const noexcept { return retryability_; }
  [[nodiscard]] constexpr SecurityRelevance security_relevance() const noexcept {
    return security_relevance_;
  }
  [[nodiscard]] constexpr DependencyId dependency_id() const noexcept { return dependency_id_; }
  [[nodiscard]] constexpr DependencyOperation dependency_operation() const noexcept {
    return dependency_operation_;
  }
  [[nodiscard]] constexpr DependencyStatus dependency_status() const noexcept {
    return dependency_status_;
  }
  [[nodiscard]] constexpr std::string_view diagnostic_context() const noexcept {
    return {diagnostic_context_.data(), diagnostic_context_length_};
  }
  [[nodiscard]] constexpr const std::array<char, diagnostic_context_capacity>& diagnostic_bytes()
      const noexcept {
    return diagnostic_context_;
  }
  [[nodiscard]] constexpr bool diagnostic_truncated() const noexcept {
    return diagnostic_truncated_;
  }

 private:
  [[nodiscard]] static constexpr ErrorCode normalize_errno(int native_code) noexcept {
    if (native_code == EINVAL) {
      return ErrorCode::invalid_input;
    }
    if (native_code == ERANGE) {
      return ErrorCode::invalid_range;
    }
    if (native_code == EOVERFLOW) {
      return ErrorCode::overflow;
    }
    if (native_code == ENOMEM) {
      return ErrorCode::exhaustion;
    }
    if (native_code == ECANCELED) {
      return ErrorCode::cancellation;
    }
    if (native_code == ETIMEDOUT) {
      return ErrorCode::deadline;
    }
    if (native_code == EILSEQ || native_code == EBADMSG) {
      return ErrorCode::corrupt_data;
    }
    if (native_code == ENOSYS || native_code == ENOTSUP || native_code == EOPNOTSUPP) {
      return ErrorCode::unavailable_capability;
    }
    return ErrorCode::io;
  }

  [[nodiscard]] static constexpr ErrorCode normalize_dependency(DependencyStatus status) noexcept {
    switch (status) {
      case DependencyStatus::invalid_input:
        return ErrorCode::invalid_input;
      case DependencyStatus::invalid_range:
        return ErrorCode::invalid_range;
      case DependencyStatus::unavailable:
        return ErrorCode::unavailable_capability;
      case DependencyStatus::exhaustion:
        return ErrorCode::exhaustion;
      case DependencyStatus::unsupported_version:
        return ErrorCode::unsupported_version;
      case DependencyStatus::corrupt_data:
        return ErrorCode::corrupt_data;
      case DependencyStatus::checksum:
        return ErrorCode::checksum;
      case DependencyStatus::crypto:
        return ErrorCode::crypto;
      case DependencyStatus::io:
        return ErrorCode::io;
      case DependencyStatus::unknown:
        return ErrorCode::dependency;
    }
    return ErrorCode::dependency;
  }

  [[nodiscard]] static constexpr Retryability retryability_for(ErrorCode code) noexcept {
    switch (code) {
      case ErrorCode::exhaustion:
      case ErrorCode::deadline:
      case ErrorCode::io:
      case ErrorCode::dependency:
        return Retryability::may_retry;
      case ErrorCode::invalid_input:
      case ErrorCode::invalid_range:
      case ErrorCode::overflow:
      case ErrorCode::unavailable_capability:
      case ErrorCode::invalid_state:
      case ErrorCode::cancellation:
      case ErrorCode::corrupt_data:
      case ErrorCode::unsupported_version:
      case ErrorCode::checksum:
      case ErrorCode::crypto:
        return Retryability::never;
    }
    return Retryability::never;
  }

  [[nodiscard]] static constexpr SecurityRelevance security_relevance_for(ErrorCode code) noexcept {
    switch (code) {
      case ErrorCode::corrupt_data:
      case ErrorCode::checksum:
      case ErrorCode::crypto:
        return SecurityRelevance::security_relevant;
      case ErrorCode::invalid_input:
      case ErrorCode::invalid_range:
      case ErrorCode::overflow:
      case ErrorCode::exhaustion:
      case ErrorCode::unavailable_capability:
      case ErrorCode::invalid_state:
      case ErrorCode::cancellation:
      case ErrorCode::deadline:
      case ErrorCode::io:
      case ErrorCode::unsupported_version:
      case ErrorCode::dependency:
        return SecurityRelevance::ordinary;
    }
    return SecurityRelevance::ordinary;
  }

  constexpr void set_diagnostic_context(std::string_view diagnostic) noexcept {
    for (char& byte : diagnostic_context_) {
      byte = '\0';
    }
    const std::size_t copied = diagnostic.size() < diagnostic_context_capacity
                                   ? diagnostic.size()
                                   : diagnostic_context_capacity;
    for (std::size_t index = 0; index < copied; ++index) {
      diagnostic_context_[index] = diagnostic[index];
    }
    diagnostic_context_length_ = static_cast<std::uint8_t>(copied);
    diagnostic_truncated_ = diagnostic.size() > diagnostic_context_capacity;
  }

  ErrorDomain domain_;
  ErrorCode code_;
  std::int32_t native_code_;
  Retryability retryability_;
  SecurityRelevance security_relevance_;
  DependencyId dependency_id_{DependencyId::none};
  DependencyOperation dependency_operation_{DependencyOperation::none};
  DependencyStatus dependency_status_{DependencyStatus::unknown};
  std::array<char, diagnostic_context_capacity> diagnostic_context_{};
  std::uint8_t diagnostic_context_length_{};
  bool diagnostic_truncated_{};
};

static_assert(sizeof(std::array<char, Error::diagnostic_context_capacity>) == 96);
static_assert(std::is_trivially_copyable_v<Error>);
static_assert(std::is_standard_layout_v<Error>);

template <class T>
using Result = std::expected<T, Error>;

[[nodiscard]] int compiler_family_id() noexcept;
}  // namespace laghu::core
