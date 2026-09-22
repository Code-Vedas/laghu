// SPDX-License-Identifier: AGPL-3.0-only
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <laghu/adapters/otlp.hpp>

namespace laghu::adapters {
namespace {

template <std::size_t Size>
[[nodiscard]] bool nonzero(const std::array<std::byte, Size>& value) noexcept {
  for (const std::byte byte : value) if (byte != std::byte{}) return true;
  return false;
}

[[nodiscard]] core::Error invalid(const char* text) noexcept {
  return {core::ErrorDomain::core, core::ErrorCode::invalid_input, 0, text};
}

[[nodiscard]] constexpr std::size_t varint_size(std::uint64_t value) noexcept {
  std::size_t result{1};
  while (value >= 0x80U) { value >>= 7U; ++result; }
  return result;
}

class Sizer final {
 public:
  bool varint(std::uint64_t value) noexcept { used_ += varint_size(value); return true; }
  bool tag(std::uint32_t field, std::uint8_t wire) noexcept {
    return varint((static_cast<std::uint64_t>(field) << 3U) | wire);
  }
  bool bytes(std::uint32_t field, std::span<const std::byte> value) noexcept {
    const std::size_t size = value.size();
    used_ += varint_size((static_cast<std::uint64_t>(field) << 3U) | 2U) +
        varint_size(size) + size;
    return true;
  }
  bool text(std::uint32_t field, std::string_view value) noexcept {
    used_ += varint_size((static_cast<std::uint64_t>(field) << 3U) | 2U) +
        varint_size(value.size()) + value.size();
    return true;
  }
  bool fixed64(std::uint32_t field, std::uint64_t) noexcept {
    used_ += varint_size((static_cast<std::uint64_t>(field) << 3U) | 1U) + 8U;
    return true;
  }
  template <class Encoder> bool message(std::uint32_t field, Encoder encoder) noexcept {
    Sizer child;
    if (!encoder(child)) return false;
    used_ += varint_size((static_cast<std::uint64_t>(field) << 3U) | 2U) +
        varint_size(child.used()) + child.used();
    return true;
  }
  [[nodiscard]] std::size_t used() const noexcept { return used_; }
 private:
  std::size_t used_{};
};

class Writer final {
 public:
  explicit Writer(std::span<std::byte> output) noexcept : output_(output) {}
  bool varint(std::uint64_t value) noexcept {
    do {
      if (used_ == output_.size()) return false;
      auto byte = static_cast<std::uint8_t>(value & 0x7fU);
      value >>= 7U;
      if (value != 0U) byte |= 0x80U;
      output_[used_++] = static_cast<std::byte>(byte);
    } while (value != 0U);
    return true;
  }
  bool tag(std::uint32_t field, std::uint8_t wire) noexcept {
    return varint((static_cast<std::uint64_t>(field) << 3U) | wire);
  }
  bool bytes(std::uint32_t field, std::span<const std::byte> value) noexcept {
    const std::size_t size = value.size();
    if (!tag(field, 2U) || !varint(size) || size > output_.size() - used_) return false;
    for (const std::byte byte : value) output_[used_++] = byte;
    return true;
  }
  bool text(std::uint32_t field, std::string_view value) noexcept {
    if (!tag(field, 2U) || !varint(value.size()) ||
        value.size() > output_.size() - used_) return false;
    for (const char character : value) {
      output_[used_++] = static_cast<std::byte>(character);
    }
    return true;
  }
  bool fixed64(std::uint32_t field, std::uint64_t value) noexcept {
    if (!tag(field, 1U) || output_.size() - used_ < 8U) return false;
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
      output_[used_++] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
    return true;
  }
  template <class Encoder> bool message(std::uint32_t field, Encoder encoder) noexcept {
    Sizer child;
    return encoder(child) && tag(field, 2U) && varint(child.used()) && encoder(*this);
  }
  [[nodiscard]] std::size_t used() const noexcept { return used_; }
 private:
  std::span<std::byte> output_;
  std::size_t used_{};
};

template <class Output>
bool encode_attribute(Output& output, const OtlpAttribute& attribute) noexcept {
  return output.text(1U, attribute.key.value()) &&
      output.message(2U, [&](auto& value) {
        return value.text(1U, attribute.value.value());
      });
}

template <class Output, std::size_t Size>
bool encode_optional_bytes(Output& output, std::uint32_t field,
                           const std::array<std::byte, Size>& value) noexcept {
  return !nonzero(value) || output.bytes(field, value);
}

template <class Output, class Record>
bool encode_attributes(Output& output, const Record& record, std::uint32_t field) noexcept {
  if (record.attribute_count > record.attributes.size()) return false;
  for (std::size_t index = 0; index < record.attribute_count; ++index) {
    if (!output.message(field, [&](auto& item) {
          return encode_attribute(item, record.attributes[index]);
        })) return false;
  }
  return true;
}

template <class Output>
bool encode_resource(Output& output, const OtlpMetadata& metadata) noexcept {
  if (metadata.resource_attribute_count > metadata.resource_attributes.size()) return false;
  OtlpAttribute service{};
  service.key = *OtlpText<64>::from(core::TextView::from("service.name"));
  service.value = *OtlpText<128>::from(core::TextView::from(metadata.service_name.value()));
  if (!output.message(1U, [&](auto& item) { return encode_attribute(item, service); })) return false;
  for (std::size_t index = 0; index < metadata.resource_attribute_count; ++index) {
    if (!output.message(1U, [&](auto& item) {
          return encode_attribute(item, metadata.resource_attributes[index]);
        })) return false;
  }
  return true;
}

template <class Record>
[[nodiscard]] bool valid_attribute_count(const Record& record) noexcept {
  return record.attribute_count <= record.attributes.size();
}

[[nodiscard]] bool valid_metadata(const OtlpMetadata& metadata) noexcept {
  return metadata.resource_attribute_count <= metadata.resource_attributes.size();
}

template <class Record, class EncodeRecord>
core::Result<OtlpEncodeResult> encode_request(
    const OtlpMetadata& metadata, std::span<const Record> records,
    core::MutableByteView output, core::MutableByteView scratch,
    EncodeRecord encode_record) noexcept {
  if (records.size() > 64U || scratch.size() < 64U) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
        core::ErrorCode::exhaustion, 0, "OTLP batch or scratch capacity is exhausted"}};
  }
  Writer writer{output.span()};
  const bool valid = writer.message(1U, [&](auto& resource_records) {
    return resource_records.message(1U, [&](auto& resource) {
      return encode_resource(resource, metadata);
    }) && resource_records.message(2U, [&](auto& scope_records) {
      if (!scope_records.message(1U, [&](auto& scope) {
            return scope.text(1U, metadata.scope_name.value()) &&
                scope.text(2U, metadata.scope_version.value());
          })) return false;
      for (const auto& record : records) {
        if (!scope_records.message(2U, [&](auto& item) {
              return encode_record(item, record);
            })) return false;
      }
      return true;
    });
  });
  if (!valid) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
        core::ErrorCode::exhaustion, 0, "OTLP output capacity is exhausted"}};
  }
  return OtlpEncodeResult{writer.used()};
}

}  // namespace

