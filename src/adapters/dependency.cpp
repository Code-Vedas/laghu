// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <laghu/adapters/dependency.hpp>
#include <laghu/adapters/internal/dependency.hpp>

namespace laghu::adapters {
namespace {

[[nodiscard]] constexpr core::DependencyId normalize_dependency_id(
    core::DependencyId value) noexcept {
  switch (value) {
    case core::DependencyId::none:
    case core::DependencyId::openssl:
    case core::DependencyId::libressl:
    case core::DependencyId::yyjson:
    case core::DependencyId::nghttp2:
    case core::DependencyId::ngtcp2:
    case core::DependencyId::nghttp3:
    case core::DependencyId::c_ares:
    case core::DependencyId::pcre2_8bit:
    case core::DependencyId::zlib_ng:
    case core::DependencyId::brotli:
    case core::DependencyId::zstd:
    case core::DependencyId::libmaxminddb:
    case core::DependencyId::libidn2:
    case core::DependencyId::libxcrypt:
    case core::DependencyId::protobuf_c:
      return value;
  }
  return core::DependencyId::none;
}

[[nodiscard]] constexpr core::DependencyOperation normalize_dependency_operation(
    core::DependencyOperation value) noexcept {
  switch (value) {
    case core::DependencyOperation::none:
    case core::DependencyOperation::sha256:
    case core::DependencyOperation::hmac_sha256:
    case core::DependencyOperation::ed25519_private_key:
    case core::DependencyOperation::ed25519_sign:
    case core::DependencyOperation::ed25519_verify:
    case core::DependencyOperation::spki_decode:
    case core::DependencyOperation::idna_lookup:
    case core::DependencyOperation::password_verify:
    case core::DependencyOperation::json_parse:
    case core::DependencyOperation::http2_session:
    case core::DependencyOperation::http2_receive:
    case core::DependencyOperation::http2_send:
    case core::DependencyOperation::http2_submit:
    case core::DependencyOperation::quic_session:
    case core::DependencyOperation::quic_receive:
    case core::DependencyOperation::quic_send:
    case core::DependencyOperation::quic_stream:
    case core::DependencyOperation::quic_expiry:
    case core::DependencyOperation::http3_session:
    case core::DependencyOperation::http3_receive:
    case core::DependencyOperation::http3_send:
    case core::DependencyOperation::http3_submit:
    case core::DependencyOperation::dns_session:
    case core::DependencyOperation::dns_query:
    case core::DependencyOperation::regex_compile:
    case core::DependencyOperation::regex_match:
      return value;
  }
  return core::DependencyOperation::none;
}

[[nodiscard]] constexpr core::DependencyStatus normalize_dependency_status(
    core::DependencyStatus value) noexcept {
  switch (value) {
    case core::DependencyStatus::invalid_input:
    case core::DependencyStatus::invalid_range:
    case core::DependencyStatus::unavailable:
    case core::DependencyStatus::exhaustion:
    case core::DependencyStatus::unsupported_version:
    case core::DependencyStatus::corrupt_data:
    case core::DependencyStatus::checksum:
    case core::DependencyStatus::crypto:
    case core::DependencyStatus::io:
    case core::DependencyStatus::unknown:
      return value;
  }
  return core::DependencyStatus::unknown;
}

[[nodiscard]] constexpr std::string_view dependency_name(core::DependencyId value) noexcept {
  switch (value) {
    case core::DependencyId::none:
      return "none";
    case core::DependencyId::openssl:
      return "openssl";
    case core::DependencyId::libressl:
      return "libressl";
    case core::DependencyId::yyjson:
      return "yyjson";
    case core::DependencyId::nghttp2:
      return "nghttp2";
    case core::DependencyId::ngtcp2:
      return "ngtcp2";
    case core::DependencyId::nghttp3:
      return "nghttp3";
    case core::DependencyId::c_ares:
      return "c_ares";
    case core::DependencyId::pcre2_8bit:
      return "pcre2_8bit";
    case core::DependencyId::zlib_ng:
      return "zlib_ng";
    case core::DependencyId::brotli:
      return "brotli";
    case core::DependencyId::zstd:
      return "zstd";
    case core::DependencyId::libmaxminddb:
      return "libmaxminddb";
    case core::DependencyId::libidn2:
      return "libidn2";
    case core::DependencyId::libxcrypt:
      return "libxcrypt";
    case core::DependencyId::protobuf_c:
      return "protobuf_c";
  }
  return "none";
}

[[nodiscard]] constexpr std::string_view operation_name(
    core::DependencyOperation value) noexcept {
  switch (value) {
    case core::DependencyOperation::none:
      return "none";
    case core::DependencyOperation::sha256:
      return "sha256";
    case core::DependencyOperation::hmac_sha256:
      return "hmac_sha256";
    case core::DependencyOperation::ed25519_private_key:
      return "ed25519_private_key";
    case core::DependencyOperation::ed25519_sign:
      return "ed25519_sign";
    case core::DependencyOperation::ed25519_verify:
      return "ed25519_verify";
    case core::DependencyOperation::spki_decode:
      return "spki_decode";
    case core::DependencyOperation::idna_lookup:
      return "idna_lookup";
    case core::DependencyOperation::password_verify:
      return "password_verify";
    case core::DependencyOperation::json_parse:
      return "json_parse";
    case core::DependencyOperation::http2_session:
      return "http2_session";
    case core::DependencyOperation::http2_receive:
      return "http2_receive";
    case core::DependencyOperation::http2_send:
      return "http2_send";
    case core::DependencyOperation::http2_submit:
      return "http2_submit";
    case core::DependencyOperation::quic_session:
      return "quic_session";
    case core::DependencyOperation::quic_receive:
      return "quic_receive";
    case core::DependencyOperation::quic_send:
      return "quic_send";
    case core::DependencyOperation::quic_stream:
      return "quic_stream";
    case core::DependencyOperation::quic_expiry:
      return "quic_expiry";
    case core::DependencyOperation::http3_session:
      return "http3_session";
    case core::DependencyOperation::http3_receive:
      return "http3_receive";
    case core::DependencyOperation::http3_send:
      return "http3_send";
    case core::DependencyOperation::http3_submit:
      return "http3_submit";
    case core::DependencyOperation::dns_session:
      return "dns_session";
    case core::DependencyOperation::dns_query:
      return "dns_query";
    case core::DependencyOperation::regex_compile:
      return "regex_compile";
    case core::DependencyOperation::regex_match:
      return "regex_match";
  }
  return "none";
}

[[nodiscard]] constexpr std::string_view status_name(core::DependencyStatus value) noexcept {
  switch (value) {
    case core::DependencyStatus::invalid_input:
      return "invalid_input";
    case core::DependencyStatus::invalid_range:
      return "invalid_range";
    case core::DependencyStatus::unavailable:
      return "unavailable";
    case core::DependencyStatus::exhaustion:
      return "exhaustion";
    case core::DependencyStatus::unsupported_version:
      return "unsupported_version";
    case core::DependencyStatus::corrupt_data:
      return "corrupt_data";
    case core::DependencyStatus::checksum:
      return "checksum";
    case core::DependencyStatus::crypto:
      return "crypto";
    case core::DependencyStatus::io:
      return "io";
    case core::DependencyStatus::unknown:
      return "unknown";
  }
  return "unknown";
}

template <std::size_t Capacity>
constexpr void append_text(std::array<char, Capacity>& output, std::size_t& size,
                           std::string_view value) noexcept {
  const std::size_t remaining = Capacity - size;
  const std::size_t copied = value.size() < remaining ? value.size() : remaining;
  for (std::size_t index = 0; index < copied; ++index) {
    output[size + index] = value[index];
  }
  size += copied;
}

template <std::size_t Capacity>
constexpr void set_text(std::array<char, Capacity>& output, std::uint8_t& size,
                        std::string_view value) noexcept {
  std::size_t copied{};
  append_text(output, copied, value);
  size = static_cast<std::uint8_t>(copied);
}

[[nodiscard]] constexpr DependencyLogLevel log_level_for(const core::Error& error) noexcept {
  if (error.retryability() == core::Retryability::may_retry &&
      error.security_relevance() == core::SecurityRelevance::ordinary) {
    return DependencyLogLevel::warning;
  }
  return DependencyLogLevel::error;
}

}  // namespace

core::Error normalize_dependency_error(core::DependencyId dependency,
                                       core::DependencyOperation operation,
                                       core::DependencyStatus status,
                                       std::int32_t native_code) noexcept {
  const core::DependencyId normalized_dependency = normalize_dependency_id(dependency);
  const core::DependencyOperation normalized_operation =
      normalize_dependency_operation(operation);
  const core::DependencyStatus normalized_status = normalize_dependency_status(status);
  std::array<char, core::Error::diagnostic_context_capacity> diagnostic{};
  std::size_t diagnostic_size{};
  append_text(diagnostic, diagnostic_size, "dependency=");
  append_text(diagnostic, diagnostic_size, dependency_name(normalized_dependency));
  append_text(diagnostic, diagnostic_size, " operation=");
  append_text(diagnostic, diagnostic_size, operation_name(normalized_operation));
  append_text(diagnostic, diagnostic_size, " status=");
  append_text(diagnostic, diagnostic_size, status_name(normalized_status));
  return core::Error::from_dependency(normalized_dependency, normalized_operation,
                                      normalized_status, native_code,
                                      {diagnostic.data(), diagnostic_size});
}

void log_dependency_error(const DependencyLogSink& sink, const core::Error& error) noexcept {
  if (!sink.enabled() || error.domain() != core::ErrorDomain::dependency) {
    return;
  }
  DependencyLogRecord record{};
  record.level = log_level_for(error);
  record.dependency = normalize_dependency_id(error.dependency_id());
  record.operation = normalize_dependency_operation(error.dependency_operation());
  record.status = normalize_dependency_status(error.dependency_status());
  record.native_code = error.native_code();
  set_text(record.component_bytes, record.component_size, dependency_name(record.dependency));
  std::array<char, DependencyLogRecord::message_capacity> message{};
  std::size_t message_size{};
  append_text(message, message_size, "operation=");
  append_text(message, message_size, operation_name(record.operation));
  append_text(message, message_size, " status=");
  append_text(message, message_size, status_name(record.status));
  set_text(record.message_bytes, record.message_size,
           {message.data(), message_size});
  sink.write(sink.context, record);
}

}  // namespace laghu::adapters

namespace laghu::adapters::internal {

void dependency_failure_callback(void* context, std::int32_t native_code) noexcept {
  if (context == nullptr) {
    return;
  }
  auto& state = *static_cast<DependencyCallbackState*>(context);
  state.error = normalize_dependency_error(state.dependency, state.operation, state.status,
                                           native_code);
  state.failed = true;
  log_dependency_error(state.log_sink, state.error);
}

}  // namespace laghu::adapters::internal
