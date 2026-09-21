// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstring>
#include <span>

#include <laghu/adapters/otlp.hpp>

#define export export_method
#include "opentelemetry/proto/collector/logs/v1/logs_service.pb-c.h"
#include "opentelemetry/proto/collector/metrics/v1/metrics_service.pb-c.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.pb-c.h"
#undef export
#include "laghu_test_support.hpp"

namespace {
using namespace laghu::adapters;

template <std::size_t Capacity>
[[nodiscard]] OtlpText<Capacity> text(const char* value) noexcept {
  return *OtlpText<Capacity>::from(laghu::core::TextView::from(value));
}

[[nodiscard]] OtlpMetadata metadata() noexcept {
  OtlpMetadata value{};
  value.service_name = text<64>("laghu");
  value.scope_name = text<64>("laghu.test");
  value.scope_version = text<32>("1");
  return value;
}

template <class Record, class Encode, class Unpack, class Free>
[[nodiscard]] bool round_trip(Record record, Encode encode, Unpack unpack,
                              Free release) noexcept {
  std::array<std::byte, 2048> output{};
  std::array<std::byte, 128> scratch{};
  const auto result = encode(metadata(), std::span<const Record>{&record, 1U},
      *laghu::core::MutableByteView::from(output),
      *laghu::core::MutableByteView::from(scratch));
  if (!result.has_value()) return false;
  auto* decoded = unpack(nullptr, result->bytes_written,
      reinterpret_cast<const std::uint8_t*>(output.data()));
  if (decoded == nullptr) return false;
  const bool valid = decoded->base.descriptor != nullptr;
  release(decoded, nullptr);
  return valid;
}

[[nodiscard]] bool all_signals_decode() noexcept {
  OtlpTraceRecord trace{};
  trace.trace_id[0] = std::byte{1}; trace.span_id[0] = std::byte{2};
  trace.name = text<128>("request"); trace.start_time_unix_nano = 10U;
  trace.end_time_unix_nano = 20U;
  OtlpMetricRecord metric{};
  metric.name = text<128>("requests"); metric.unit = text<64>("1");
  metric.time_unix_nano = 20U; metric.value = 2.0;
  OtlpLogRecord log{};
  log.time_unix_nano = 20U; log.severity_number = 9U;
  log.severity_text = text<32>("INFO"); log.body = text<256>("ready");
  return round_trip(trace, encode_otlp_traces,
      opentelemetry__proto__collector__trace__v1__export_trace_service_request__unpack,
      opentelemetry__proto__collector__trace__v1__export_trace_service_request__free_unpacked) &&
      round_trip(metric, encode_otlp_metrics,
      opentelemetry__proto__collector__metrics__v1__export_metrics_service_request__unpack,
      opentelemetry__proto__collector__metrics__v1__export_metrics_service_request__free_unpacked) &&
      round_trip(log, encode_otlp_logs,
      opentelemetry__proto__collector__logs__v1__export_logs_service_request__unpack,
      opentelemetry__proto__collector__logs__v1__export_logs_service_request__free_unpacked);
}

[[nodiscard]] bool boundaries_fail() noexcept {
  OtlpTraceRecord invalid{};
  invalid.name = text<128>("invalid");
  std::array<std::byte, 8> output{};
  std::array<std::byte, 64> scratch{};
  const auto identifier = encode_otlp_traces(metadata(), {&invalid, 1U},
      *laghu::core::MutableByteView::from(output),
      *laghu::core::MutableByteView::from(scratch));
  OtlpLogRecord log{};
  log.time_unix_nano = 1U; log.body = text<256>("x");
  const auto exhausted = encode_otlp_logs(metadata(), {&log, 1U},
      *laghu::core::MutableByteView::from(output),
      *laghu::core::MutableByteView::from(scratch));
  return !identifier.has_value() && !exhausted.has_value();
}

[[nodiscard]] bool empty_batches_decode() noexcept {
  std::array<std::byte, 512> output{};
  std::array<std::byte, 64> scratch{};
  const auto output_view = *laghu::core::MutableByteView::from(output);
  const auto scratch_view = *laghu::core::MutableByteView::from(scratch);
  const auto traces = encode_otlp_traces(metadata(), {}, output_view, scratch_view);
  if (!traces.has_value()) return false;
  auto* decoded_traces =
      opentelemetry__proto__collector__trace__v1__export_trace_service_request__unpack(
          nullptr, traces->bytes_written,
          reinterpret_cast<const std::uint8_t*>(output.data()));
  if (decoded_traces == nullptr || decoded_traces->n_resource_spans != 1U ||
      decoded_traces->resource_spans[0]->n_scope_spans != 1U ||
      decoded_traces->resource_spans[0]->scope_spans[0]->n_spans != 0U) {
    if (decoded_traces != nullptr) {
      opentelemetry__proto__collector__trace__v1__export_trace_service_request__free_unpacked(
          decoded_traces, nullptr);
    }
    return false;
  }
  opentelemetry__proto__collector__trace__v1__export_trace_service_request__free_unpacked(
      decoded_traces, nullptr);
  return encode_otlp_metrics(metadata(), {}, output_view, scratch_view).has_value() &&
      encode_otlp_logs(metadata(), {}, output_view, scratch_view).has_value();
}

[[nodiscard]] bool maximums_and_order_are_deterministic() noexcept {
  OtlpTraceRecord trace{};
  trace.trace_id[0] = std::byte{1};
  trace.span_id[0] = std::byte{2};
  trace.name = text<128>("bounded-trace");
  trace.start_time_unix_nano = 10U;
  trace.end_time_unix_nano = 20U;
  trace.attribute_count = static_cast<std::uint8_t>(trace.attributes.size());
  for (std::size_t index = 0; index < trace.attributes.size(); ++index) {
    trace.attributes[index].key = text<64>("key");
    trace.attributes[index].value = text<128>("value");
  }
  trace.event_count = static_cast<std::uint8_t>(trace.events.size());
  for (auto& event : trace.events) {
    event.time_unix_nano = 15U;
    event.name = text<128>("event");
    event.attribute_count = static_cast<std::uint8_t>(event.attributes.size());
    for (auto& attribute : event.attributes) {
      attribute.key = text<64>("event-key");
      attribute.value = text<128>("event-value");
    }
  }
  auto rich_metadata = metadata();
  rich_metadata.resource_attribute_count =
      static_cast<std::uint8_t>(rich_metadata.resource_attributes.size());
  for (auto& attribute : rich_metadata.resource_attributes) {
    attribute.key = text<64>("resource-key");
    attribute.value = text<128>("resource-value");
  }
  std::array<std::byte, 16384> first{};
  std::array<std::byte, 16384> second{};
  std::array<std::byte, 128> scratch{};
  const auto first_result = encode_otlp_traces(rich_metadata, {&trace, 1U},
      *laghu::core::MutableByteView::from(first),
      *laghu::core::MutableByteView::from(scratch));
  const auto second_result = encode_otlp_traces(rich_metadata, {&trace, 1U},
      *laghu::core::MutableByteView::from(second),
      *laghu::core::MutableByteView::from(scratch));
  return first_result.has_value() && second_result.has_value() &&
      first_result->bytes_written == second_result->bytes_written &&
      std::memcmp(first.data(), second.data(), first_result->bytes_written) == 0;
}
}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"otlp.all-signals-decode", all_signals_decode},
      laghu::test::TestCase{"otlp.boundaries", boundaries_fail},
      laghu::test::TestCase{"otlp.empty-batches", empty_batches_decode},
      laghu::test::TestCase{"otlp.maximums-deterministic",
                            maximums_and_order_are_deterministic}};
  return laghu::test::run_tests(tests);
}
