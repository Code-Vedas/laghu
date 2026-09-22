// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

#include <laghu/adapters/dependency.hpp>
#include <laghu/adapters/native_memory.hpp>
#include <laghu/core/views.hpp>

namespace laghu::adapters {

struct RegexCompileLimits final {
  std::size_t maximum_pattern_bytes{};
  std::uint32_t maximum_pattern_depth{};
  std::uint32_t maximum_capture_count{};
};

struct RegexMatchLimits final {
  std::size_t maximum_subject_bytes{};
  std::uint32_t maximum_match_steps{};
  std::uint32_t maximum_depth{};
  std::size_t maximum_output_captures{};
};

struct RegexCompileFailure final {
  std::size_t offset{};
};

struct RegexCapture final {
  std::size_t begin{};
  std::size_t end{};
  bool matched{};
};

struct RegexMatchResult final {
  std::size_t capture_count{};
  bool matched{};
};

class RegexPattern final {
 public:
  RegexPattern(const RegexPattern&) = delete;
  RegexPattern& operator=(const RegexPattern&) = delete;
  RegexPattern(RegexPattern&& other) noexcept;
  RegexPattern& operator=(RegexPattern&& other) noexcept;
  ~RegexPattern();

  // The memory pool and its arena must outlive this pattern. Compilation never
  // enables JIT. failure.offset is set to the bounded pattern offset reported
  // by PCRE2 when compilation fails.
  [[nodiscard]] static core::Result<RegexPattern> compile(
      core::ByteView pattern, NativeMemoryPool& memory, RegexCompileLimits limits,
      RegexCompileFailure& failure, DependencyLogSink log_sink = {}) noexcept;
  [[nodiscard]] static core::Result<RegexPattern> compile(
      core::TextView pattern, NativeMemoryPool& memory, RegexCompileLimits limits,
      RegexCompileFailure& failure, DependencyLogSink log_sink = {}) noexcept;

  [[nodiscard]] core::Result<RegexMatchResult> match(
      core::ByteView subject, std::span<RegexCapture> captures,
      RegexMatchLimits limits) const noexcept;
  [[nodiscard]] core::Result<RegexMatchResult> match(
      core::TextView subject, std::span<RegexCapture> captures,
      RegexMatchLimits limits) const noexcept;

 private:
  RegexPattern(void* code, void* state, const core::BoundedArena& arena,
               std::uint64_t generation, core::ArenaPin pin) noexcept
      : code_(code), state_(state), arena_(&arena), generation_(generation),
        pin_(std::move(pin)) {}

  [[nodiscard]] core::Result<void> require_valid() const noexcept;
  void release() noexcept;
  void move_from(RegexPattern&& other) noexcept;

  void* code_{};
  void* state_{};
  const core::BoundedArena* arena_{};
  std::uint64_t generation_{};
  core::ArenaPin pin_{};
};

static_assert(std::is_trivially_copyable_v<RegexCompileLimits>);
static_assert(std::is_trivially_copyable_v<RegexMatchLimits>);
static_assert(std::is_trivially_copyable_v<RegexCompileFailure>);
static_assert(std::is_trivially_copyable_v<RegexCapture>);
static_assert(std::is_trivially_copyable_v<RegexMatchResult>);
static_assert(!std::is_copy_constructible_v<RegexPattern>);

}  // namespace laghu::adapters
