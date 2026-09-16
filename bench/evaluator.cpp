// SPDX-License-Identifier: AGPL-3.0-only
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr std::size_t maximum_document_bytes = 262144U;
constexpr std::size_t maximum_json_nodes = 8192U;
constexpr std::size_t maximum_json_members = 1024U;
constexpr std::size_t maximum_json_elements = 8192U;
constexpr std::size_t maximum_rules = 128U;
constexpr std::size_t minimum_samples = 10U;
constexpr std::size_t maximum_samples = 1000U;
constexpr std::size_t bootstrap_resamples = 10000U;
constexpr std::uint64_t percent_ppm = 10000U;
constexpr std::uint64_t fixed_seed = 0x6c616768755f3832ULL;

enum class MetricId : std::uint8_t {
  latency,
  throughput,
  cpu_time,
  peak_rss,
  allocation,
  laghu_syscall,
  count,
  invalid,
};

constexpr std::size_t metric_count = static_cast<std::size_t>(MetricId::count);

enum class ExitCode : int {
  success = 0,
  hard_regression = 1,
  invalid_arguments = 64,
  invalid_input = 65,
  output_failure = 74,
};

enum class JsonType : std::uint8_t {
  null_value,
  boolean,
  number,
  string,
  array,
  object,
};

struct JsonNode final {
  JsonType type{JsonType::null_value};
  std::string_view string{};
  std::string_view raw{};
  std::uint64_t number{};
  bool boolean{};
};

struct JsonMember final {
  std::size_t parent{};
  std::string_view key{};
  std::size_t value{};
};

struct JsonElement final {
  std::size_t parent{};
  std::size_t value{};
};

[[nodiscard]] constexpr bool is_space(char character) noexcept {
  return character == ' ' || character == '\n' || character == '\r' || character == '\t';
}

[[nodiscard]] constexpr bool is_digit(char character) noexcept {
  return character >= '0' && character <= '9';
}

[[nodiscard]] constexpr bool is_hex(char character) noexcept {
  return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
      (character >= 'A' && character <= 'F');
}

