// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

#include <fcntl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

#include <laghu/benchmark_identity.hpp>
#include <laghu/benchmark/internal/metrics.hpp>
#include <laghu/benchmark/internal/workload.hpp>

namespace {

using laghu::benchmark::internal::Percentiles;
using laghu::benchmark::internal::WorkloadCounters;

constexpr std::size_t maximum_intervals = 1000U;
constexpr std::uint64_t maximum_warmup_intervals = 10000U;
constexpr std::size_t diagnostic_capacity = 256U;
constexpr std::uint64_t nanoseconds_per_second = 1000000000U;
// A report can retain all 1,000 measured interval values without truncating
// their observed order for every supported regression metric. It remains a
// bounded, build-local benchmark artifact.
constexpr std::size_t json_capacity = 262144U;

enum class ExitCode : int {
  success = 0,
  invalid_arguments = 64,
  metric_unavailable = 69,
  output_failure = 74,
};

struct Options final {
  std::uint64_t warmup_intervals{};
  std::size_t measured_intervals{};
  std::string_view workload{};
};

struct TextMetric final {
  bool available{};
  std::array<char, diagnostic_capacity> text{};
  std::size_t size{};
};

struct NumericMetric final {
  bool available{};
  std::uint64_t value{};
  std::string_view reason{};
};

class JsonWriter final {
 public:
  [[nodiscard]] bool append(std::string_view text) noexcept {
    if (text.size() > output_.size() - size_) {
      return false;
    }
    std::memcpy(output_.data() + size_, text.data(), text.size());
    size_ += text.size();
    return true;
  }

  [[nodiscard]] bool append_number(std::uint64_t value) noexcept {
    const auto converted = std::to_chars(output_.data() + size_, output_.data() + output_.size(), value);
    if (converted.ec != std::errc{}) {
      return false;
    }
    size_ = static_cast<std::size_t>(converted.ptr - output_.data());
    return true;
  }

  [[nodiscard]] bool append_json_string(std::string_view value) noexcept {
    if (!append("\"")) {
      return false;
    }
    for (const char character : value) {
      switch (character) {
        case '\\':
          if (!append("\\\\")) {
            return false;
          }
          break;
        case '"':
          if (!append("\\\"")) {
            return false;
          }
          break;
        case '\n':
          if (!append("\\n")) {
            return false;
          }
          break;
        case '\r':
          if (!append("\\r")) {
            return false;
          }
          break;
        case '\t':
          if (!append("\\t")) {
            return false;
          }
          break;
        default:
          if (static_cast<unsigned char>(character) < 0x20U || !append({&character, 1U})) {
            return false;
          }
          break;
      }
    }
    return append("\"");
  }

  [[nodiscard]] std::string_view view() const noexcept { return {output_.data(), size_}; }

 private:
  std::array<char, json_capacity> output_{};
  std::size_t size_{};
};

[[nodiscard]] bool write_all(int descriptor, std::string_view text) noexcept {
  while (!text.empty()) {
    const ssize_t written = ::write(descriptor, text.data(), text.size());
    if (written > 0) {
      text.remove_prefix(static_cast<std::size_t>(written));
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] bool parse_positive(std::string_view text, std::uint64_t maximum,
                                  std::uint64_t& output) noexcept {
  if (text.empty()) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), output);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() && output <= maximum;
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) noexcept {
  bool workload_seen = false;
  bool warmup_seen = false;
  bool intervals_seen = false;
  for (int argument = 1; argument < argc; ++argument) {
    const std::string_view option{argv[argument]};
    if (argument + 1 >= argc) {
      return false;
    }
    const std::string_view value{argv[++argument]};
    if (option == "--workload" && !workload_seen && value == "core-foundation") {
      options.workload = value;
      workload_seen = true;
      continue;
    }
    if (option == "--warmup" && !warmup_seen &&
        parse_positive(value, maximum_warmup_intervals, options.warmup_intervals)) {
      warmup_seen = true;
      continue;
    }
    std::uint64_t intervals{};
    if (option == "--intervals" && !intervals_seen &&
        parse_positive(value, maximum_intervals, intervals) && intervals != 0U) {
      options.measured_intervals = static_cast<std::size_t>(intervals);
      intervals_seen = true;
      continue;
    }
    return false;
  }
  return workload_seen && warmup_seen && intervals_seen;
}

[[nodiscard]] bool timespec_nanoseconds(const timespec& value, std::uint64_t& output) noexcept {
  if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= static_cast<long>(nanoseconds_per_second)) {
    return false;
  }
  const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
  const auto nanoseconds = static_cast<std::uint64_t>(value.tv_nsec);
  if (seconds > (std::numeric_limits<std::uint64_t>::max() - nanoseconds) / nanoseconds_per_second) {
    return false;
  }
  output = seconds * nanoseconds_per_second + nanoseconds;
  return true;
}

