// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "laghu_test_support.hpp"

#include <laghu/adapters/regex.hpp>
#include <laghu/core/bounded_arena.hpp>
#include <laghu/core/memory_budget.hpp>

namespace {

using laghu::adapters::NativeMemoryPool;
using laghu::adapters::RegexCapture;
using laghu::adapters::RegexCompileFailure;
using laghu::adapters::RegexCompileLimits;
using laghu::adapters::RegexMatchLimits;
using laghu::adapters::RegexPattern;
using laghu::core::ArenaBlockSource;
using laghu::core::BoundedArena;
using laghu::core::ByteView;
using laghu::core::Error;
using laghu::core::ErrorCode;
using laghu::core::MemoryBudget;
using laghu::core::MutableByteView;
using laghu::core::Result;
using laghu::core::WorkerId;

struct FixedSource final {
  std::array<std::byte, 262144> storage{};
  bool fail{};
};

[[nodiscard]] Result<MutableByteView> acquire(void* context,
                                               std::size_t minimum) noexcept {
  auto& source = *static_cast<FixedSource*>(context);
  if (source.fail || minimum > source.storage.size()) {
    return std::unexpected{Error{laghu::core::ErrorDomain::core,
                                 ErrorCode::exhaustion, 0,
                                 "injected regex allocation failure"}};
  }
  return MutableByteView::from(source.storage);
}

[[nodiscard]] Result<void> reset(void*) noexcept { return {}; }

[[nodiscard]] ArenaBlockSource block_source(FixedSource& source) noexcept {
  return {&source, acquire, reset};
}

[[nodiscard]] ByteView bytes(std::string_view text) noexcept {
  return *ByteView::from(std::as_bytes(std::span{text.data(), text.size()}));
}

struct Fixture final {
  static constexpr WorkerId worker = *WorkerId::from_uint64(1U);
  FixedSource source{};
  MemoryBudget budget{worker, source.storage.size()};
  BoundedArena arena{worker, budget, block_source(source), 4096U, source.storage.size()};
  NativeMemoryPool memory{worker, arena};

  ~Fixture() {
    const auto boundary = arena.quiescent_boundary(worker);
    if (boundary.has_value()) (void)arena.reset(worker, *boundary);
  }
};

constexpr RegexCompileLimits compile_limits{1024U, 64U, 16U};
constexpr RegexMatchLimits match_limits{4096U, 100000U, 256U, 8U};

[[nodiscard]] bool golden_and_captures() noexcept {
  Fixture fixture;
  RegexCompileFailure failure{};
  auto pattern = RegexPattern::compile(bytes("^/users/([0-9]+)$"), fixture.memory,
                                       compile_limits, failure);
  if (!pattern.has_value()) return false;
  std::array<RegexCapture, 8> captures{};
  const auto matched = pattern->match(bytes("/users/42"), captures, match_limits);
  const auto missed = pattern->match(bytes("/groups/42"), captures, match_limits);
  return matched.has_value() && matched->matched && matched->capture_count == 2U &&
         captures[0].matched && captures[0].begin == 0U && captures[0].end == 9U &&
         captures[1].matched && captures[1].begin == 7U && captures[1].end == 9U &&
         missed.has_value() && !missed->matched;
}

[[nodiscard]] bool text_views() noexcept {
  Fixture fixture;
  RegexCompileFailure failure{};
  auto pattern = RegexPattern::compile(laghu::core::TextView::from("^laghu$"),
                                       fixture.memory, compile_limits, failure);
  if (!pattern.has_value()) return false;
  std::array<RegexCapture, 8> captures{};
  const auto matched = pattern->match(laghu::core::TextView::from("laghu"),
                                      captures, match_limits);
  return matched.has_value() && matched->matched && captures[0].matched &&
         captures[0].begin == 0U && captures[0].end == 5U;
}

[[nodiscard]] bool invalid_pattern_and_limits() noexcept {
  Fixture fixture;
  RegexCompileFailure failure{};
  auto invalid = RegexPattern::compile(bytes("a("), fixture.memory,
                                       compile_limits, failure);
  if (invalid.has_value() || failure.offset != 2U ||
      invalid.error().dependency_id() != laghu::core::DependencyId::pcre2_8bit ||
      invalid.error().dependency_operation() !=
          laghu::core::DependencyOperation::regex_compile) {
    return false;
  }
  const RegexCompileLimits small_pattern{1U, 64U, 16U};
  const RegexCompileLimits small_capture{1024U, 64U, 1U};
  return !RegexPattern::compile(bytes("ab"), fixture.memory, small_pattern, failure).has_value() &&
         !RegexPattern::compile(bytes("(a)(b)"), fixture.memory,
                                small_capture, failure).has_value();
}

[[nodiscard]] bool match_limits_are_enforced() noexcept {
  Fixture fixture;
  RegexCompileFailure failure{};
  auto pattern = RegexPattern::compile(bytes("(a+)+$"), fixture.memory,
                                       compile_limits, failure);
  if (!pattern.has_value()) return false;
  std::array<RegexCapture, 8> captures{};
  const RegexMatchLimits short_subject{4U, 100U, 32U, captures.size()};
  const RegexMatchLimits short_output{4096U, 100U, 32U, 1U};
  const RegexMatchLimits bounded_work{4096U, 8U, 8U, captures.size()};
  const auto subject_limit = pattern->match(bytes("aaaaa"), captures, short_subject);
  const auto output_limit = pattern->match(bytes("aaaa"), captures, short_output);
  const auto work_limit = pattern->match(
      bytes("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!"), captures, bounded_work);
  return !subject_limit.has_value() && subject_limit.error().code() == ErrorCode::invalid_range &&
         !output_limit.has_value() && output_limit.error().code() == ErrorCode::invalid_range &&
         !work_limit.has_value() && work_limit.error().dependency_operation() ==
             laghu::core::DependencyOperation::regex_match;
}

[[nodiscard]] bool allocation_failure() noexcept {
  Fixture fixture;
  fixture.source.fail = true;
  RegexCompileFailure failure{};
  const auto pattern = RegexPattern::compile(bytes("a"), fixture.memory,
                                              compile_limits, failure);
  return !pattern.has_value() && pattern.error().code() == ErrorCode::exhaustion;
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"regex.golden-captures", golden_and_captures},
      laghu::test::TestCase{"regex.text-views", text_views},
      laghu::test::TestCase{"regex.invalid-pattern-limits", invalid_pattern_and_limits},
      laghu::test::TestCase{"regex.match-limits", match_limits_are_enforced},
      laghu::test::TestCase{"regex.allocation-failure", allocation_failure},
  };
  return laghu::test::run_tests(tests);
}