core::Result<OtlpEncodeResult> encode_otlp_traces(
    const OtlpMetadata& metadata, std::span<const OtlpTraceRecord> records,
    core::MutableByteView output, core::MutableByteView scratch) noexcept {
  if (!valid_metadata(metadata)) {
    return std::unexpected{invalid("OTLP resource attribute count is invalid")};
  }
  for (const auto& record : records) {
    if (!nonzero(record.trace_id) || !nonzero(record.span_id) ||
        record.name.empty() || record.start_time_unix_nano == 0U ||
        record.end_time_unix_nano < record.start_time_unix_nano ||
        record.event_count > record.events.size() || !valid_attribute_count(record)) {
      return std::unexpected{invalid("OTLP trace identifier or time range is invalid")};
    }
    for (std::size_t index = 0; index < record.event_count; ++index) {
      if (record.events[index].name.empty() ||
          record.events[index].time_unix_nano == 0U ||
          !valid_attribute_count(record.events[index])) {
        return std::unexpected{invalid("OTLP trace event is invalid")};
      }
    }
  }
  return encode_request(metadata, records, output, scratch,
      [](auto& writer, const OtlpTraceRecord& record) {
        return writer.bytes(1U, record.trace_id) &&
            writer.bytes(2U, record.span_id) &&
            writer.text(5U, record.name.value()) &&
            writer.fixed64(7U, record.start_time_unix_nano) &&
            writer.fixed64(8U, record.end_time_unix_nano) &&
            encode_attributes(writer, record, 9U) && [&] {
              for (std::size_t index = 0; index < record.event_count; ++index) {
                const auto& event = record.events[index];
                if (!writer.message(11U, [&](auto& item) {
                      return item.fixed64(1U, event.time_unix_nano) &&
                          item.text(2U, event.name.value()) &&
                          encode_attributes(item, event, 3U);
                    })) return false;
              }
              return true;
            }();
      });
}