[[nodiscard]] bool monotonic_now(std::uint64_t& output) noexcept {
  timespec value{};
  return ::clock_gettime(CLOCK_MONOTONIC, &value) == 0 && timespec_nanoseconds(value, output);
}

[[nodiscard]] NumericMetric process_cpu_time() noexcept {
  timespec value{};
  std::uint64_t nanoseconds{};
  if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0 || !timespec_nanoseconds(value, nanoseconds)) {
    return NumericMetric{false, 0U, "clock_gettime_process_cpu_time_unavailable"};
  }
  return NumericMetric{true, nanoseconds, {}};
}

[[nodiscard]] NumericMetric peak_rss_bytes() noexcept {
  rusage usage{};
  if (::getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
    return NumericMetric{false, 0U, "getrusage_peak_rss_unavailable"};
  }
#if defined(__APPLE__)
  return NumericMetric{true, static_cast<std::uint64_t>(usage.ru_maxrss), {}};
#elif defined(__linux__) || defined(__FreeBSD__)
  const auto kilobytes = static_cast<std::uint64_t>(usage.ru_maxrss);
  if (kilobytes > std::numeric_limits<std::uint64_t>::max() / 1024U) {
    return NumericMetric{false, 0U, "peak_rss_overflow"};
  }
  return NumericMetric{true, kilobytes * 1024U, {}};
#else
  return NumericMetric{false, 0U, "peak_rss_unit_platform_unsupported"};
#endif
}

#if defined(__linux__)
[[nodiscard]] bool copy_metric_text(TextMetric& metric, std::string_view text) noexcept {
  if (text.empty() || text.size() >= metric.text.size()) {
    return false;
  }
  std::memmove(metric.text.data(), text.data(), text.size());
  metric.size = text.size();
  metric.available = true;
  return true;
}
#endif

[[nodiscard]] TextMetric cpu_description() noexcept {
  TextMetric metric{};
#if defined(__APPLE__) || defined(__FreeBSD__)
  #if defined(__APPLE__)
  constexpr std::string_view cpu_sysctl{"machdep.cpu.brand_string"};
  #else
  constexpr std::string_view cpu_sysctl{"hw.model"};
  #endif
  std::size_t size = metric.text.size() - 1U;
  if (::sysctlbyname(cpu_sysctl.data(), metric.text.data(), &size, nullptr, 0U) == 0 &&
      size != 0U && size < metric.text.size()) {
    metric.size = size;
    if (metric.text[metric.size - 1U] == '\0') {
      --metric.size;
    }
    metric.text[metric.size] = '\0';
    metric.available = metric.size != 0U;
  }
#elif defined(__linux__)
  int flags = O_RDONLY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  const int descriptor = ::open("/proc/cpuinfo", flags);
  if (descriptor >= 0) {
    const ssize_t read_count = ::read(descriptor, metric.text.data(), metric.text.size() - 1U);
    static_cast<void>(::close(descriptor));
    if (read_count > 0) {
      const auto size = static_cast<std::size_t>(read_count);
      metric.text[size] = '\0';
      const std::string_view source{metric.text.data(), size};
      for (const std::string_view label : {std::string_view{"model name"}, std::string_view{"Hardware"}}) {
        const std::size_t label_position = source.find(label);
        if (label_position == std::string_view::npos) {
          continue;
        }
        const std::size_t separator = source.find(':', label_position + label.size());
        const std::size_t line_end = source.find('\n', separator);
        if (separator == std::string_view::npos || line_end == std::string_view::npos) {
          continue;
        }
        std::string_view value = source.substr(separator + 1U, line_end - separator - 1U);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
          value.remove_prefix(1U);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
          value.remove_suffix(1U);
        }
        if (copy_metric_text(metric, value)) {
          return metric;
        }
      }
    }
  }
#endif
  return metric;
}

