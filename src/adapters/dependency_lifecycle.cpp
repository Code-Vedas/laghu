// SPDX-License-Identifier: AGPL-3.0-only
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include <unistd.h>

#include <laghu/adapters/dependency_lifecycle.hpp>

namespace laghu::adapters {
namespace {

[[nodiscard]] std::uint64_t current_process() noexcept {
  const pid_t process = ::getpid();
  return process > 0 ? static_cast<std::uint64_t>(process) : 0;
}

[[nodiscard]] core::Error lifecycle_error(core::ErrorCode code,
                                          std::string_view diagnostic) noexcept {
  return core::Error{core::ErrorDomain::core, code, 0, diagnostic};
}

[[nodiscard]] core::Result<void> lifecycle_failure(core::ErrorCode code,
                                                    std::string_view diagnostic) noexcept {
  return std::unexpected{lifecycle_error(code, diagnostic)};
}

[[nodiscard]] core::Result<DependencyForkEpoch> fork_failure(
    core::ErrorCode code, std::string_view diagnostic) noexcept {
  return std::unexpected{lifecycle_error(code, diagnostic)};
}

[[nodiscard]] bool is_master_process(const DependencyLifecycleRegistry& registry) noexcept {
  return registry.master_process() != 0 && current_process() == registry.master_process();
}

[[nodiscard]] constexpr bool is_tls_provider(core::DependencyId dependency) noexcept {
  return dependency == core::DependencyId::openssl || dependency == core::DependencyId::libressl;
}

}  // namespace

DependencyLifecycleRegistry::DependencyLifecycleRegistry() noexcept
    : master_process_(current_process()) {}

core::Result<void> DependencyLifecycleRegistry::persistent_failure() const noexcept {
  if (!has_terminal_failure_) {
    return lifecycle_failure(core::ErrorCode::invalid_state,
                             "dependency lifecycle failure state is missing detail");
  }
  return std::unexpected{terminal_failure_};
}

core::Result<DependencyForkEpoch> DependencyLifecycleRegistry::persistent_fork_failure() const
    noexcept {
  if (!has_terminal_failure_) {
    return fork_failure(core::ErrorCode::invalid_state,
                        "dependency lifecycle failure state is missing detail");
  }
  return std::unexpected{terminal_failure_};
}

void DependencyLifecycleRegistry::set_terminal_failure(DependencyLifecyclePhase phase,
                                                        core::Error failure) noexcept {
  terminal_failure_ = failure;
  has_terminal_failure_ = true;
  phase_ = phase;
}

core::Result<void> DependencyLifecycleRegistry::register_dependency(
    DependencyLifecycleHooks hooks) noexcept {
  if (!is_master_process(*this)) {
    return lifecycle_failure(core::ErrorCode::invalid_state,
                             "dependency lifecycle registration requires master owner");
  }
  if (phase_ != DependencyLifecyclePhase::collecting) {
    return lifecycle_failure(core::ErrorCode::invalid_state,
                             "dependency lifecycle registration transition is invalid");
  }
  if (!hooks.valid()) {
    return lifecycle_failure(core::ErrorCode::invalid_input,
                             "dependency lifecycle hooks are invalid");
  }
  for (std::size_t index = 0; index < count_; ++index) {
    if (hooks_[index].dependency == hooks.dependency) {
      return lifecycle_failure(core::ErrorCode::invalid_state,
                               "dependency lifecycle registration is duplicate");
    }
    if (is_tls_provider(hooks_[index].dependency) && is_tls_provider(hooks.dependency)) {
      return lifecycle_failure(core::ErrorCode::invalid_state,
                               "dependency lifecycle TLS providers conflict");
    }
  }
  if (count_ == hooks_.size()) {
    return lifecycle_failure(core::ErrorCode::exhaustion,
                             "dependency lifecycle registry capacity exhausted");
  }
  hooks_[count_] = hooks;
  ++count_;
  return {};
}

core::Result<void> DependencyLifecycleRegistry::preflight() noexcept {
  if (!is_master_process(*this)) {
    return lifecycle_failure(core::ErrorCode::invalid_state,
                             "dependency lifecycle preflight requires master owner");
  }
  if (phase_ == DependencyLifecyclePhase::preflighted ||
      phase_ == DependencyLifecyclePhase::fork_ready) {
    return {};
  }
  if (phase_ == DependencyLifecyclePhase::preflight_rollback_failed) {
    return persistent_failure();
  }
  if (phase_ != DependencyLifecyclePhase::collecting) {
    return lifecycle_failure(core::ErrorCode::invalid_state,
                             "dependency lifecycle preflight transition is invalid");
  }
  for (std::size_t index = 0; index < count_; ++index) {
    const auto result = hooks_[index].preflight(hooks_[index].context);
    if (!result.has_value()) {
      core::Error first_cleanup_failure{core::ErrorDomain::core, core::ErrorCode::invalid_state};
      bool cleanup_failed{};
      while (preflighted_count_ != 0) {
        --preflighted_count_;
        const auto cleanup = hooks_[preflighted_count_].master_cleanup(
            hooks_[preflighted_count_].context);
        if (!cleanup.has_value() && !cleanup_failed) {
          first_cleanup_failure = cleanup.error();
          cleanup_failed = true;
        }
      }
      if (cleanup_failed) {
        set_terminal_failure(DependencyLifecyclePhase::preflight_rollback_failed,
                             first_cleanup_failure);
        return std::unexpected{first_cleanup_failure};
      }
      phase_ = DependencyLifecyclePhase::preflight_failed;
      return std::unexpected{result.error()};
    }
    ++preflighted_count_;
  }
  phase_ = DependencyLifecyclePhase::preflighted;
  return {};
}

core::Result<DependencyForkEpoch> DependencyLifecycleRegistry::prepare_fork(
    std::uint32_t master_thread_count) noexcept {
  if (!is_master_process(*this)) {
    return fork_failure(core::ErrorCode::invalid_state,
                        "dependency lifecycle fork requires master owner");
  }
  if (phase_ == DependencyLifecyclePhase::preflight_rollback_failed ||
      phase_ == DependencyLifecyclePhase::master_cleanup_failed) {
    return persistent_fork_failure();
  }
  if (phase_ != DependencyLifecyclePhase::preflighted &&
      phase_ != DependencyLifecyclePhase::fork_ready) {
    return fork_failure(core::ErrorCode::invalid_state,
                        "dependency lifecycle fork transition is invalid");
  }
  if (master_thread_count != 1) {
    return fork_failure(core::ErrorCode::invalid_state,
                        "dependency lifecycle fork requires one master thread");
  }
  for (std::size_t index = 0; index < count_; ++index) {
    if (!hooks_[index].live_state(hooks_[index].context).is_quiescent()) {
      return fork_failure(core::ErrorCode::invalid_state,
                          "dependency lifecycle fork requires quiescent state");
    }
  }
  if (phase_ == DependencyLifecyclePhase::fork_ready) {
    return DependencyForkEpoch{master_process_, fork_epoch_};
  }
  if (fork_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
    return fork_failure(core::ErrorCode::overflow,
                        "dependency lifecycle fork epoch is exhausted");
  }
  ++fork_epoch_;
  phase_ = DependencyLifecyclePhase::fork_ready;
  return DependencyForkEpoch{master_process_, fork_epoch_};
}

core::Result<void> DependencyLifecycleRegistry::worker_initialize(
    DependencyForkEpoch epoch) noexcept {
  const std::uint64_t process = current_process();
  if (!epoch.valid() || epoch.master_process != master_process_ || epoch.value != fork_epoch_) {
    return lifecycle_failure(core::ErrorCode::invalid_state,
                             "dependency lifecycle worker epoch is invalid");
  }
  if (process == 0 || process == master_process_) {
    return lifecycle_failure(core::ErrorCode::invalid_state,
                             "dependency lifecycle worker initialization requires forked child");
  }
  if (phase_ == DependencyLifecyclePhase::worker_rollback_failed ||
      phase_ == DependencyLifecyclePhase::worker_cleanup_failed) {
    return persistent_failure();
  }
  if (phase_ == DependencyLifecyclePhase::worker_ready && worker_process_ == process) {
    return {};
  }
  if (phase_ != DependencyLifecyclePhase::fork_ready) {
    return lifecycle_failure(core::ErrorCode::invalid_state,
                             "dependency lifecycle worker initialization transition is invalid");
  }
  worker_process_ = process;
  for (std::size_t index = 0; index < count_; ++index) {
    const auto result = hooks_[index].worker_initialize(hooks_[index].context);
    if (!result.has_value()) {
      core::Error first_cleanup_failure{core::ErrorDomain::core, core::ErrorCode::invalid_state};
      bool cleanup_failed{};
      while (worker_initialized_count_ != 0) {
        --worker_initialized_count_;
        const auto cleanup = hooks_[worker_initialized_count_].worker_cleanup(
            hooks_[worker_initialized_count_].context);
        if (!cleanup.has_value() && !cleanup_failed) {
          first_cleanup_failure = cleanup.error();
          cleanup_failed = true;
        }
      }
      if (cleanup_failed) {
        set_terminal_failure(DependencyLifecyclePhase::worker_rollback_failed,
                             first_cleanup_failure);
        return std::unexpected{first_cleanup_failure};
      }
      phase_ = DependencyLifecyclePhase::worker_rolled_back;
      return std::unexpected{result.error()};
    }
    ++worker_initialized_count_;
  }
  phase_ = DependencyLifecyclePhase::worker_ready;
  return {};
}

core::Result<void> DependencyLifecycleRegistry::worker_cleanup() noexcept {
  const std::uint64_t process = current_process();
  if (process == 0 || process == master_process_ || process != worker_process_) {
    return lifecycle_failure(core::ErrorCode::invalid_state,
                             "dependency lifecycle worker cleanup requires worker owner");
  }
  if (phase_ == DependencyLifecyclePhase::worker_rollback_failed ||
      phase_ == DependencyLifecyclePhase::worker_cleanup_failed) {
    return persistent_failure();
  }
  if (phase_ == DependencyLifecyclePhase::worker_rolled_back ||
      phase_ == DependencyLifecyclePhase::worker_cleaned) {
    return {};
  }
  if (phase_ != DependencyLifecyclePhase::worker_ready) {
    return lifecycle_failure(core::ErrorCode::invalid_state,
                             "dependency lifecycle worker cleanup transition is invalid");
  }

  core::Error first_error{core::ErrorDomain::core, core::ErrorCode::invalid_state};
  bool failed{};
  while (worker_initialized_count_ != 0) {
    --worker_initialized_count_;
    const auto result = hooks_[worker_initialized_count_].worker_cleanup(
        hooks_[worker_initialized_count_].context);
    if (!result.has_value() && !failed) {
      first_error = result.error();
      failed = true;
    }
  }
  if (failed) {
    set_terminal_failure(DependencyLifecyclePhase::worker_cleanup_failed, first_error);
    return persistent_failure();
  }
  phase_ = DependencyLifecyclePhase::worker_cleaned;
  return {};
}

core::Result<void> DependencyLifecycleRegistry::master_cleanup() noexcept {
  if (!is_master_process(*this)) {
    return lifecycle_failure(core::ErrorCode::invalid_state,
                             "dependency lifecycle master cleanup requires master owner");
  }
  if (phase_ == DependencyLifecyclePhase::preflight_rollback_failed ||
      phase_ == DependencyLifecyclePhase::master_cleanup_failed) {
    return persistent_failure();
  }
  if (phase_ == DependencyLifecyclePhase::master_cleaned ||
      phase_ == DependencyLifecyclePhase::preflight_failed) {
    return {};
  }
  if (phase_ != DependencyLifecyclePhase::preflighted &&
      phase_ != DependencyLifecyclePhase::fork_ready) {
    return lifecycle_failure(core::ErrorCode::invalid_state,
                             "dependency lifecycle master cleanup transition is invalid");
  }

  core::Error first_error{core::ErrorDomain::core, core::ErrorCode::invalid_state};
  bool failed{};
  while (preflighted_count_ != 0) {
    --preflighted_count_;
    const auto result = hooks_[preflighted_count_].master_cleanup(
        hooks_[preflighted_count_].context);
    if (!result.has_value() && !failed) {
      first_error = result.error();
      failed = true;
    }
  }
  if (failed) {
    set_terminal_failure(DependencyLifecyclePhase::master_cleanup_failed, first_error);
    return persistent_failure();
  }
  phase_ = DependencyLifecyclePhase::master_cleaned;
  return {};
}

}  // namespace laghu::adapters
