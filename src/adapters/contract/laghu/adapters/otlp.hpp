// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

#include <laghu/core/views.hpp>

namespace laghu::adapters {

template <std::size_t Capacity>
struct OtlpText final {
  static_assert(Capacity <= UINT16_MAX);
  std::array<char, Capacity> bytes{};
  std::uint16_t size{};

  [[nodiscard]] static core::Result<OtlpText> from(core::TextView text) noexcept {
    if (text.size() > Capacity) {
      return std::unexpected{core::Error{core::ErrorDomain::core,
          core::ErrorCode::exhaustion, 0, "OTLP text capacity is exhausted"}};
    }
    if (!valid_utf8(text.string_view())) {
      return std::unexpected{core::Error{core::ErrorDomain::core,
          core::ErrorCode::invalid_input, 0, "OTLP text is not valid UTF-8"}};
    }
    OtlpText result;
    for (std::size_t index = 0; index < text.size(); ++index) {
      result.bytes[index] = text.string_view()[index];
    }
    result.size = static_cast<std::uint16_t>(text.size());
    return result;
  }

  [[nodiscard]] constexpr std::string_view value() const noexcept {
    return {bytes.data(), size};
  }

 private:
  [[nodiscard]] static constexpr bool continuation(unsigned char byte) noexcept {
    return (byte & 0xc0U) == 0x80U;
  }

  [[nodiscard]] static constexpr bool valid_utf8(std::string_view text) noexcept {
    std::size_t index{};
    while (index < text.size()) {
      const auto first = static_cast<unsigned char>(text[index]);
      if (first <= 0x7fU) {
        ++index;
      } else if (first >= 0xc2U && first <= 0xdfU) {
        if (index + 1U >= text.size() ||
            !continuation(static_cast<unsigned char>(text[index + 1U]))) return false;
        index += 2U;
      } else if (first >= 0xe0U && first <= 0xefU) {
        if (index + 2U >= text.size()) return false;
        const auto second = static_cast<unsigned char>(text[index + 1U]);
        const auto third = static_cast<unsigned char>(text[index + 2U]);
        if (!continuation(second) || !continuation(third) ||
            (first == 0xe0U && second < 0xa0U) ||
            (first == 0xedU && second >= 0xa0U)) return false;
        index += 3U;
      } else if (first >= 0xf0U && first <= 0xf4U) {
        if (index + 3U >= text.size()) return false;
        const auto second = static_cast<unsigned char>(text[index + 1U]);
        if (!continuation(second) ||
            !continuation(static_cast<unsigned char>(text[index + 2U])) ||
            !continuation(static_cast<unsigned char>(text[index + 3U])) ||
            (first == 0xf0U && second < 0x90U) ||
            (first == 0xf4U && second >= 0x90U)) return false;
        index += 4U;
      } else {
        return false;
      }
    }
    return true;
  }
};

struct OtlpAttribute final {
  OtlpText<64> key{};
  OtlpText<128> value{};
};

struct OtlpMetadata final {
  OtlpText<64> service_name{};
  OtlpText<64> scope_name{};
  OtlpText<32> scope_version{};
  std::array<OtlpAttribute, 16> resource_attributes{};
  std::uint8_t resource_attribute_count{};
};

struct OtlpEvent final {
  std::uint64_t time_unix_nano{};
  OtlpText<128> name{};
  std::array<OtlpAttribute, 8> attributes{};
  std::uint8_t attribute_count{};
};

struct OtlpTraceRecord final {
  std::array<std::byte, 16> trace_id{};
  std::array<std::byte, 8> span_id{};
  OtlpText<128> name{};
  std::uint64_t start_time_unix_nano{};
  std::uint64_t end_time_unix_nano{};
  std::array<OtlpAttribute, 16> attributes{};
  std::uint8_t attribute_count{};
  std::array<OtlpEvent, 8> events{};
  std::uint8_t event_count{};
};

struct OtlpMetricRecord final {
  OtlpText<128> name{};
  OtlpText<64> unit{};
  std::uint64_t time_unix_nano{};
  double value{};
  std::array<OtlpAttribute, 16> attributes{};
  std::uint8_t attribute_count{};
};

struct OtlpLogRecord final {
  std::uint64_t time_unix_nano{};
  std::uint32_t severity_number{};
  OtlpText<32> severity_text{};
  OtlpText<256> body{};
  std::array<std::byte, 16> trace_id{};
  std::array<std::byte, 8> span_id{};
  std::array<OtlpAttribute, 16> attributes{};
  std::uint8_t attribute_count{};
};

struct OtlpEncodeResult final { std::size_t bytes_written{}; };

[[nodiscard]] core::Result<OtlpEncodeResult> encode_otlp_traces(
    const OtlpMetadata& metadata, std::span<const OtlpTraceRecord> records,
    core::MutableByteView output, core::MutableByteView scratch) noexcept;
[[nodiscard]] core::Result<OtlpEncodeResult> encode_otlp_metrics(
    const OtlpMetadata& metadata, std::span<const OtlpMetricRecord> records,
    core::MutableByteView output, core::MutableByteView scratch) noexcept;
[[nodiscard]] core::Result<OtlpEncodeResult> encode_otlp_logs(
    const OtlpMetadata& metadata, std::span<const OtlpLogRecord> records,
    core::MutableByteView output, core::MutableByteView scratch) noexcept;

static_assert(std::is_trivially_copyable_v<OtlpAttribute>);
static_assert(std::is_trivially_copyable_v<OtlpMetadata>);
static_assert(std::is_trivially_copyable_v<OtlpEvent>);
static_assert(std::is_trivially_copyable_v<OtlpTraceRecord>);
static_assert(std::is_trivially_copyable_v<OtlpMetricRecord>);
static_assert(std::is_trivially_copyable_v<OtlpLogRecord>);

}  // namespace laghu::adapters