[[nodiscard]] bool append_samples(JsonWriter& writer,
                                  const std::array<std::uint64_t, maximum_intervals>& samples,
                                  std::size_t count) noexcept {
  if (!writer.append("[")) {
    return false;
  }
  for (std::size_t index = 0U; index < count; ++index) {
    if ((index != 0U && !writer.append(",")) || !writer.append_number(samples[index])) {
      return false;
    }
  }
  return writer.append("]");
}

[[nodiscard]] bool append_interval_metric(
    JsonWriter& writer, const NumericMetric& metric,
    const std::array<std::uint64_t, maximum_intervals>& samples, std::size_t count,
    std::string_view samples_key) noexcept {
  if (!writer.append("{\"status\":\"")) {
    return false;
  }
  if (!metric.available) {
    return writer.append("unavailable\",\"reason\":") && writer.append_json_string(metric.reason) &&
        writer.append("}");
  }
  return writer.append("available\",\"value\":") && writer.append_number(metric.value) &&
      writer.append(",\"") && writer.append(samples_key) && writer.append("\":") &&
      append_samples(writer, samples, count) && writer.append("}");
}

[[nodiscard]] bool append_counter_metric(
    JsonWriter& writer, bool instrumented, std::uint64_t value,
    const std::array<std::uint64_t, maximum_intervals>& samples, std::size_t count,
    std::string_view unavailable_reason) noexcept {
  if (!writer.append("{\"instrumented\":") ||
      !writer.append(instrumented ? "true" : "false") ||
      !writer.append(",\"value\":") || !writer.append_number(value) ||
      !writer.append(",\"status\":\"")) {
    return false;
  }
  if (!instrumented) {
    return writer.append("unavailable\",\"reason\":") &&
        writer.append_json_string(unavailable_reason) && writer.append("}");
  }
  return writer.append("available\",\"samples_count\":") &&
      append_samples(writer, samples, count) && writer.append("}");
}

[[nodiscard]] bool write_report(const Options& options, const Percentiles& percentiles,
                                const std::array<std::uint64_t, maximum_intervals>& durations,
                                const NumericMetric& throughput,
                                const std::array<std::uint64_t, maximum_intervals>& throughput_samples,
                                const NumericMetric& cpu_time,
                                const std::array<std::uint64_t, maximum_intervals>& cpu_samples,
                                const NumericMetric& peak_rss,
                                const std::array<std::uint64_t, maximum_intervals>& rss_samples,
                                const TextMetric& cpu, const WorkloadCounters& counters,
                                const std::array<std::uint64_t, maximum_intervals>& allocation_samples,
                                const std::array<std::uint64_t, maximum_intervals>& syscall_samples,
                                std::uint64_t checksum) noexcept {
  JsonWriter writer;
  const std::string_view cpu_text{cpu.text.data(), cpu.size};
  const bool complete =
      writer.append("{\"schema_version\":") &&
      writer.append_json_string(laghu::benchmark::internal::schema_version) &&
      writer.append(",\"build\":{\"build_id\":") &&
      writer.append_json_string(laghu::benchmark::internal::build_id) &&
      writer.append(",\"compiler\":{\"id\":") &&
      writer.append_json_string(laghu::benchmark::internal::compiler_id) &&
      writer.append(",\"version\":") &&
      writer.append_json_string(laghu::benchmark::internal::compiler_version) &&
      writer.append("},\"dependencies\":") &&
      writer.append(laghu::benchmark::internal::dependencies_json) &&
      writer.append(",\"features\":") &&
      writer.append(laghu::benchmark::internal::features_json) &&
      writer.append(",\"profile\":") &&
      writer.append_json_string(laghu::benchmark::internal::build_profile) &&
      writer.append(",\"hardening\":") &&
      writer.append(laghu::benchmark::internal::hardening_json) &&
      writer.append(",\"sanitizer_profile\":") &&
      writer.append_json_string(laghu::benchmark::internal::sanitizer_profile) &&
      writer.append(",\"standard_library\":{\"id\":") &&
      writer.append_json_string(laghu::benchmark::internal::standard_library_id) &&
      writer.append(",\"version\":") &&
      writer.append_json_string(laghu::benchmark::internal::standard_library_version) &&
      writer.append("},\"target\":{\"architecture\":") &&
      writer.append_json_string(laghu::benchmark::internal::target_architecture) &&
      writer.append(",\"os\":") &&
      writer.append_json_string(laghu::benchmark::internal::target_os) &&
      writer.append("}},\"cpu\":{\"description\":{") &&
      writer.append(cpu.available ? "\"status\":\"available\",\"value\":"
                                  : "\"status\":\"unavailable\",\"reason\":") &&
      writer.append_json_string(cpu.available ? cpu_text : std::string_view{"cpu_description_unavailable"}) &&
      writer.append("}},\"metrics\":{\"allocation_count\":");
  if (!complete || !append_counter_metric(writer, counters.allocation_instrumented(),
      counters.allocation_count(), allocation_samples, options.measured_intervals,
      "allocation_counter_not_instrumented") ||
      !writer.append(",\"cpu_time_ns\":") ||
      !append_interval_metric(writer, cpu_time, cpu_samples, options.measured_intervals, "samples_ns") ||
      !writer.append(",\"laghu_syscall_count\":") ||
      !append_counter_metric(writer, counters.laghu_syscall_instrumented(),
      counters.laghu_syscall_count(), syscall_samples, options.measured_intervals,
      "laghu_syscall_counter_not_instrumented") ||
      !writer.append(",\"latency_ns_per_interval\":{\"p50\":") ||
      !writer.append_number(percentiles.p50) ||
      !writer.append(",\"p95\":") || !writer.append_number(percentiles.p95) ||
      !writer.append(",\"p99\":") || !writer.append_number(percentiles.p99) ||
      !writer.append(",\"p99_9\":") || !writer.append_number(percentiles.p999) ||
      !writer.append(",\"samples_ns\":")) {
    return false;
  }
  if (!append_samples(writer, durations, options.measured_intervals) ||
      !writer.append("}")) {
    return false;
  }
  if (!writer.append(",\"peak_rss_bytes\":") ||
      !append_interval_metric(writer, peak_rss, rss_samples, options.measured_intervals,
        "samples_bytes") ||
      !writer.append(",\"throughput_operations_per_second\":") ||
      !append_interval_metric(writer, throughput, throughput_samples, options.measured_intervals,
        "samples_operations_per_second") ||
      !writer.append("},\"parameters\":{\"intervals\":") ||
      !writer.append_number(options.measured_intervals) ||
      !writer.append(",\"operations_per_interval\":") ||
      !writer.append_number(laghu::benchmark::internal::core_foundation_operations_per_interval) ||
      !writer.append(",\"warmup\":") || !writer.append_number(options.warmup_intervals) ||
      !writer.append("},\"workload\":") || !writer.append_json_string(options.workload) ||
      !writer.append(",\"workload_checksum\":") || !writer.append_number(checksum) || !writer.append("}\n")) {
    return false;
  }
  return write_all(STDOUT_FILENO, writer.view());
}

}  // namespace