[[nodiscard]] bool write_all(int descriptor, std::string_view value) noexcept {
  while (!value.empty()) {
    const ssize_t written = ::write(descriptor, value.data(), value.size());
    if (written > 0) {
      value.remove_prefix(static_cast<std::size_t>(written));
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

template <std::size_t Capacity>
[[nodiscard]] bool read_file(const char* path, std::array<char, Capacity>& output,
                             std::size_t& size) noexcept {
  size = 0U;
  const int descriptor = ::open(path, O_RDONLY);
  if (descriptor < 0) {
    return false;
  }
  bool complete = true;
  while (size < output.size()) {
    const ssize_t read_count = ::read(descriptor, output.data() + size, output.size() - size);
    if (read_count > 0) {
      size += static_cast<std::size_t>(read_count);
      continue;
    }
    if (read_count == 0) {
      break;
    }
    if (errno != EINTR) {
      complete = false;
      break;
    }
  }
  if (size == output.size()) {
    char extra{};
    const ssize_t read_count = ::read(descriptor, &extra, 1U);
    if (read_count != 0) {
      complete = false;
    }
  }
  static_cast<void>(::close(descriptor));
  return complete;
}

class JsonDocument final {
 public:
  [[nodiscard]] bool load(const char* path) noexcept {
    if (!read_file(path, storage_, size_) || size_ == 0U) {
      return false;
    }
    cursor_ = 0U;
    node_count_ = 0U;
    member_count_ = 0U;
    element_count_ = 0U;
    if (!parse_value(root_, 0U)) {
      return false;
    }
    skip_space();
    return cursor_ == size_;
  }

  [[nodiscard]] const JsonNode* root() const noexcept { return node(root_); }

  [[nodiscard]] const JsonNode* field(const JsonNode* object, std::string_view key) const noexcept {
    if (object == nullptr || object->type != JsonType::object) {
      return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(object - nodes_.data());
    for (std::size_t member = 0U; member < member_count_; ++member) {
      if (members_[member].parent == index && members_[member].key == key) {
        return node(members_[member].value);
      }
    }
    return nullptr;
  }

  [[nodiscard]] const JsonNode* element(const JsonNode* array, std::size_t ordinal) const noexcept {
    if (array == nullptr || array->type != JsonType::array) {
      return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(array - nodes_.data());
    std::size_t found = 0U;
    for (std::size_t element = 0U; element < element_count_; ++element) {
      if (elements_[element].parent == index) {
        if (found == ordinal) {
          return node(elements_[element].value);
        }
        ++found;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::size_t element_count(const JsonNode* array) const noexcept {
    if (array == nullptr || array->type != JsonType::array) {
      return 0U;
    }
    const std::size_t index = static_cast<std::size_t>(array - nodes_.data());
    std::size_t count = 0U;
    for (std::size_t element = 0U; element < element_count_; ++element) {
      if (elements_[element].parent == index) {
        ++count;
      }
    }
    return count;
  }

 private:
  [[nodiscard]] const JsonNode* node(std::size_t index) const noexcept {
    return index < node_count_ ? &nodes_[index] : nullptr;
  }

  void skip_space() noexcept {
    while (cursor_ < size_ && is_space(storage_[cursor_])) {
      ++cursor_;
    }
  }

  [[nodiscard]] bool parse_string(std::string_view& output) noexcept {
    if (cursor_ >= size_ || storage_[cursor_] != '"') {
      return false;
    }
    const std::size_t start = ++cursor_;
    while (cursor_ < size_) {
      const unsigned char character = static_cast<unsigned char>(storage_[cursor_]);
      if (character == '"') {
        output = {storage_.data() + start, cursor_ - start};
        ++cursor_;
        return true;
      }
      if (character < 0x20U) {
        return false;
      }
      if (character == '\\') {
        ++cursor_;
        if (cursor_ >= size_) {
          return false;
        }
        const char escape = storage_[cursor_];
        if (escape == 'u') {
          if (cursor_ + 4U >= size_) {
            return false;
          }
          for (std::size_t digit = 1U; digit <= 4U; ++digit) {
            if (!is_hex(storage_[cursor_ + digit])) {
              return false;
            }
          }
          cursor_ += 5U;
          continue;
        }
        if (escape != '"' && escape != '\\' && escape != '/' && escape != 'b' &&
            escape != 'f' && escape != 'n' && escape != 'r' && escape != 't') {
          return false;
        }
      }
      ++cursor_;
    }
    return false;
  }

  [[nodiscard]] bool parse_number(std::uint64_t& output) noexcept {
    const std::size_t start = cursor_;
    if (cursor_ >= size_ || !is_digit(storage_[cursor_])) {
      return false;
    }
    if (storage_[cursor_] == '0') {
      ++cursor_;
      if (cursor_ < size_ && is_digit(storage_[cursor_])) {
        return false;
      }
    } else {
      while (cursor_ < size_ && is_digit(storage_[cursor_])) {
        ++cursor_;
      }
    }
    const auto converted = std::from_chars(storage_.data() + start, storage_.data() + cursor_, output);
    return converted.ec == std::errc{} && converted.ptr == storage_.data() + cursor_;
  }

  [[nodiscard]] bool append_node(JsonType type, std::size_t& output) noexcept {
    if (node_count_ == nodes_.size()) {
      return false;
    }
    output = node_count_;
    nodes_[node_count_++] = JsonNode{type, {}, {}, 0U, false};
    return true;
  }

  [[nodiscard]] bool parse_object(std::size_t parent, std::size_t depth) noexcept {
    ++cursor_;
    skip_space();
    if (cursor_ < size_ && storage_[cursor_] == '}') {
      ++cursor_;
      return true;
    }
    while (cursor_ < size_) {
      std::string_view key;
      if (!parse_string(key)) {
        return false;
      }
      for (std::size_t member = 0U; member < member_count_; ++member) {
        if (members_[member].parent == parent && members_[member].key == key) {
          return false;
        }
      }
      skip_space();
      if (cursor_ >= size_ || storage_[cursor_] != ':') {
        return false;
      }
      ++cursor_;
      skip_space();
      std::size_t value{};
      if (!parse_value(value, depth + 1U) || member_count_ == members_.size()) {
        return false;
      }
      members_[member_count_++] = JsonMember{parent, key, value};
      skip_space();
      if (cursor_ < size_ && storage_[cursor_] == '}') {
        ++cursor_;
        return true;
      }
      if (cursor_ >= size_ || storage_[cursor_] != ',') {
        return false;
      }
      ++cursor_;
      skip_space();
    }
    return false;
  }

  [[nodiscard]] bool parse_array(std::size_t parent, std::size_t depth) noexcept {
    ++cursor_;
    skip_space();
    if (cursor_ < size_ && storage_[cursor_] == ']') {
      ++cursor_;
      return true;
    }
    while (cursor_ < size_) {
      std::size_t value{};
      if (!parse_value(value, depth + 1U) || element_count_ == elements_.size()) {
        return false;
      }
      elements_[element_count_++] = JsonElement{parent, value};
      skip_space();
      if (cursor_ < size_ && storage_[cursor_] == ']') {
        ++cursor_;
        return true;
      }
      if (cursor_ >= size_ || storage_[cursor_] != ',') {
        return false;
      }
      ++cursor_;
      skip_space();
    }
    return false;
  }

  [[nodiscard]] bool parse_value(std::size_t& output, std::size_t depth) noexcept {
    if (depth > 64U) {
      return false;
    }
    skip_space();
    if (cursor_ >= size_) {
      return false;
    }
    const std::size_t start = cursor_;
    const char character = storage_[cursor_];
    if (character == '"') {
      if (!append_node(JsonType::string, output) || !parse_string(nodes_[output].string)) {
        return false;
      }
    } else if (character == '{') {
      if (!append_node(JsonType::object, output) || !parse_object(output, depth)) {
        return false;
      }
    } else if (character == '[') {
      if (!append_node(JsonType::array, output) || !parse_array(output, depth)) {
        return false;
      }
    } else if (character == 't' && cursor_ + 4U <= size_ &&
               std::memcmp(storage_.data() + cursor_, "true", 4U) == 0) {
      if (!append_node(JsonType::boolean, output)) {
        return false;
      }
      nodes_[output].boolean = true;
      cursor_ += 4U;
    } else if (character == 'f' && cursor_ + 5U <= size_ &&
               std::memcmp(storage_.data() + cursor_, "false", 5U) == 0) {
      if (!append_node(JsonType::boolean, output)) {
        return false;
      }
      cursor_ += 5U;
    } else if (character == 'n' && cursor_ + 4U <= size_ &&
               std::memcmp(storage_.data() + cursor_, "null", 4U) == 0) {
      if (!append_node(JsonType::null_value, output)) {
        return false;
      }
      cursor_ += 4U;
    } else {
      if (!append_node(JsonType::number, output) || !parse_number(nodes_[output].number)) {
        return false;
      }
    }
    nodes_[output].raw = {storage_.data() + start, cursor_ - start};
    return true;
  }

  std::array<char, maximum_document_bytes> storage_{};
  std::array<JsonNode, maximum_json_nodes> nodes_{};
  std::array<JsonMember, maximum_json_members> members_{};
  std::array<JsonElement, maximum_json_elements> elements_{};
  std::size_t size_{};
  std::size_t cursor_{};
  std::size_t root_{};
  std::size_t node_count_{};
  std::size_t member_count_{};
  std::size_t element_count_{};
};

[[nodiscard]] const JsonNode* object_field(const JsonDocument& document, const JsonNode* object,
                                            std::string_view key) noexcept {
  return document.field(object, key);
}

[[nodiscard]] bool required_object(const JsonDocument& document, const JsonNode* object,
                                   std::string_view key, const JsonNode*& output) noexcept {
  output = object_field(document, object, key);
  return output != nullptr && output->type == JsonType::object;
}

[[nodiscard]] bool required_array(const JsonDocument& document, const JsonNode* object,
                                  std::string_view key, const JsonNode*& output) noexcept {
  output = object_field(document, object, key);
  return output != nullptr && output->type == JsonType::array;
}

[[nodiscard]] bool required_string(const JsonDocument& document, const JsonNode* object,
                                   std::string_view key, std::string_view& output) noexcept {
  const JsonNode* value = object_field(document, object, key);
  if (value == nullptr || value->type != JsonType::string || value->string.empty()) {
    return false;
  }
  output = value->string;
  return true;
}

[[nodiscard]] bool required_number(const JsonDocument& document, const JsonNode* object,
                                   std::string_view key, std::uint64_t& output) noexcept {
  const JsonNode* value = object_field(document, object, key);
  if (value == nullptr || value->type != JsonType::number) {
    return false;
  }
  output = value->number;
  return true;
}

[[nodiscard]] bool required_boolean(const JsonDocument& document, const JsonNode* object,
                                    std::string_view key, bool& output) noexcept {
  const JsonNode* value = object_field(document, object, key);
  if (value == nullptr || value->type != JsonType::boolean) {
    return false;
  }
  output = value->boolean;
  return true;
}

struct MetricSeries final {
  std::array<std::uint64_t, maximum_samples> values{};
  std::size_t count{};
  bool available{};
  std::string_view reason{};
};

struct Artifact final {
  JsonDocument document{};
  std::string_view build_id{};
  std::string_view compiler_id{};
  std::string_view compiler_version{};
  std::string_view standard_library_id{};
  std::string_view standard_library_version{};
  std::string_view profile{};
  std::string_view hardening{};
  std::string_view sanitizer_profile{};
  std::string_view target_architecture{};
  std::string_view target_os{};
  std::string_view cpu_description{};
  bool cpu_description_available{};
  std::string_view dependencies{};
  std::string_view features{};
  std::string_view parameters{};
  std::string_view workload{};
  std::uint64_t workload_checksum{};
  std::array<MetricSeries, metric_count> metrics{};
};

[[nodiscard]] constexpr std::size_t metric_index(MetricId metric) noexcept {
  return static_cast<std::size_t>(metric);
}

[[nodiscard]] MetricId metric_id(std::string_view name) noexcept {
  if (name == "latency_ns_per_interval") {
    return MetricId::latency;
  }
  if (name == "throughput_operations_per_second") {
    return MetricId::throughput;
  }
  if (name == "cpu_time_ns") {
    return MetricId::cpu_time;
  }
  if (name == "peak_rss_bytes") {
    return MetricId::peak_rss;
  }
  if (name == "allocation_count") {
    return MetricId::allocation;
  }
  if (name == "laghu_syscall_count") {
    return MetricId::laghu_syscall;
  }
  return MetricId::invalid;
}

[[nodiscard]] bool load_samples(const JsonDocument& document, const JsonNode* metric,
                                std::string_view key, std::uint64_t intervals,
                                MetricSeries& output) noexcept {
  const JsonNode* samples{};
  if (!required_array(document, metric, key, samples)) {
    return false;
  }
  output.count = document.element_count(samples);
  if (output.count < minimum_samples || output.count > output.values.size() ||
      output.count != intervals) {
    return false;
  }
  for (std::size_t index = 0U; index < output.count; ++index) {
    const JsonNode* sample = document.element(samples, index);
    if (sample == nullptr || sample->type != JsonType::number) {
      return false;
    }
    output.values[index] = sample->number;
  }
  output.available = true;
  return true;
}

[[nodiscard]] bool load_host_metric(const JsonDocument& document, const JsonNode* metrics,
                                    std::string_view key, std::string_view samples_key,
                                    std::uint64_t intervals, MetricSeries& output) noexcept {
  const JsonNode* metric{};
  std::string_view status;
  if (!required_object(document, metrics, key, metric) ||
      !required_string(document, metric, "status", status)) {
    return false;
  }
  if (status == "unavailable") {
    return required_string(document, metric, "reason", output.reason);
  }
  std::uint64_t value{};
  return status == "available" && required_number(document, metric, "value", value) &&
      load_samples(document, metric, samples_key, intervals, output);
}

[[nodiscard]] bool load_counter_metric(const JsonDocument& document, const JsonNode* metrics,
                                       std::string_view key, std::uint64_t intervals,
                                       MetricSeries& output) noexcept {
  const JsonNode* metric{};
  bool instrumented{};
  std::uint64_t value{};
  std::string_view status;
  if (!required_object(document, metrics, key, metric) ||
      !required_boolean(document, metric, "instrumented", instrumented) ||
      !required_number(document, metric, "value", value) ||
      !required_string(document, metric, "status", status)) {
    return false;
  }
  if (!instrumented) {
    return status == "unavailable" && required_string(document, metric, "reason", output.reason);
  }
  return status == "available" &&
      load_samples(document, metric, "samples_count", intervals, output);
}

[[nodiscard]] bool load_latency_metric(const JsonDocument& document, const JsonNode* metrics,
                                       std::uint64_t intervals, MetricSeries& output) noexcept {
  const JsonNode* metric{};
  std::uint64_t ignored{};
  return required_object(document, metrics, "latency_ns_per_interval", metric) &&
      required_number(document, metric, "p50", ignored) &&
      required_number(document, metric, "p95", ignored) &&
      required_number(document, metric, "p99", ignored) &&
      required_number(document, metric, "p99_9", ignored) &&
      load_samples(document, metric, "samples_ns", intervals, output);
}

[[nodiscard]] bool load_artifact(const char* path, Artifact& artifact,
                                 std::string_view& error) noexcept {
  if (!artifact.document.load(path)) {
    error = "document";
    return false;
  }
  const JsonNode* root = artifact.document.root();
  if (root == nullptr || root->type != JsonType::object) {
    error = "root";
    return false;
  }
  std::string_view schema;
  if (!required_string(artifact.document, root, "schema_version", schema) ||
      schema != "laghu-benchmark-v1") {
    error = "schema_version";
    return false;
  }
  const JsonNode* build{};
  const JsonNode* compiler{};
  const JsonNode* standard_library{};
  const JsonNode* target{};
  const JsonNode* hardening{};
  const JsonNode* dependencies{};
  const JsonNode* features{};
  if (!required_object(artifact.document, root, "build", build) ||
      !required_string(artifact.document, build, "build_id", artifact.build_id) ||
      !required_object(artifact.document, build, "compiler", compiler) ||
      !required_string(artifact.document, compiler, "id", artifact.compiler_id) ||
      !required_string(artifact.document, compiler, "version", artifact.compiler_version) ||
      !required_object(artifact.document, build, "standard_library", standard_library) ||
      !required_string(artifact.document, standard_library, "id", artifact.standard_library_id) ||
      !required_string(artifact.document, standard_library, "version", artifact.standard_library_version) ||
      !required_string(artifact.document, build, "profile", artifact.profile) ||
      !required_object(artifact.document, build, "hardening", hardening) ||
      !required_string(artifact.document, build, "sanitizer_profile", artifact.sanitizer_profile) ||
      !required_object(artifact.document, build, "target", target) ||
      !required_string(artifact.document, target, "architecture", artifact.target_architecture) ||
      !required_string(artifact.document, target, "os", artifact.target_os) ||
      !required_array(artifact.document, build, "dependencies", dependencies) ||
      !required_array(artifact.document, build, "features", features)) {
    error = "build";
    return false;
  }
  artifact.dependencies = dependencies->raw;
  artifact.features = features->raw;
  artifact.hardening = hardening->raw;

  const JsonNode* cpu{};
  const JsonNode* description{};
  std::string_view cpu_status;
  if (!required_object(artifact.document, root, "cpu", cpu) ||
      !required_object(artifact.document, cpu, "description", description) ||
      !required_string(artifact.document, description, "status", cpu_status)) {
    error = "cpu_description";
    return false;
  }
  if (cpu_status == "available") {
    if (!required_string(artifact.document, description, "value", artifact.cpu_description)) {
      error = "cpu_description";
      return false;
    }
    artifact.cpu_description_available = true;
  } else if (cpu_status == "unavailable") {
    std::string_view reason;
    if (!required_string(artifact.document, description, "reason", reason)) {
      error = "cpu_description";
      return false;
    }
  } else {
    error = "cpu_description";
    return false;
  }

  const JsonNode* parameters{};
  std::uint64_t intervals{};
  std::uint64_t operations{};
  std::uint64_t warmup{};
  if (!required_object(artifact.document, root, "parameters", parameters) ||
      !required_number(artifact.document, parameters, "intervals", intervals) || intervals < minimum_samples ||
      intervals > maximum_samples ||
      !required_number(artifact.document, parameters, "operations_per_interval", operations) ||
      !required_number(artifact.document, parameters, "warmup", warmup) ||
      !required_string(artifact.document, root, "workload", artifact.workload) ||
      !required_number(artifact.document, root, "workload_checksum", artifact.workload_checksum)) {
    error = "parameters";
    return false;
  }
  artifact.parameters = parameters->raw;

  const JsonNode* metrics{};
  if (!required_object(artifact.document, root, "metrics", metrics) ||
      !load_counter_metric(artifact.document, metrics, "allocation_count", intervals,
        artifact.metrics[metric_index(MetricId::allocation)]) ||
      !load_host_metric(artifact.document, metrics, "cpu_time_ns", "samples_ns", intervals,
        artifact.metrics[metric_index(MetricId::cpu_time)]) ||
      !load_counter_metric(artifact.document, metrics, "laghu_syscall_count", intervals,
        artifact.metrics[metric_index(MetricId::laghu_syscall)]) ||
      !load_latency_metric(artifact.document, metrics, intervals,
        artifact.metrics[metric_index(MetricId::latency)]) ||
      !load_host_metric(artifact.document, metrics, "peak_rss_bytes", "samples_bytes", intervals,
        artifact.metrics[metric_index(MetricId::peak_rss)]) ||
      !load_host_metric(artifact.document, metrics, "throughput_operations_per_second",
        "samples_operations_per_second", intervals,
        artifact.metrics[metric_index(MetricId::throughput)])) {
    error = "metrics";
    return false;
  }
  return true;
}

[[nodiscard]] bool equal_configuration(const Artifact& baseline, const Artifact& candidate,
                                       std::string_view& error) noexcept {
  if (baseline.target_architecture != candidate.target_architecture ||
      baseline.target_os != candidate.target_os) {
    error = "target";
    return false;
  }
  if (baseline.compiler_id != candidate.compiler_id ||
      baseline.compiler_version != candidate.compiler_version ||
      baseline.standard_library_id != candidate.standard_library_id ||
      baseline.standard_library_version != candidate.standard_library_version ||
      baseline.profile != candidate.profile || baseline.hardening != candidate.hardening ||
      baseline.sanitizer_profile != candidate.sanitizer_profile ||
      baseline.features != candidate.features || baseline.dependencies != candidate.dependencies) {
    error = "build";
    return false;
  }
  if (!baseline.cpu_description_available || !candidate.cpu_description_available) {
    error = "hardware_identity_unavailable";
    return false;
  }
  if (baseline.cpu_description != candidate.cpu_description) {
    error = "hardware";
    return false;
  }
  if (baseline.workload != candidate.workload ||
      baseline.workload_checksum != candidate.workload_checksum ||
      baseline.parameters != candidate.parameters) {
    error = "workload";
    return false;
  }
  return true;
}

[[nodiscard]] std::uint64_t hash_append(std::uint64_t value, std::string_view input) noexcept {
  for (const char character : input) {
    value ^= static_cast<unsigned char>(character);
    value *= 1099511628211ULL;
  }
  value ^= 0xffU;
  return value * 1099511628211ULL;
}

[[nodiscard]] std::uint64_t environment_hash(const Artifact& artifact) noexcept {
  std::uint64_t value = 1469598103934665603ULL;
  for (const std::string_view component : {
           artifact.target_os, artifact.target_architecture, artifact.cpu_description,
           artifact.compiler_id, artifact.compiler_version, artifact.standard_library_id,
           artifact.standard_library_version, artifact.profile, artifact.hardening,
           artifact.sanitizer_profile,
           artifact.features, artifact.dependencies}) {
    value = hash_append(value, component);
  }
  return value;
}

[[nodiscard]] bool hex_environment(std::uint64_t value, std::array<char, 17U>& output) noexcept {
  constexpr std::array<char, 16U> digits{
      '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  for (std::size_t index = 0U; index < 16U; ++index) {
    const std::size_t shift = (15U - index) * 4U;
    output[index] = digits[(value >> shift) & 0x0fU];
  }
  output[16U] = '\0';
  return true;
}

struct Rule final {
  std::string_view workload{};
  std::string_view metric{};
  bool higher_is_better{};
  std::uint64_t allowed_regression_ppm{};
  std::uint64_t noise_band_ppm{};
  std::string_view reference_environment{};
  bool hard{};
};

struct Manifest final {
  std::array<char, maximum_document_bytes> storage{};
  std::array<Rule, maximum_rules> rules{};
  std::size_t count{};
};

[[nodiscard]] bool parse_percent_ppm(std::string_view input, std::uint64_t& output) noexcept {
  if (input.empty()) {
    return false;
  }
  std::size_t cursor = 0U;
  std::uint64_t whole{};
  while (cursor < input.size() && is_digit(input[cursor])) {
    if (whole > (std::numeric_limits<std::uint64_t>::max() - 9U) / 10U) {
      return false;
    }
    whole = whole * 10U + static_cast<std::uint64_t>(input[cursor] - '0');
    ++cursor;
  }
  if (cursor == 0U) {
    return false;
  }
  std::uint64_t fraction{};
  std::size_t fraction_digits{};
  if (cursor < input.size()) {
    if (input[cursor] != '.') {
      return false;
    }
    ++cursor;
    while (cursor < input.size() && is_digit(input[cursor]) && fraction_digits < 4U) {
      fraction = fraction * 10U + static_cast<std::uint64_t>(input[cursor] - '0');
      ++fraction_digits;
      ++cursor;
    }
    if (fraction_digits == 0U || cursor != input.size()) {
      return false;
    }
  }
  while (fraction_digits < 4U) {
    fraction *= 10U;
    ++fraction_digits;
  }
  if (whole > (std::numeric_limits<std::uint64_t>::max() - fraction) / percent_ppm) {
    return false;
  }
  output = whole * percent_ppm + fraction;
  return true;
}

[[nodiscard]] int compare_rule_key(const Rule& left, const Rule& right) noexcept {
  if (left.workload < right.workload) {
    return -1;
  }
  if (left.workload > right.workload) {
    return 1;
  }
  if (left.metric < right.metric) {
    return -1;
  }
  if (left.metric > right.metric) {
    return 1;
  }
  return 0;
}

[[nodiscard]] bool parse_manifest(const char* path, Manifest& manifest,
                                  std::string_view& error) noexcept {
  std::size_t size{};
  if (!read_file(path, manifest.storage, size)) {
    error = "document";
    return false;
  }
  std::size_t begin{};
  while (begin < size) {
    std::size_t end = begin;
    while (end < size && manifest.storage[end] != '\n') {
      ++end;
    }
    std::size_t line_end = end;
    if (line_end > begin && manifest.storage[line_end - 1U] == '\r') {
      --line_end;
    }
    std::string_view line{manifest.storage.data() + begin, line_end - begin};
    if (!line.empty() && line.front() != '#') {
      if (manifest.count == manifest.rules.size()) {
        error = "too_many_rules";
        return false;
      }
      std::array<std::string_view, 7U> fields{};
      std::size_t field{};
      std::size_t field_begin{};
      for (std::size_t index = 0U; index <= line.size(); ++index) {
        if (index == line.size() || line[index] == '\t') {
          if (field >= fields.size()) {
            error = "field_count";
            return false;
          }
          fields[field++] = line.substr(field_begin, index - field_begin);
          field_begin = index + 1U;
        }
      }
      if (field != fields.size()) {
        error = "field_count";
        return false;
      }
      Rule rule{};
      rule.workload = fields[0U];
      rule.metric = fields[1U];
      if (rule.workload.empty() || rule.metric.empty() ||
          !parse_percent_ppm(fields[3U], rule.allowed_regression_ppm) ||
          !parse_percent_ppm(fields[4U], rule.noise_band_ppm)) {
        error = "field_value";
        return false;
      }
      if (metric_id(rule.metric) == MetricId::invalid) {
        error = "metric";
        return false;
      }
      if (fields[2U] == "lower") {
        rule.higher_is_better = false;
      } else if (fields[2U] == "higher") {
        rule.higher_is_better = true;
      } else {
        error = "direction";
        return false;
      }
      rule.reference_environment = fields[5U];
      if (rule.reference_environment.size() != 16U) {
        error = "reference_environment";
        return false;
      }
      for (const char character : rule.reference_environment) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
          error = "reference_environment";
          return false;
        }
      }
      if (fields[6U] == "hard") {
        rule.hard = true;
      } else if (fields[6U] == "advisory") {
        rule.hard = false;
      } else {
        error = "mode";
        return false;
      }
      if (manifest.count != 0U && compare_rule_key(manifest.rules[manifest.count - 1U], rule) >= 0) {
        error = "ordering_or_duplicate";
        return false;
      }
      manifest.rules[manifest.count++] = rule;
    }
    begin = end < size ? end + 1U : size;
  }
  return true;
}

[[nodiscard]] std::uint64_t next_random(std::uint64_t& state) noexcept {
  state ^= state >> 12U;
  state ^= state << 25U;
  state ^= state >> 27U;
  return state * 2685821657736338717ULL;
}

[[nodiscard]] bool sample_mean(const std::array<std::uint64_t, maximum_samples>& samples,
                               std::size_t count, std::uint64_t& state,
                               std::uint64_t& output) noexcept {
  if (count == 0U || count > samples.size()) {
    return false;
  }
  std::uint64_t total{};
  for (std::size_t sample = 0U; sample < count; ++sample) {
    const auto sample_index = next_random(state) % count;
    const std::uint64_t value = samples[sample_index];
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
      return false;
    }
    total += value;
  }
  output = total / count;
  return true;
}

[[nodiscard]] bool sample_max(const std::array<std::uint64_t, maximum_samples>& samples,
                              std::size_t count, std::uint64_t& state,
                              std::uint64_t& output) noexcept {
  if (count == 0U || count > samples.size()) {
    return false;
  }
  output = 0U;
  for (std::size_t sample = 0U; sample < count; ++sample) {
    const auto sample_index = next_random(state) % count;
    const std::uint64_t value = samples[sample_index];
    if (value > output) {
      output = value;
    }
  }
  return true;
}

enum class RegressionResult : std::uint8_t {
  success,
  arithmetic_overflow,
};

constexpr std::int64_t unbounded_regression_ppm = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t unbounded_improvement_ppm = std::numeric_limits<std::int64_t>::min();

[[nodiscard]] RegressionResult regression_ppm(std::uint64_t baseline, std::uint64_t candidate,
                                              bool higher_is_better,
                                              std::int64_t& output) noexcept {
  if (baseline == 0U) {
    if (candidate == 0U) {
      output = 0;
      return RegressionResult::success;
    }
    output = higher_is_better ? unbounded_improvement_ppm : unbounded_regression_ppm;
    return RegressionResult::success;
  }
  const bool candidate_is_regression = higher_is_better ? candidate < baseline : candidate > baseline;
  const std::uint64_t difference = candidate >= baseline ? candidate - baseline : baseline - candidate;
  const std::uint64_t whole = difference / baseline;
  const std::uint64_t fraction_source = difference % baseline;
  std::uint64_t remainder{};
  std::uint64_t fractional{};
  for (std::uint64_t bit = 1U << 19U; bit != 0U; bit >>= 1U) {
    std::uint64_t carry{};
    if (remainder >= baseline - remainder) {
      remainder -= baseline - remainder;
      carry = 1U;
    } else {
      remainder += remainder;
    }
    if ((bit & 1000000U) != 0U) {
      if (remainder >= baseline - fraction_source) {
        remainder -= baseline - fraction_source;
        ++carry;
      } else {
        remainder += fraction_source;
      }
    }
    if (fractional > (std::numeric_limits<std::uint64_t>::max() - carry) / 2U) {
      return RegressionResult::arithmetic_overflow;
    }
    fractional = fractional * 2U + carry;
  }
  if (whole > (static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
               fractional) / 1000000U) {
    return RegressionResult::arithmetic_overflow;
  }
  const std::uint64_t magnitude = whole * 1000000U + fractional;
  output = candidate_is_regression ? static_cast<std::int64_t>(magnitude)
                                   : -static_cast<std::int64_t>(magnitude);
  return RegressionResult::success;
}

struct Interval final {
  std::int64_t lower{};
  std::int64_t upper{};
};

enum class BootstrapResult : std::uint8_t {
  success,
  invalid_samples,
  zero_baseline_undefined,
  arithmetic_overflow,
};

[[nodiscard]] bool uses_maximum(MetricId metric) noexcept {
  return metric == MetricId::peak_rss;
}

[[nodiscard]] bool has_nonzero_sample(const MetricSeries& series) noexcept {
  for (std::size_t index = 0U; index < series.count; ++index) {
    if (series.values[index] != 0U) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] BootstrapResult bootstrap_interval(const MetricSeries& baseline,
                                                 const MetricSeries& candidate,
                                                 MetricId metric, bool higher_is_better,
                                                 Interval& output) noexcept {
  if (!baseline.available || !candidate.available) {
    return BootstrapResult::invalid_samples;
  }
  if (!has_nonzero_sample(baseline) && has_nonzero_sample(candidate)) {
    return BootstrapResult::zero_baseline_undefined;
  }
  std::array<std::int64_t, bootstrap_resamples> samples{};
  std::uint64_t state = fixed_seed;
  for (std::size_t iteration = 0U; iteration < samples.size(); ++iteration) {
    std::uint64_t baseline_mean{};
    std::uint64_t candidate_mean{};
    const bool baseline_ok = uses_maximum(metric)
        ? sample_max(baseline.values, baseline.count, state, baseline_mean)
        : sample_mean(baseline.values, baseline.count, state, baseline_mean);
    const bool candidate_ok = uses_maximum(metric)
        ? sample_max(candidate.values, candidate.count, state, candidate_mean)
        : sample_mean(candidate.values, candidate.count, state, candidate_mean);
    if (!baseline_ok || !candidate_ok) {
      return BootstrapResult::invalid_samples;
    }
    const RegressionResult regression =
        regression_ppm(baseline_mean, candidate_mean, higher_is_better, samples[iteration]);
    if (regression == RegressionResult::arithmetic_overflow) {
      return BootstrapResult::arithmetic_overflow;
    }
  }
  std::sort(samples.begin(), samples.end());
  constexpr std::size_t lower_index = (bootstrap_resamples * 25U + 999U) / 1000U - 1U;
  constexpr std::size_t upper_index = (bootstrap_resamples * 975U + 999U) / 1000U - 1U;
  output = Interval{samples[lower_index], samples[upper_index]};
  return BootstrapResult::success;
}

class JsonWriter final {
 public:
  [[nodiscard]] bool append(std::string_view value) noexcept {
    if (value.size() > output_.size() - size_) {
      return false;
    }
    std::memcpy(output_.data() + size_, value.data(), value.size());
    size_ += value.size();
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

  [[nodiscard]] bool append_signed_number(std::int64_t value) noexcept {
    const auto converted = std::to_chars(output_.data() + size_, output_.data() + output_.size(), value);
    if (converted.ec != std::errc{}) {
      return false;
    }
    size_ = static_cast<std::size_t>(converted.ptr - output_.data());
    return true;
  }

  [[nodiscard]] bool append_string(std::string_view value) noexcept {
    if (!append("\"")) {
      return false;
    }
    for (const char character : value) {
      if (character == '\\' && !append("\\\\")) {
        return false;
      }
      if (character == '"' && !append("\\\"")) {
        return false;
      }
      if (character != '\\' && character != '"' &&
          (static_cast<unsigned char>(character) < 0x20U || !append({&character, 1U}))) {
        return false;
      }
    }
    return append("\"");
  }

  [[nodiscard]] std::string_view view() const noexcept { return {output_.data(), size_}; }

 private:
  std::array<char, maximum_document_bytes> output_{};
  std::size_t size_{};
};

struct Options final {
  const char* baseline{};
  const char* candidate{};
  const char* manifest{};
  const char* environment{};
};

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) noexcept {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (index + 1 >= argc) {
      return false;
    }
    const char* value = argv[++index];
    if (argument == "--baseline" && options.baseline == nullptr) {
      options.baseline = value;
    } else if (argument == "--candidate" && options.candidate == nullptr) {
      options.candidate = value;
    } else if (argument == "--manifest" && options.manifest == nullptr) {
      options.manifest = value;
    } else if (argument == "--environment" && options.environment == nullptr) {
      options.environment = value;
    } else {
      return false;
    }
  }
  if (options.environment != nullptr) {
    return options.baseline == nullptr && options.candidate == nullptr && options.manifest == nullptr;
  }
  return options.baseline != nullptr && options.candidate != nullptr && options.manifest != nullptr;
}

[[nodiscard]] bool write_environment(const Artifact& artifact) noexcept {
  if (!artifact.cpu_description_available) {
    return false;
  }
  std::array<char, 17U> environment{};
  static_cast<void>(hex_environment(environment_hash(artifact), environment));
  return write_all(STDOUT_FILENO, {environment.data(), 16U}) && write_all(STDOUT_FILENO, "\n");
}

[[nodiscard]] bool write_result(const Artifact& baseline, const Manifest& manifest,
                                const std::array<Interval, maximum_rules>& intervals,
                                const std::array<bool, maximum_rules>& regressions,
                                const std::array<bool, maximum_rules>& applicable,
                                bool any_hard_failure) noexcept {
  std::array<char, 17U> environment{};
  static_cast<void>(hex_environment(environment_hash(baseline), environment));
  JsonWriter writer;
  if (!writer.append("{\"reference_environment\":") ||
      !writer.append_string({environment.data(), 16U}) ||
      !writer.append(",\"unbounded_regression_ppm_sentinels\":{\"improvement\":") ||
      !writer.append_signed_number(unbounded_improvement_ppm) ||
      !writer.append(",\"regression\":") ||
      !writer.append_signed_number(unbounded_regression_ppm) ||
      !writer.append("}") ||
      !writer.append(",\"results\":[")) {
    return false;
  }
  bool first = true;
  for (std::size_t index = 0U; index < manifest.count; ++index) {
    if (!applicable[index]) {
      continue;
    }
    const Rule& rule = manifest.rules[index];
    const std::uint64_t threshold = rule.allowed_regression_ppm + rule.noise_band_ppm;
    const bool hard_failure = rule.hard && regressions[index];
    const std::string_view status = hard_failure ? "hard_regression"
                                    : regressions[index] ? "advisory_regression"
                                                         : "pass";
    if ((!first && !writer.append(",")) || !writer.append("{\"allowed_regression_ppm\":") ||
        !writer.append_number(rule.allowed_regression_ppm) ||
        !writer.append(",\"mode\":") ||
        !writer.append_string(rule.hard ? "hard" : "advisory") ||
        !writer.append(",\"metric\":") || !writer.append_string(rule.metric) ||
        !writer.append(",\"noise_band_ppm\":") || !writer.append_number(rule.noise_band_ppm) ||
        !writer.append(",\"regression_ppm_ci95\":{\"lower\":") ||
        !writer.append_signed_number(intervals[index].lower) ||
        !writer.append(",\"upper\":") || !writer.append_signed_number(intervals[index].upper) ||
        !writer.append("},\"status\":") || !writer.append_string(status) ||
        !writer.append(",\"threshold_ppm\":") || !writer.append_number(threshold) ||
        !writer.append(",\"workload\":") || !writer.append_string(rule.workload) || !writer.append("}")) {
      return false;
    }
    first = false;
  }
  const std::string_view status = first ? "no_threshold" : any_hard_failure ? "hard_regression" : "pass";
  return writer.append("],\"schema_version\":\"laghu-benchmark-evaluation-v1\",\"status\":") &&
      writer.append_string(status) && writer.append("}\n") && write_all(STDOUT_FILENO, writer.view());
}

}  // namespace

int main(int argc, char** argv) {
  Options options{};
  if (!parse_options(argc, argv, options)) {
    static_cast<void>(write_all(STDERR_FILENO,
        "usage: laghu_benchmark_evaluate --baseline <artifact> --candidate <artifact> --manifest <tsv>\n"
        "       laghu_benchmark_evaluate --environment <artifact>\n"));
    return static_cast<int>(ExitCode::invalid_arguments);
  }

  Artifact baseline{};
  std::string_view error;
  if (options.environment != nullptr) {
    if (!load_artifact(options.environment, baseline, error)) {
      static_cast<void>(write_all(STDERR_FILENO, "laghu-benchmark-evaluate: artifact=environment; field="));
      static_cast<void>(write_all(STDERR_FILENO, error));
      static_cast<void>(write_all(STDERR_FILENO, "\n"));
      return static_cast<int>(ExitCode::invalid_input);
    }
    if (!baseline.cpu_description_available) {
      static_cast<void>(write_all(STDERR_FILENO,
          "laghu-benchmark-evaluate: environment=hardware_identity_unavailable\n"));
      return static_cast<int>(ExitCode::invalid_input);
    }
    return write_environment(baseline) ? static_cast<int>(ExitCode::success)
                                       : static_cast<int>(ExitCode::output_failure);
  }

  if (!load_artifact(options.baseline, baseline, error)) {
    static_cast<void>(write_all(STDERR_FILENO, "laghu-benchmark-evaluate: artifact=baseline; field="));
    static_cast<void>(write_all(STDERR_FILENO, error));
    static_cast<void>(write_all(STDERR_FILENO, "\n"));
    return static_cast<int>(ExitCode::invalid_input);
  }
  Artifact candidate{};
  if (!load_artifact(options.candidate, candidate, error)) {
    static_cast<void>(write_all(STDERR_FILENO, "laghu-benchmark-evaluate: artifact=candidate; field="));
    static_cast<void>(write_all(STDERR_FILENO, error));
    static_cast<void>(write_all(STDERR_FILENO, "\n"));
    return static_cast<int>(ExitCode::invalid_input);
  }
  if (!equal_configuration(baseline, candidate, error)) {
    static_cast<void>(write_all(STDERR_FILENO, "laghu-benchmark-evaluate: compatibility="));
    static_cast<void>(write_all(STDERR_FILENO, error));
    static_cast<void>(write_all(STDERR_FILENO, "\n"));
    return static_cast<int>(ExitCode::invalid_input);
  }
  Manifest manifest{};
  if (!parse_manifest(options.manifest, manifest, error)) {
    static_cast<void>(write_all(STDERR_FILENO, "laghu-benchmark-evaluate: manifest="));
    static_cast<void>(write_all(STDERR_FILENO, error));
    static_cast<void>(write_all(STDERR_FILENO, "\n"));
    return static_cast<int>(ExitCode::invalid_input);
  }

  std::array<char, 17U> environment{};
  static_cast<void>(hex_environment(environment_hash(baseline), environment));
  const std::string_view reference_environment{environment.data(), 16U};
  std::array<Interval, maximum_rules> intervals{};
  std::array<bool, maximum_rules> regressions{};
  std::array<bool, maximum_rules> applicable{};
  bool any_hard_failure = false;
  for (std::size_t index = 0U; index < manifest.count; ++index) {
    const Rule& rule = manifest.rules[index];
    if (rule.workload != baseline.workload) {
      continue;
    }
    applicable[index] = true;
    const MetricId metric = metric_id(rule.metric);
    if (rule.reference_environment != reference_environment) {
      static_cast<void>(write_all(STDERR_FILENO,
          "laghu-benchmark-evaluate: manifest=reference_environment_mismatch; expected="));
      static_cast<void>(write_all(STDERR_FILENO, reference_environment));
      static_cast<void>(write_all(STDERR_FILENO, "\n"));
      return static_cast<int>(ExitCode::invalid_input);
    }
    const MetricSeries& baseline_metric = baseline.metrics[metric_index(metric)];
    const MetricSeries& candidate_metric = candidate.metrics[metric_index(metric)];
    if (!baseline_metric.available || !candidate_metric.available) {
      static_cast<void>(write_all(STDERR_FILENO, "laghu-benchmark-evaluate: metric="));
      static_cast<void>(write_all(STDERR_FILENO, rule.metric));
      static_cast<void>(write_all(STDERR_FILENO, "; reason="));
      static_cast<void>(write_all(STDERR_FILENO,
          baseline_metric.available ? "candidate_unavailable\n" : "baseline_unavailable\n"));
      return static_cast<int>(ExitCode::invalid_input);
    }
    const BootstrapResult bootstrap = bootstrap_interval(
        baseline_metric, candidate_metric, metric, rule.higher_is_better, intervals[index]);
    if (bootstrap != BootstrapResult::success) {
      static_cast<void>(write_all(STDERR_FILENO,
          "laghu-benchmark-evaluate: metric="));
      static_cast<void>(write_all(STDERR_FILENO, rule.metric));
      static_cast<void>(write_all(STDERR_FILENO, "; reason="));
      if (bootstrap == BootstrapResult::zero_baseline_undefined) {
        static_cast<void>(write_all(STDERR_FILENO, "zero_baseline_undefined\n"));
      } else if (bootstrap == BootstrapResult::arithmetic_overflow) {
        static_cast<void>(write_all(STDERR_FILENO, "arithmetic_overflow\n"));
      } else {
        static_cast<void>(write_all(STDERR_FILENO, "invalid_samples\n"));
      }
      return static_cast<int>(ExitCode::invalid_input);
    }
    if (rule.allowed_regression_ppm > std::numeric_limits<std::uint64_t>::max() -
        rule.noise_band_ppm) {
      static_cast<void>(write_all(STDERR_FILENO,
          "laghu-benchmark-evaluate: manifest=threshold_overflow\n"));
      return static_cast<int>(ExitCode::invalid_input);
    }
    const std::uint64_t threshold = rule.allowed_regression_ppm + rule.noise_band_ppm;
    regressions[index] = intervals[index].lower > 0 &&
        static_cast<std::uint64_t>(intervals[index].lower) > threshold;
    any_hard_failure = any_hard_failure || (rule.hard && regressions[index]);
  }
  if (!write_result(baseline, manifest, intervals, regressions, applicable, any_hard_failure)) {
    return static_cast<int>(ExitCode::output_failure);
  }
  return any_hard_failure ? static_cast<int>(ExitCode::hard_regression)
                          : static_cast<int>(ExitCode::success);
}
