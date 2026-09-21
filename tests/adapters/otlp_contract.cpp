// SPDX-License-Identifier: AGPL-3.0-only
#include <type_traits>
#include <laghu/adapters/otlp.hpp>

static_assert(std::is_trivially_copyable_v<laghu::adapters::OtlpTraceRecord>);
static_assert(std::is_trivially_copyable_v<laghu::adapters::OtlpMetricRecord>);
static_assert(std::is_trivially_copyable_v<laghu::adapters::OtlpLogRecord>);
int main() { return 0; }