int main(int argc, char** argv) {
  Options options{};
  if (!parse_options(argc, argv, options)) {
    static_cast<void>(write_all(STDERR_FILENO,
              "usage: laghu_benchmark_core_foundation --workload core-foundation --warmup <0..10000> "
              "--intervals <1..1000>\n"));
    return static_cast<int>(ExitCode::invalid_arguments);
  }

  WorkloadCounters warmup_counters{};
  std::uint64_t checksum{};
  for (std::uint64_t interval = 0U; interval < options.warmup_intervals; ++interval) {
    checksum ^= laghu::benchmark::internal::run_core_foundation(interval + 1U, warmup_counters);
  }

  std::array<std::uint64_t, maximum_intervals> durations{};
  std::array<std::uint64_t, maximum_intervals> throughput_samples{};
  std::array<std::uint64_t, maximum_intervals> cpu_samples{};
  std::array<std::uint64_t, maximum_intervals> rss_samples{};
  std::array<std::uint64_t, maximum_intervals> allocation_samples{};
  std::array<std::uint64_t, maximum_intervals> syscall_samples{};
  WorkloadCounters measured_counters{};
  std::uint64_t total_duration{};
  NumericMetric throughput{true, 0U, {}};
  NumericMetric cpu_time{true, 0U, {}};
  NumericMetric peak_rss{true, 0U, {}};
  for (std::size_t interval = 0U; interval < options.measured_intervals; ++interval) {
    std::uint64_t begin{};
    std::uint64_t end{};
    if (!monotonic_now(begin)) {
      static_cast<void>(write_all(STDERR_FILENO, "laghu-benchmark: monotonic clock is unavailable\n"));
      return static_cast<int>(ExitCode::metric_unavailable);
    }
    const NumericMetric cpu_begin = process_cpu_time();
    const std::uint64_t allocation_begin = measured_counters.allocation_count();
    const std::uint64_t syscall_begin = measured_counters.laghu_syscall_count();
    checksum ^= laghu::benchmark::internal::run_core_foundation(
        static_cast<std::uint64_t>(interval) + options.warmup_intervals + 1U, measured_counters);
    const NumericMetric cpu_end = process_cpu_time();
    if (!monotonic_now(end) || end < begin) {
      static_cast<void>(write_all(STDERR_FILENO, "laghu-benchmark: monotonic clock is invalid\n"));
      return static_cast<int>(ExitCode::metric_unavailable);
    }
    durations[interval] = end - begin;
    if (durations[interval] > std::numeric_limits<std::uint64_t>::max() - total_duration) {
      static_cast<void>(write_all(STDERR_FILENO, "laghu-benchmark: duration overflow\n"));
      return static_cast<int>(ExitCode::metric_unavailable);
    }
    total_duration += durations[interval];
    if (durations[interval] == 0U) {
      throughput = NumericMetric{false, 0U, "interval_zero_elapsed_time"};
    } else if (throughput.available) {
      const std::uint64_t operations =
          laghu::benchmark::internal::core_foundation_operations_per_interval;
      if (operations > std::numeric_limits<std::uint64_t>::max() / nanoseconds_per_second) {
        throughput = NumericMetric{false, 0U, "throughput_overflow"};
      } else {
        throughput_samples[interval] = operations * nanoseconds_per_second / durations[interval];
      }
    }
    if (!cpu_begin.available || !cpu_end.available || cpu_end.value < cpu_begin.value) {
      cpu_time = NumericMetric{false, 0U, "interval_cpu_time_unavailable"};
    } else if (cpu_time.available) {
      const std::uint64_t cpu_delta = cpu_end.value - cpu_begin.value;
      if (cpu_delta > std::numeric_limits<std::uint64_t>::max() - cpu_time.value) {
        cpu_time = NumericMetric{false, 0U, "cpu_time_overflow"};
      } else {
        cpu_samples[interval] = cpu_delta;
        cpu_time.value += cpu_delta;
      }
    }
    const NumericMetric rss = peak_rss_bytes();
    if (!rss.available) {
      peak_rss = rss;
    } else if (peak_rss.available) {
      rss_samples[interval] = rss.value;
      if (rss.value > peak_rss.value) {
        peak_rss.value = rss.value;
      }
    }
    const std::uint64_t allocation_end = measured_counters.allocation_count();
    const std::uint64_t syscall_end = measured_counters.laghu_syscall_count();
    if (allocation_end < allocation_begin || syscall_end < syscall_begin) {
      static_cast<void>(write_all(STDERR_FILENO, "laghu-benchmark: counter is invalid\n"));
      return static_cast<int>(ExitCode::metric_unavailable);
    }
    allocation_samples[interval] = allocation_end - allocation_begin;
    syscall_samples[interval] = syscall_end - syscall_begin;
  }

  Percentiles percentiles{};
  std::array<std::uint64_t, maximum_intervals> percentile_samples = durations;
  if (!laghu::benchmark::internal::summarize_percentiles(
          percentile_samples, options.measured_intervals, percentiles)) {
    static_cast<void>(write_all(STDERR_FILENO, "laghu-benchmark: percentile calculation failed\n"));
    return static_cast<int>(ExitCode::metric_unavailable);
  }
  if (total_duration == 0U) {
    throughput = NumericMetric{false, 0U, "zero_elapsed_time"};
  } else if (throughput.available) {
    const std::uint64_t operations =
        static_cast<std::uint64_t>(options.measured_intervals) *
        laghu::benchmark::internal::core_foundation_operations_per_interval;
    if (operations > std::numeric_limits<std::uint64_t>::max() / nanoseconds_per_second) {
      throughput = NumericMetric{false, 0U, "throughput_overflow"};
    } else {
      throughput.value = operations * nanoseconds_per_second / total_duration;
    }
  }
  if (!write_report(options, percentiles, durations, throughput, throughput_samples, cpu_time, cpu_samples,
                    peak_rss, rss_samples, cpu_description(), measured_counters, allocation_samples,
                    syscall_samples, checksum)) {
    static_cast<void>(write_all(STDERR_FILENO, "laghu-benchmark: report exceeds bounded output capacity\n"));
    return static_cast<int>(ExitCode::output_failure);
  }
  return static_cast<int>(ExitCode::success);
}
