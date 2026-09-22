// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <laghu/adapters/regex.hpp>
#include <laghu/core/bounded_arena.hpp>
#include <laghu/core/memory_budget.hpp>

namespace {

struct Source final { std::array<std::byte, 131072> storage{}; };

laghu::core::Result<laghu::core::MutableByteView> acquire(
    void* context, std::size_t minimum) noexcept {
  auto& source = *static_cast<Source*>(context);
  if (minimum > source.storage.size()) {
    return std::unexpected{laghu::core::Error{
        laghu::core::ErrorDomain::core, laghu::core::ErrorCode::exhaustion}};
  }
  return laghu::core::MutableByteView::from(source.storage);
}

laghu::core::Result<void> reset(void*) noexcept { return {}; }

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0U || size > 8192U) return 0;
  const std::size_t pattern_size = static_cast<std::size_t>(data[0]) % size;
  const auto input = std::span{reinterpret_cast<const std::byte*>(data), size};
  const auto pattern_bytes = *laghu::core::ByteView::from(input.subspan(1U, pattern_size));
  const auto subject_bytes = *laghu::core::ByteView::from(input.subspan(1U + pattern_size));
  constexpr auto worker = *laghu::core::WorkerId::from_uint64(1U);
  Source source{};
  laghu::core::MemoryBudget budget{worker, source.storage.size()};
  laghu::core::BoundedArena arena{worker, budget, {&source, acquire, reset},
                                   4096U, source.storage.size()};
  laghu::adapters::NativeMemoryPool memory{worker, arena};
  laghu::adapters::RegexCompileFailure failure{};
  auto pattern = laghu::adapters::RegexPattern::compile(
      pattern_bytes, memory, {4096U, 64U, 32U}, failure);
  if (pattern.has_value()) {
    std::array<laghu::adapters::RegexCapture, 33> captures{};
    (void)pattern->match(subject_bytes, captures,
                         {4096U, 10000U, 128U, captures.size()});
    pattern = std::unexpected{laghu::core::Error{
        laghu::core::ErrorDomain::core, laghu::core::ErrorCode::invalid_state}};
  }
  const auto boundary = arena.quiescent_boundary(worker);
  if (boundary.has_value()) (void)arena.reset(worker, *boundary);
  return 0;
}
