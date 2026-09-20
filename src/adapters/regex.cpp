// SPDX-License-Identifier: AGPL-3.0-only
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <utility>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <laghu/adapters/internal/arena_memory.hpp>
#include <laghu/adapters/regex.hpp>

namespace laghu::adapters {
namespace {

struct State final {
  internal::ArenaMemory memory;
  pcre2_general_context* general{};
  DependencyLogSink log{};
  std::uint32_t capture_count{};
};

[[nodiscard]] constexpr core::Error core_error(core::ErrorCode code,
                                                const char* diagnostic) noexcept {
  return {core::ErrorDomain::core, code, 0, diagnostic};
}

[[nodiscard]] constexpr core::DependencyStatus status_for(int code) noexcept {
  if (code == PCRE2_ERROR_NOMEMORY || code == PCRE2_ERROR_HEAP_FAILED) {
    return core::DependencyStatus::exhaustion;
  }
  if (code == PCRE2_ERROR_MATCHLIMIT || code == PCRE2_ERROR_DEPTHLIMIT ||
      code == PCRE2_ERROR_HEAPLIMIT) return core::DependencyStatus::invalid_range;
  if (code == PCRE2_ERROR_BADOPTION || code == PCRE2_ERROR_BADOFFSET ||
      code == PCRE2_ERROR_NULL) return core::DependencyStatus::invalid_input;
  return core::DependencyStatus::invalid_input;
}

[[nodiscard]] core::Error native_error(core::DependencyOperation operation, int code,
                                       const DependencyLogSink& sink) noexcept {
  const core::Error error = normalize_dependency_error(
      core::DependencyId::pcre2_8bit, operation, status_for(code), code);
  log_dependency_error(sink, error);
  return error;
}

[[nodiscard]] bool valid_compile_limits(const RegexCompileLimits& limits) noexcept {
  return limits.maximum_pattern_bytes != 0U && limits.maximum_pattern_depth != 0U &&
         limits.maximum_capture_count != 0U;
}

[[nodiscard]] bool valid_match_limits(const RegexMatchLimits& limits) noexcept {
  return limits.maximum_subject_bytes != 0U && limits.maximum_match_steps != 0U &&
         limits.maximum_depth != 0U && limits.maximum_output_captures != 0U;
}

}  // namespace

RegexPattern::RegexPattern(RegexPattern&& other) noexcept { move_from(std::move(other)); }

RegexPattern& RegexPattern::operator=(RegexPattern&& other) noexcept {
  if (this != &other) {
    release();
    move_from(std::move(other));
  }
  return *this;
}

RegexPattern::~RegexPattern() { release(); }

namespace {

[[nodiscard]] core::ByteView bytes_of(core::TextView text) noexcept {
  const auto characters = std::span<const char>{text.data(), text.size()};
  return *core::ByteView::from(std::as_bytes(characters));
}

}  // namespace

core::Result<RegexPattern> RegexPattern::compile(
    core::ByteView pattern, NativeMemoryPool& memory, RegexCompileLimits limits,
    RegexCompileFailure& failure, DependencyLogSink log_sink) noexcept {
  failure = {};
  if (!valid_compile_limits(limits)) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "regex compile limits must be positive")};
  }
  if (pattern.size() > limits.maximum_pattern_bytes) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "regex pattern exceeds the caller byte limit")};
  }
  internal::ArenaMemoryAccess::synchronize(memory);
  auto& arena = internal::ArenaMemoryAccess::arena(memory);
  const auto worker = internal::ArenaMemoryAccess::worker(memory);
  auto pin = arena.pin(worker);
  if (!pin.has_value()) return std::unexpected{pin.error()};
  internal::ArenaMemory allocator{&memory};
  void* const storage = internal::arena_malloc(sizeof(State), &allocator);
  if (storage == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::exhaustion,
                                      "regex pattern state allocation is exhausted")};
  }
  auto* const state = ::new (storage) State{{&memory}, nullptr, log_sink, 0U};
  state->general = pcre2_general_context_create(internal::arena_malloc,
                                                internal::arena_free, &state->memory);
  if (state->general == nullptr) {
    state->~State();
    internal::arena_free(storage, &allocator);
    return std::unexpected{native_error(core::DependencyOperation::regex_compile,
                                        PCRE2_ERROR_NOMEMORY, log_sink)};
  }
  pcre2_compile_context* const compile_context =
      pcre2_compile_context_create(state->general);
  if (compile_context == nullptr) {
    pcre2_general_context_free(state->general);
    state->~State();
    internal::arena_free(storage, &allocator);
    return std::unexpected{native_error(core::DependencyOperation::regex_compile,
                                        PCRE2_ERROR_NOMEMORY, log_sink)};
  }
  (void)pcre2_set_parens_nest_limit(compile_context, limits.maximum_pattern_depth);
  int compile_error{};
  PCRE2_SIZE error_offset{};
  static constexpr PCRE2_UCHAR empty_pattern{};
  const auto* const pattern_data = pattern.empty()
      ? &empty_pattern
      : reinterpret_cast<PCRE2_SPTR>(pattern.data());
  pcre2_code* const code = pcre2_compile(pattern_data, pattern.size(), 0U,
                                          &compile_error, &error_offset, compile_context);
  pcre2_compile_context_free(compile_context);
  if (code == nullptr) {
    failure.offset = static_cast<std::size_t>(error_offset);
    pcre2_general_context_free(state->general);
    state->~State();
    internal::arena_free(storage, &allocator);
    return std::unexpected{native_error(core::DependencyOperation::regex_compile,
                                        compile_error, log_sink)};
  }
  std::uint32_t capture_count{};
  const int info = pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &capture_count);
  if (info != 0 || capture_count > limits.maximum_capture_count) {
    pcre2_code_free(code);
    pcre2_general_context_free(state->general);
    state->~State();
    internal::arena_free(storage, &allocator);
    if (info != 0) {
      return std::unexpected{native_error(core::DependencyOperation::regex_compile,
                                          info, log_sink)};
    }
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "regex pattern exceeds the caller capture limit")};
  }
  state->capture_count = capture_count;
  return RegexPattern{code, state, arena, arena.generation(), std::move(*pin)};
}

