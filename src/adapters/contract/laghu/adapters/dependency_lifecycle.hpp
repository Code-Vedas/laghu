// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <laghu/core/contract.hpp>

namespace laghu::adapters {

// A full build has one selected TLS provider plus 13 other direct runtime
// dependencies. OpenSSL and LibreSSL are mutually exclusive.
inline constexpr std::size_t dependency_lifecycle_capacity = 14;

// Every count must be zero before the master forks. A dependency owns these
// counts through its caller-provided hook; the registry never inspects a
// native dependency object or stores one itself.
struct DependencyLiveState final {
  std::uint32_t sessions{};
  std::uint32_t contexts{};
  std::uint32_t threads{};
  std::uint32_t callbacks{};

  [[nodiscard]] constexpr bool is_quiescent() const noexcept {
    return sessions == 0 && contexts == 0 && threads == 0 && callbacks == 0;
  }
};

using DependencyLifecycleAction = core::Result<void> (*)(void*) noexcept;
using DependencyLiveStateRead = DependencyLiveState (*)(void*) noexcept;

// This is a caller-owned, nonvirtual function table. The context is borrowed
// for the lifetime of the registry and must be valid in the master and each
// child until that process has completed its corresponding cleanup stage.
struct DependencyLifecycleHooks final {
  core::DependencyId dependency{core::DependencyId::none};
  void* context{};
  DependencyLifecycleAction preflight{};
  DependencyLifecycleAction worker_initialize{};
  DependencyLifecycleAction worker_cleanup{};
  DependencyLifecycleAction master_cleanup{};
  DependencyLiveStateRead live_state{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return dependency != core::DependencyId::none && core::is_known_dependency_id(dependency) &&
           preflight != nullptr &&
           worker_initialize != nullptr && worker_cleanup != nullptr &&
           master_cleanup != nullptr && live_state != nullptr;
  }
};

struct DependencyForkEpoch final {
  std::uint64_t master_process{};
  std::uint64_t value{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return master_process != 0 && value != 0;
  }
};

enum class DependencyLifecyclePhase : std::uint8_t {
  collecting,
  preflighted,
  preflight_failed,
  preflight_rollback_failed,
  fork_ready,
  worker_ready,
  worker_rolled_back,
  worker_rollback_failed,
  worker_cleaned,
  worker_cleanup_failed,
  master_cleaned,
  master_cleanup_failed,
};

// The registry is intentionally process-local. A master calls preflight(),
// verifies its single-threaded and quiescent fork boundary with prepare_fork(),
// then each child calls worker_initialize() using the returned epoch. The
// parent never initializes a worker and a child never performs master cleanup.
class DependencyLifecycleRegistry final {
 public:
  DependencyLifecycleRegistry() noexcept;
  DependencyLifecycleRegistry(const DependencyLifecycleRegistry&) = delete;
  DependencyLifecycleRegistry& operator=(const DependencyLifecycleRegistry&) = delete;
  DependencyLifecycleRegistry(DependencyLifecycleRegistry&&) = delete;
  DependencyLifecycleRegistry& operator=(DependencyLifecycleRegistry&&) = delete;

  [[nodiscard]] core::Result<void> register_dependency(
      DependencyLifecycleHooks hooks) noexcept;
  [[nodiscard]] core::Result<void> preflight() noexcept;
  [[nodiscard]] core::Result<DependencyForkEpoch> prepare_fork(
      std::uint32_t master_thread_count) noexcept;
  [[nodiscard]] core::Result<void> worker_initialize(DependencyForkEpoch epoch) noexcept;
  [[nodiscard]] core::Result<void> worker_cleanup() noexcept;
  [[nodiscard]] core::Result<void> master_cleanup() noexcept;

  [[nodiscard]] constexpr DependencyLifecyclePhase phase() const noexcept { return phase_; }
  [[nodiscard]] constexpr std::size_t registered_count() const noexcept { return count_; }
  [[nodiscard]] constexpr std::uint64_t master_process() const noexcept {
    return master_process_;
  }
  [[nodiscard]] constexpr std::uint64_t worker_process() const noexcept {
    return worker_process_;
  }
  [[nodiscard]] constexpr DependencyForkEpoch fork_epoch() const noexcept {
    return {master_process_, fork_epoch_};
  }

 private:
  [[nodiscard]] core::Result<void> persistent_failure() const noexcept;
  [[nodiscard]] core::Result<DependencyForkEpoch> persistent_fork_failure() const noexcept;
  void set_terminal_failure(DependencyLifecyclePhase phase, core::Error failure) noexcept;

  std::array<DependencyLifecycleHooks, dependency_lifecycle_capacity> hooks_{};
  DependencyLifecyclePhase phase_{DependencyLifecyclePhase::collecting};
  core::Error terminal_failure_{core::ErrorDomain::core, core::ErrorCode::invalid_state};
  std::uint64_t master_process_{};
  std::uint64_t worker_process_{};
  std::uint64_t fork_epoch_{};
  std::size_t count_{};
  std::size_t preflighted_count_{};
  std::size_t worker_initialized_count_{};
  bool has_terminal_failure_{};
};

static_assert(std::is_trivially_copyable_v<DependencyLiveState>);
static_assert(std::is_standard_layout_v<DependencyLiveState>);
static_assert(std::is_trivially_copyable_v<DependencyLifecycleHooks>);
static_assert(std::is_standard_layout_v<DependencyLifecycleHooks>);
static_assert(std::is_trivially_copyable_v<DependencyForkEpoch>);
static_assert(std::is_standard_layout_v<DependencyForkEpoch>);

}  // namespace laghu::adapters
