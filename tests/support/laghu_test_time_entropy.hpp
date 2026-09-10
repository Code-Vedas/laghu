// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <laghu/adapters/internal/entropy.hpp>
#include <laghu/core/clocks.hpp>
#include <laghu/core/views.hpp>

namespace laghu::test {

class ManualClock final {
 public:
  constexpr ManualClock(core::MonotonicInstant monotonic, core::RealtimeInstant realtime) noexcept
      : monotonic_(monotonic), realtime_(realtime) {}

  [[nodiscard]] core::Result<void> advance_monotonic_to(core::MonotonicInstant target) noexcept;
  [[nodiscard]] core::Result<void> advance_monotonic_by(core::MonotonicInstant elapsed) noexcept;
  constexpr void jump_realtime_to(core::RealtimeInstant target) noexcept { realtime_ = target; }

  [[nodiscard]] constexpr core::MonotonicInstant monotonic() const noexcept { return monotonic_; }
  [[nodiscard]] constexpr core::RealtimeInstant realtime() const noexcept { return realtime_; }
  [[nodiscard]] core::ClockOperations operations() noexcept;

 private:
  [[nodiscard]] static core::Result<core::MonotonicInstant> read_monotonic(
      void* context) noexcept;
  [[nodiscard]] static core::Result<core::RealtimeInstant> read_realtime(void* context) noexcept;

  core::MonotonicInstant monotonic_{};
  core::RealtimeInstant realtime_{};
};

class DeterministicEntropy final {
 public:
  explicit constexpr DeterministicEntropy(std::span<const std::byte> bytes) noexcept
      : bytes_(bytes) {}

  [[nodiscard]] bool fail_on_call(std::size_t call, int native_error) noexcept;
  [[nodiscard]] core::Result<void> fill(core::MutableByteView output) noexcept;
  [[nodiscard]] constexpr adapters::internal::EntropyCall call() const noexcept {
    return fill_call;
  }
  [[nodiscard]] constexpr void* context() noexcept { return this; }
  [[nodiscard]] constexpr std::size_t calls() const noexcept { return calls_; }
  [[nodiscard]] constexpr std::size_t consumed() const noexcept { return offset_; }

 private:
  [[nodiscard]] static int fill_call(core::MutableByteView output, void* context) noexcept;

  std::span<const std::byte> bytes_{};
  std::size_t offset_{};
  std::size_t calls_{};
  std::size_t failure_call_{};
  int failure_error_{};
};

}  // namespace laghu::test