core::Result<RegexPattern> RegexPattern::compile(
    core::TextView pattern, NativeMemoryPool& memory, RegexCompileLimits limits,
    RegexCompileFailure& failure, DependencyLogSink log_sink) noexcept {
  return compile(bytes_of(pattern), memory, limits, failure, log_sink);
}

core::Result<void> RegexPattern::require_valid() const noexcept {
  if (code_ == nullptr || state_ == nullptr || arena_ == nullptr ||
      arena_->generation() != generation_) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "regex pattern is inactive or its arena was reset")};
  }
  return {};
}

core::Result<RegexMatchResult> RegexPattern::match(
    core::ByteView subject, std::span<RegexCapture> captures,
    RegexMatchLimits limits) const noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  if (!valid_match_limits(limits)) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "regex match limits must be positive")};
  }
  if (subject.size() > limits.maximum_subject_bytes) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "regex subject exceeds the caller byte limit")};
  }
  const auto& state = *static_cast<const State*>(state_);
  const std::size_t required_captures = static_cast<std::size_t>(state.capture_count) + 1U;
  if (limits.maximum_output_captures > captures.size() ||
      limits.maximum_output_captures < required_captures ||
      limits.maximum_output_captures > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "regex capture output capacity is insufficient")};
  }
  pcre2_match_context* const match_context = pcre2_match_context_create(state.general);
  if (match_context == nullptr) {
    return std::unexpected{native_error(core::DependencyOperation::regex_match,
                                        PCRE2_ERROR_NOMEMORY, state.log)};
  }
  (void)pcre2_set_match_limit(match_context, limits.maximum_match_steps);
  (void)pcre2_set_depth_limit(match_context, limits.maximum_depth);
  pcre2_match_data* const match_data =
      pcre2_match_data_create(static_cast<std::uint32_t>(limits.maximum_output_captures),
                              state.general);
  if (match_data == nullptr) {
    pcre2_match_context_free(match_context);
    return std::unexpected{native_error(core::DependencyOperation::regex_match,
                                        PCRE2_ERROR_NOMEMORY, state.log)};
  }
  static constexpr PCRE2_UCHAR empty_subject{};
  const auto* const subject_data = subject.empty()
      ? &empty_subject
      : reinterpret_cast<PCRE2_SPTR>(subject.data());
  const int result = pcre2_match(static_cast<const pcre2_code*>(code_), subject_data,
                                 subject.size(), 0U, 0U, match_data, match_context);
  if (result == PCRE2_ERROR_NOMATCH) {
    std::fill_n(captures.begin(), limits.maximum_output_captures, RegexCapture{});
    pcre2_match_data_free(match_data);
    pcre2_match_context_free(match_context);
    return RegexMatchResult{};
  }
  if (result < 0) {
    const core::Error error = native_error(core::DependencyOperation::regex_match,
                                           result, state.log);
    pcre2_match_data_free(match_data);
    pcre2_match_context_free(match_context);
    return std::unexpected{error};
  }
  const auto* const vector = pcre2_get_ovector_pointer(match_data);
  const std::size_t count = static_cast<std::size_t>(result);
  std::fill_n(captures.begin(), limits.maximum_output_captures, RegexCapture{});
  for (std::size_t index = 0; index < count; ++index) {
    const PCRE2_SIZE begin = vector[index * 2U];
    const PCRE2_SIZE end = vector[index * 2U + 1U];
    captures[index] = begin == PCRE2_UNSET
        ? RegexCapture{}
        : RegexCapture{static_cast<std::size_t>(begin), static_cast<std::size_t>(end), true};
  }
  pcre2_match_data_free(match_data);
  pcre2_match_context_free(match_context);
  return RegexMatchResult{count, true};
}

core::Result<RegexMatchResult> RegexPattern::match(
    core::TextView subject, std::span<RegexCapture> captures,
    RegexMatchLimits limits) const noexcept {
  return match(bytes_of(subject), captures, limits);
}

void RegexPattern::release() noexcept {
  if (code_ != nullptr && state_ != nullptr && arena_ != nullptr &&
      arena_->generation() == generation_) {
    auto* const state = static_cast<State*>(state_);
    pcre2_code_free(static_cast<pcre2_code*>(code_));
    pcre2_general_context_free(state->general);
    internal::ArenaMemory allocator{state->memory.pool};
    state->~State();
    internal::arena_free(state, &allocator);
  }
  code_ = nullptr;
  state_ = nullptr;
  arena_ = nullptr;
  generation_ = 0U;
  pin_ = {};
}

void RegexPattern::move_from(RegexPattern&& other) noexcept {
  code_ = std::exchange(other.code_, nullptr);
  state_ = std::exchange(other.state_, nullptr);
  arena_ = std::exchange(other.arena_, nullptr);
  generation_ = std::exchange(other.generation_, 0U);
  pin_ = std::move(other.pin_);
}

}  // namespace laghu::adapters