core::Result<OtlpEncodeResult> encode_otlp_metrics(
    const OtlpMetadata& metadata, std::span<const OtlpMetricRecord> records,
    core::MutableByteView output, core::MutableByteView scratch) noexcept {
  if (!valid_metadata(metadata)) {
    return std::unexpected{invalid("OTLP resource attribute count is invalid")};
  }
  for (const auto& record : records) {
    if (record.name.empty() || record.time_unix_nano == 0U ||
        !valid_attribute_count(record)) {
      return std::unexpected{invalid("OTLP metric name or timestamp is invalid")};
    }
  }
  return encode_request(metadata, records, output, scratch,
      [](auto& writer, const OtlpMetricRecord& record) {
        return writer.text(1U, record.name.value()) &&
            writer.text(3U, record.unit.value()) &&
            writer.message(5U, [&](auto& gauge) {
              return gauge.message(1U, [&](auto& point) {
                return point.fixed64(3U, record.time_unix_nano) &&
                    point.fixed64(4U, std::bit_cast<std::uint64_t>(record.value)) &&
                    encode_attributes(point, record, 7U);
              });
            });
      });
}

core::Result<OtlpEncodeResult> encode_otlp_logs(
    const OtlpMetadata& metadata, std::span<const OtlpLogRecord> records,
    core::MutableByteView output, core::MutableByteView scratch) noexcept {
  if (!valid_metadata(metadata)) {
    return std::unexpected{invalid("OTLP resource attribute count is invalid")};
  }
  for (const auto& record : records) {
    if (record.body.empty() || record.time_unix_nano == 0U ||
        (nonzero(record.span_id) && !nonzero(record.trace_id)) ||
        !valid_attribute_count(record)) {
      return std::unexpected{invalid("OTLP log body, timestamp, or correlation identifier is invalid")};
    }
  }
  return encode_request(metadata, records, output, scratch,
      [](auto& writer, const OtlpLogRecord& record) {
        return writer.fixed64(1U, record.time_unix_nano) &&
            writer.tag(2U, 0U) && writer.varint(record.severity_number) &&
            writer.text(3U, record.severity_text.value()) &&
            writer.message(5U, [&](auto& body) {
              return body.text(1U, record.body.value());
            }) && encode_attributes(writer, record, 6U) &&
            encode_optional_bytes(writer, 9U, record.trace_id) &&
            encode_optional_bytes(writer, 10U, record.span_id);
      });
}

}  // namespace laghu::adapters
