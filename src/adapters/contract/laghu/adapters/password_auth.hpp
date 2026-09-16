// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <laghu/adapters/dependency.hpp>
#include <laghu/core/identifiers.hpp>
#include <laghu/core/views.hpp>

namespace laghu::adapters {

struct PasswordVerificationLimits final {
  static constexpr std::size_t maximum_supported_password_bytes = 1024;

  std::size_t maximum_password_bytes{};
  std::uint32_t minimum_bcrypt_cost{};
  std::uint32_t maximum_bcrypt_cost{};
  std::uint32_t minimum_sha512_rounds{};
  std::uint32_t maximum_sha512_rounds{};
};

// Password work may only enter through this caller-owned, worker-affine
// boundary. The synchronous adapter holds one in-flight slot for the native
// call; scheduling and worker lifetime remain caller-owned.
class PasswordAuthWorker final {
 public:
  constexpr PasswordAuthWorker(core::WorkerId worker,
                               std::size_t maximum_in_flight) noexcept
      : worker_(worker), maximum_in_flight_(maximum_in_flight) {}

  PasswordAuthWorker(const PasswordAuthWorker&) = delete;
  PasswordAuthWorker& operator=(const PasswordAuthWorker&) = delete;
  PasswordAuthWorker(PasswordAuthWorker&&) = delete;
  PasswordAuthWorker& operator=(PasswordAuthWorker&&) = delete;

 private:
  friend core::Result<bool> verify_password(
      PasswordAuthWorker&, core::WorkerId, core::TextView, core::TextView,
      PasswordVerificationLimits, DependencyLogSink) noexcept;

  core::WorkerId worker_;
  std::size_t maximum_in_flight_{};
  std::size_t active_{};
};

// `password` and `encoded_hash` remain caller-borrowed. The adapter retains
// neither input, cleanses its temporary C password buffer, and never logs it.
[[nodiscard]] core::Result<bool> verify_password(
    PasswordAuthWorker& auth_worker, core::WorkerId worker, core::TextView password,
    core::TextView encoded_hash, PasswordVerificationLimits limits,
    DependencyLogSink log_sink = {}) noexcept;

static_assert(std::is_trivially_copyable_v<PasswordVerificationLimits>);
static_assert(std::is_standard_layout_v<PasswordVerificationLimits>);
static_assert(!std::is_copy_constructible_v<PasswordAuthWorker>);

}  // namespace laghu::adapters
