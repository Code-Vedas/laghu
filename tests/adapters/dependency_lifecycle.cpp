// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <limits>
#include <type_traits>

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "laghu_test_support.hpp"

#include <laghu/adapters/dependency_lifecycle.hpp>

namespace {

using laghu::adapters::DependencyForkEpoch;
using laghu::adapters::DependencyLifecycleHooks;
using laghu::adapters::DependencyLifecyclePhase;
using laghu::adapters::DependencyLifecycleRegistry;
using laghu::adapters::DependencyLiveState;
using laghu::core::DependencyId;
using laghu::core::Error;
using laghu::core::ErrorCode;
using laghu::core::ErrorDomain;
using laghu::core::Result;

enum class HookStage : std::uint8_t {
  preflight,
  worker_initialize,
  worker_cleanup,
  master_cleanup,
};

[[nodiscard]] constexpr std::uint8_t event_code(HookStage stage,
                                                 std::uint8_t slot) noexcept {
  return static_cast<std::uint8_t>((static_cast<std::uint8_t>(stage) << 4U) | slot);
}

struct Recorder final {
  std::array<std::uint8_t, 64> events{};
  std::size_t event_count{};
  DependencyLiveState live{};
  HookStage failure_stage{HookStage::preflight};
  std::uint8_t failure_slot{std::numeric_limits<std::uint8_t>::max()};
  HookStage additional_failure_stage{HookStage::preflight};
  std::uint8_t additional_failure_slot{std::numeric_limits<std::uint8_t>::max()};
};

struct HookContext final {
  Recorder* recorder{};
  std::uint8_t slot{};
};

struct ChildReport final {
  std::uint64_t process{};
  std::array<std::uint8_t, 32> events{};
  std::uint8_t event_count{};
  std::uint8_t phase{};
  bool initialization_succeeded{};
  bool cleanup_succeeded{};
  bool repeated_cleanup_succeeded{};
  bool cleanup_failure_persisted{};
  bool repeated_initialization_succeeded{};
  bool initialization_failure_persisted{};
};

constexpr std::array dependency_ids{
    DependencyId::openssl,
    DependencyId::yyjson,
    DependencyId::nghttp2,
    DependencyId::ngtcp2,
    DependencyId::nghttp3,
    DependencyId::c_ares,
    DependencyId::pcre2_8bit,
    DependencyId::zlib_ng,
    DependencyId::brotli,
    DependencyId::zstd,
    DependencyId::libmaxminddb,
    DependencyId::libidn2,
    DependencyId::libxcrypt,
    DependencyId::protobuf_c,
};

[[nodiscard]] Result<void> fixture_error(HookStage stage) noexcept {
  const auto native_code = static_cast<std::int32_t>(-71 - static_cast<std::int32_t>(stage));
  return std::unexpected{Error{ErrorDomain::dependency, ErrorCode::dependency, native_code,
                               "dependency lifecycle fixture failure"}};
}

[[nodiscard]] Result<void> run_hook(void* context, HookStage stage) noexcept {
  if (context == nullptr) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::invalid_input, 0,
                                 "dependency lifecycle fixture context is missing"}};
  }
  auto& hook = *static_cast<HookContext*>(context);
  if (hook.recorder == nullptr || hook.recorder->event_count == hook.recorder->events.size()) {
    return std::unexpected{Error{ErrorDomain::core, ErrorCode::exhaustion, 0,
                                 "dependency lifecycle fixture record is exhausted"}};
  }
  hook.recorder->events[hook.recorder->event_count] = event_code(stage, hook.slot);
  ++hook.recorder->event_count;
  if ((hook.recorder->failure_stage == stage && hook.recorder->failure_slot == hook.slot) ||
      (hook.recorder->additional_failure_stage == stage &&
       hook.recorder->additional_failure_slot == hook.slot)) {
    return fixture_error(stage);
  }
  return {};
}

[[nodiscard]] Result<void> preflight(void* context) noexcept {
  return run_hook(context, HookStage::preflight);
}

[[nodiscard]] Result<void> worker_initialize(void* context) noexcept {
  return run_hook(context, HookStage::worker_initialize);
}

[[nodiscard]] Result<void> worker_cleanup(void* context) noexcept {
  return run_hook(context, HookStage::worker_cleanup);
}

[[nodiscard]] Result<void> master_cleanup(void* context) noexcept {
  return run_hook(context, HookStage::master_cleanup);
}

[[nodiscard]] DependencyLiveState live_state(void* context) noexcept {
  if (context == nullptr) {
    return DependencyLiveState{0, 0, 1, 0};
  }
  const auto& hook = *static_cast<const HookContext*>(context);
  return hook.recorder == nullptr ? DependencyLiveState{0, 0, 1, 0}
                                  : hook.recorder->live;
}

[[nodiscard]] DependencyLifecycleHooks make_hooks(DependencyId dependency,
                                                   HookContext& context) noexcept {
  return DependencyLifecycleHooks{dependency, &context, preflight, worker_initialize,
                                  worker_cleanup, master_cleanup, live_state};
}

template <class T>
[[nodiscard]] bool has_error(const Result<T>& result, ErrorCode expected) noexcept {
  return !result.has_value() && result.error().code() == expected;
}

template <class T>
[[nodiscard]] bool has_error(const Result<T>& result, ErrorCode expected,
                             std::int32_t native_code) noexcept {
  return !result.has_value() && result.error().code() == expected &&
         result.error().native_code() == native_code;
}

[[nodiscard]] bool same_error(const Result<void>& left, const Result<void>& right) noexcept {
  return !left.has_value() && !right.has_value() &&
         left.error().domain() == right.error().domain() &&
         left.error().code() == right.error().code() &&
         left.error().native_code() == right.error().native_code() &&
         left.error().diagnostic_context() == right.error().diagnostic_context();
}

[[nodiscard]] bool events_equal(const Recorder& recorder,
                                std::initializer_list<std::uint8_t> expected) noexcept {
  if (recorder.event_count != expected.size()) {
    return false;
  }
  std::size_t index{};
  for (const std::uint8_t event : expected) {
    if (recorder.events[index] != event) {
      return false;
    }
    ++index;
  }
  return true;
}

[[nodiscard]] bool child_events_equal(const ChildReport& report,
                                      std::initializer_list<std::uint8_t> expected) noexcept {
  if (report.event_count != expected.size()) {
    return false;
  }
  std::size_t index{};
  for (const std::uint8_t event : expected) {
    if (report.events[index] != event) {
      return false;
    }
    ++index;
  }
  return true;
}

[[nodiscard]] bool register_dependencies(DependencyLifecycleRegistry& registry,
                                         Recorder& recorder,
                                         std::array<HookContext,
                                                    laghu::adapters::dependency_lifecycle_capacity>& contexts,
                                         std::size_t count) noexcept {
  if (count > contexts.size() || count > dependency_ids.size()) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    contexts[index] = HookContext{&recorder, static_cast<std::uint8_t>(index)};
    if (!registry.register_dependency(make_hooks(dependency_ids[index], contexts[index])).has_value()) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool write_all(int descriptor, const void* input, std::size_t size) noexcept {
  const auto* cursor = static_cast<const std::byte*>(input);
  while (size != 0) {
    const ssize_t written = ::write(descriptor, cursor, size);
    if (written > 0) {
      const auto advanced = static_cast<std::size_t>(written);
      cursor += advanced;
      size -= advanced;
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] bool read_all_bounded(int descriptor, void* output, std::size_t size) noexcept {
  auto* cursor = static_cast<std::byte*>(output);
  constexpr unsigned int maximum_empty_polls = 20;
  unsigned int empty_polls{};
  while (size != 0 && empty_polls < maximum_empty_polls) {
    pollfd poll_descriptor{descriptor, static_cast<short>(POLLIN | POLLHUP), 0};
    const int ready = ::poll(&poll_descriptor, 1, 100);
    if (ready == 0) {
      ++empty_polls;
      continue;
    }
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    const ssize_t read_count = ::read(descriptor, cursor, size);
    if (read_count > 0) {
      const auto advanced = static_cast<std::size_t>(read_count);
      cursor += advanced;
      size -= advanced;
      continue;
    }
    if (read_count < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return size == 0;
}

[[nodiscard]] bool reap_child_bounded(pid_t child, bool& exited_successfully) noexcept {
  int status{};
  constexpr unsigned int wait_attempts = 20;
  for (unsigned int attempt = 0; attempt < wait_attempts; ++attempt) {
    const pid_t waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) {
      exited_successfully = WIFEXITED(status) && WEXITSTATUS(status) == 0;
      return true;
    }
    if (waited < 0 && errno != EINTR) {
      return false;
    }
    if (::poll(nullptr, 0, 50) < 0 && errno != EINTR) {
      return false;
    }
  }
  static_cast<void>(::kill(child, SIGKILL));
  for (;;) {
    const pid_t waited = ::waitpid(child, &status, 0);
    if (waited == child) {
      exited_successfully = false;
      return false;
    }
    if (waited < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
}

[[nodiscard]] bool run_worker_child(DependencyLifecycleRegistry& registry,
                                    DependencyForkEpoch epoch, Recorder& recorder,
                                    ChildReport& report) noexcept {
  std::array<int, 2> descriptors{-1, -1};
  if (::pipe(descriptors.data()) != 0) {
    return false;
  }
  const pid_t child = ::fork();
  if (child < 0) {
    static_cast<void>(::close(descriptors[0]));
    static_cast<void>(::close(descriptors[1]));
    return false;
  }
  if (child == 0) {
    static_cast<void>(::close(descriptors[0]));
    recorder.event_count = 0;
    const auto initialized = registry.worker_initialize(epoch);
    const auto cleaned = registry.worker_cleanup();
    const auto repeated_cleanup = registry.worker_cleanup();
    const auto repeated_initialization = registry.worker_initialize(epoch);
    ChildReport child_report{};
    child_report.process = static_cast<std::uint64_t>(::getpid());
    child_report.initialization_succeeded = initialized.has_value();
    child_report.cleanup_succeeded = cleaned.has_value();
    child_report.repeated_cleanup_succeeded = repeated_cleanup.has_value();
    child_report.cleanup_failure_persisted = same_error(cleaned, repeated_cleanup);
    child_report.repeated_initialization_succeeded = repeated_initialization.has_value();
    child_report.initialization_failure_persisted =
        same_error(initialized, repeated_initialization);
    child_report.phase = static_cast<std::uint8_t>(registry.phase());
    if (recorder.event_count > child_report.events.size()) {
      static_cast<void>(::close(descriptors[1]));
      _exit(1);
    }
    child_report.event_count = static_cast<std::uint8_t>(recorder.event_count);
    for (std::size_t index = 0; index < recorder.event_count; ++index) {
      child_report.events[index] = recorder.events[index];
    }
    const bool wrote = write_all(descriptors[1], &child_report, sizeof(child_report));
    static_cast<void>(::close(descriptors[1]));
    _exit(wrote ? 0 : 1);
  }

  static_cast<void>(::close(descriptors[1]));
  const bool read = read_all_bounded(descriptors[0], &report, sizeof(report));
  static_cast<void>(::close(descriptors[0]));
  bool child_success{};
  const bool reaped = reap_child_bounded(child, child_success);
  return read && reaped && child_success;
}

[[nodiscard]] bool check_registration_and_fork_boundary() noexcept {
  Recorder recorder{};
  std::array<HookContext, laghu::adapters::dependency_lifecycle_capacity> contexts{};
  DependencyLifecycleRegistry registry{};
  if (!has_error(registry.register_dependency({DependencyId::openssl}),
                 ErrorCode::invalid_input) ||
      !has_error(registry.register_dependency(
                     make_hooks(static_cast<DependencyId>(std::numeric_limits<std::uint8_t>::max()),
                                contexts[0])),
                 ErrorCode::invalid_input) ||
      !register_dependencies(registry, recorder, contexts, 1) ||
      !has_error(registry.register_dependency(make_hooks(DependencyId::openssl, contexts[0])),
                 ErrorCode::invalid_state)) {
    return false;
  }

  Recorder capacity_recorder{};
  std::array<HookContext, laghu::adapters::dependency_lifecycle_capacity> capacity_contexts{};
  DependencyLifecycleRegistry capacity_registry{};
  if (!register_dependencies(capacity_registry, capacity_recorder, capacity_contexts,
                             capacity_contexts.size()) ||
      !has_error(capacity_registry.register_dependency(
                     make_hooks(DependencyId::openssl, capacity_contexts[0])),
                 ErrorCode::exhaustion)) {
    return false;
  }

  if (!registry.preflight().has_value() ||
      !events_equal(recorder, {event_code(HookStage::preflight, 0)})) {
    return false;
  }
  recorder.live.sessions = 1;
  if (!has_error(registry.prepare_fork(1), ErrorCode::invalid_state)) {
    return false;
  }
  recorder.live.sessions = 0;
  recorder.live.contexts = 1;
  if (!has_error(registry.prepare_fork(1), ErrorCode::invalid_state)) {
    return false;
  }
  recorder.live.contexts = 0;
  recorder.live.threads = 1;
  if (!has_error(registry.prepare_fork(1), ErrorCode::invalid_state)) {
    return false;
  }
  recorder.live.threads = 0;
  recorder.live.callbacks = 1;
  if (!has_error(registry.prepare_fork(1), ErrorCode::invalid_state)) {
    return false;
  }
  recorder.live.callbacks = 0;
  if (!has_error(registry.prepare_fork(2), ErrorCode::invalid_state)) {
    return false;
  }
  const auto epoch = registry.prepare_fork(1);
  if (!epoch.has_value() || !epoch->valid() ||
      registry.phase() != DependencyLifecyclePhase::fork_ready ||
      !has_error(registry.worker_initialize(*epoch), ErrorCode::invalid_state)) {
    return false;
  }
  if (!has_error(registry.prepare_fork(2), ErrorCode::invalid_state)) {
    return false;
  }
  recorder.live.sessions = 1;
  if (!has_error(registry.prepare_fork(1), ErrorCode::invalid_state)) {
    return false;
  }
  recorder.live.sessions = 0;
  recorder.live.contexts = 1;
  if (!has_error(registry.prepare_fork(1), ErrorCode::invalid_state)) {
    return false;
  }
  recorder.live.contexts = 0;
  recorder.live.threads = 1;
  if (!has_error(registry.prepare_fork(1), ErrorCode::invalid_state)) {
    return false;
  }
  recorder.live.threads = 0;
  recorder.live.callbacks = 1;
  if (!has_error(registry.prepare_fork(1), ErrorCode::invalid_state)) {
    return false;
  }
  recorder.live.callbacks = 0;
  const DependencyForkEpoch wrong_epoch{epoch->master_process, epoch->value + 1};
  if (!has_error(registry.worker_initialize(wrong_epoch), ErrorCode::invalid_state)) {
    return false;
  }
  const auto repeated_epoch = registry.prepare_fork(1);
  if (!repeated_epoch.has_value() || repeated_epoch->value != epoch->value ||
      repeated_epoch->master_process != epoch->master_process) {
    return false;
  }
  if (!registry.master_cleanup().has_value() ||
      !events_equal(recorder, {event_code(HookStage::preflight, 0),
                               event_code(HookStage::master_cleanup, 0)}) ||
      !registry.master_cleanup().has_value() ||
      !has_error(registry.register_dependency(make_hooks(DependencyId::libressl, contexts[0])),
                 ErrorCode::invalid_state)) {
    return false;
  }
  return true;
}

[[nodiscard]] bool check_preflight_failure_rollback() noexcept {
  Recorder recorder{};
  recorder.failure_stage = HookStage::preflight;
  recorder.failure_slot = 1;
  std::array<HookContext, laghu::adapters::dependency_lifecycle_capacity> contexts{};
  DependencyLifecycleRegistry registry{};
  if (!register_dependencies(registry, recorder, contexts, 2) ||
      !has_error(registry.preflight(), ErrorCode::dependency) ||
      registry.phase() != DependencyLifecyclePhase::preflight_failed ||
      !events_equal(recorder, {event_code(HookStage::preflight, 0),
                               event_code(HookStage::preflight, 1),
                               event_code(HookStage::master_cleanup, 0)}) ||
      !registry.master_cleanup().has_value() ||
      !has_error(registry.preflight(), ErrorCode::invalid_state)) {
    return false;
  }
  return true;
}

[[nodiscard]] bool check_preflight_rollback_cleanup_failure() noexcept {
  Recorder recorder{};
  recorder.failure_stage = HookStage::preflight;
  recorder.failure_slot = 1;
  recorder.additional_failure_stage = HookStage::master_cleanup;
  recorder.additional_failure_slot = 0;
  std::array<HookContext, laghu::adapters::dependency_lifecycle_capacity> contexts{};
  DependencyLifecycleRegistry registry{};
  if (!register_dependencies(registry, recorder, contexts, 2)) {
    return false;
  }
  const auto initial = registry.preflight();
  const auto repeated = registry.preflight();
  const auto cleanup = registry.master_cleanup();
  const auto prepared = registry.prepare_fork(1);
  return has_error(initial, ErrorCode::dependency, -74) &&
         has_error(repeated, ErrorCode::dependency, -74) &&
         has_error(cleanup, ErrorCode::dependency, -74) &&
         has_error(prepared, ErrorCode::dependency, -74) &&
         registry.phase() == DependencyLifecyclePhase::preflight_rollback_failed &&
         events_equal(recorder, {event_code(HookStage::preflight, 0),
                                 event_code(HookStage::preflight, 1),
                                 event_code(HookStage::master_cleanup, 0)});
}

[[nodiscard]] bool check_partial_worker_initialization_rollback() noexcept {
  Recorder recorder{};
  std::array<HookContext, laghu::adapters::dependency_lifecycle_capacity> contexts{};
  DependencyLifecycleRegistry registry{};
  if (!register_dependencies(registry, recorder, contexts, 2) ||
      !registry.preflight().has_value()) {
    return false;
  }
  const auto epoch = registry.prepare_fork(1);
  if (!epoch.has_value()) {
    return false;
  }
  recorder.event_count = 0;
  recorder.failure_stage = HookStage::worker_initialize;
  recorder.failure_slot = 1;
  ChildReport report{};
  if (!run_worker_child(registry, *epoch, recorder, report) ||
      report.initialization_succeeded || !report.cleanup_succeeded ||
      !report.repeated_cleanup_succeeded || report.repeated_initialization_succeeded ||
      report.phase != static_cast<std::uint8_t>(DependencyLifecyclePhase::worker_rolled_back) ||
      !child_events_equal(report, {event_code(HookStage::worker_initialize, 0),
                                   event_code(HookStage::worker_initialize, 1),
                                   event_code(HookStage::worker_cleanup, 0)})) {
    return false;
  }
  return registry.master_cleanup().has_value() &&
         events_equal(recorder, {event_code(HookStage::master_cleanup, 1),
                                 event_code(HookStage::master_cleanup, 0)});
}

[[nodiscard]] bool check_worker_rollback_cleanup_failure() noexcept {
  Recorder recorder{};
  recorder.failure_stage = HookStage::worker_initialize;
  recorder.failure_slot = 1;
  recorder.additional_failure_stage = HookStage::worker_cleanup;
  recorder.additional_failure_slot = 0;
  std::array<HookContext, laghu::adapters::dependency_lifecycle_capacity> contexts{};
  DependencyLifecycleRegistry registry{};
  if (!register_dependencies(registry, recorder, contexts, 2) ||
      !registry.preflight().has_value()) {
    return false;
  }
  const auto epoch = registry.prepare_fork(1);
  if (!epoch.has_value()) {
    return false;
  }
  recorder.event_count = 0;
  ChildReport report{};
  if (!run_worker_child(registry, *epoch, recorder, report) ||
      report.initialization_succeeded || report.cleanup_succeeded ||
      report.repeated_cleanup_succeeded || !report.cleanup_failure_persisted ||
      report.repeated_initialization_succeeded || !report.initialization_failure_persisted ||
      report.phase != static_cast<std::uint8_t>(
                          DependencyLifecyclePhase::worker_rollback_failed) ||
      !child_events_equal(report, {event_code(HookStage::worker_initialize, 0),
                                   event_code(HookStage::worker_initialize, 1),
                                   event_code(HookStage::worker_cleanup, 0)})) {
    return false;
  }
  return registry.master_cleanup().has_value() &&
         events_equal(recorder, {event_code(HookStage::master_cleanup, 1),
                                 event_code(HookStage::master_cleanup, 0)});
}

[[nodiscard]] bool check_worker_cleanup_failure_continues() noexcept {
  Recorder recorder{};
  std::array<HookContext, laghu::adapters::dependency_lifecycle_capacity> contexts{};
  DependencyLifecycleRegistry registry{};
  if (!register_dependencies(registry, recorder, contexts, 2) ||
      !registry.preflight().has_value()) {
    return false;
  }
  const auto epoch = registry.prepare_fork(1);
  if (!epoch.has_value()) {
    return false;
  }
  recorder.event_count = 0;
  recorder.failure_stage = HookStage::worker_cleanup;
  recorder.failure_slot = 1;
  ChildReport report{};
  if (!run_worker_child(registry, *epoch, recorder, report) ||
      !report.initialization_succeeded || report.cleanup_succeeded ||
      report.repeated_cleanup_succeeded || !report.cleanup_failure_persisted ||
      report.repeated_initialization_succeeded ||
      report.phase != static_cast<std::uint8_t>(
                          DependencyLifecyclePhase::worker_cleanup_failed) ||
      !child_events_equal(report, {event_code(HookStage::worker_initialize, 0),
                                   event_code(HookStage::worker_initialize, 1),
                                   event_code(HookStage::worker_cleanup, 1),
                                   event_code(HookStage::worker_cleanup, 0)})) {
    return false;
  }
  return registry.master_cleanup().has_value() &&
         events_equal(recorder, {event_code(HookStage::master_cleanup, 1),
                                 event_code(HookStage::master_cleanup, 0)});
}

[[nodiscard]] bool check_master_cleanup_failure() noexcept {
  Recorder recorder{};
  recorder.failure_stage = HookStage::master_cleanup;
  recorder.failure_slot = 1;
  std::array<HookContext, laghu::adapters::dependency_lifecycle_capacity> contexts{};
  DependencyLifecycleRegistry registry{};
  if (!register_dependencies(registry, recorder, contexts, 2) ||
      !registry.preflight().has_value() || !registry.prepare_fork(1).has_value()) {
    return false;
  }
  recorder.event_count = 0;
  const auto initial = registry.master_cleanup();
  const auto repeated = registry.master_cleanup();
  const auto prepared = registry.prepare_fork(1);
  return has_error(initial, ErrorCode::dependency, -74) &&
         has_error(repeated, ErrorCode::dependency, -74) &&
         has_error(prepared, ErrorCode::dependency, -74) &&
         registry.phase() == DependencyLifecyclePhase::master_cleanup_failed &&
         events_equal(recorder, {event_code(HookStage::master_cleanup, 1),
                                 event_code(HookStage::master_cleanup, 0)});
}

[[nodiscard]] bool check_independent_child_initialization_and_cleanup_order() noexcept {
  Recorder recorder{};
  std::array<HookContext, laghu::adapters::dependency_lifecycle_capacity> contexts{};
  DependencyLifecycleRegistry registry{};
  if (!register_dependencies(registry, recorder, contexts, 2) ||
      !registry.preflight().has_value()) {
    return false;
  }
  const auto epoch = registry.prepare_fork(1);
  if (!epoch.has_value()) {
    return false;
  }
  recorder.event_count = 0;
  ChildReport first{};
  ChildReport second{};
  if (!run_worker_child(registry, *epoch, recorder, first) ||
      !run_worker_child(registry, *epoch, recorder, second) ||
      !first.initialization_succeeded || !first.cleanup_succeeded ||
      !second.initialization_succeeded || !second.cleanup_succeeded ||
      !first.repeated_cleanup_succeeded || !second.repeated_cleanup_succeeded ||
      first.repeated_initialization_succeeded || second.repeated_initialization_succeeded ||
      first.process == 0 || second.process == 0 || first.process == second.process ||
      first.phase != static_cast<std::uint8_t>(DependencyLifecyclePhase::worker_cleaned) ||
      second.phase != static_cast<std::uint8_t>(DependencyLifecyclePhase::worker_cleaned) ||
      !child_events_equal(first, {event_code(HookStage::worker_initialize, 0),
                                  event_code(HookStage::worker_initialize, 1),
                                  event_code(HookStage::worker_cleanup, 1),
                                  event_code(HookStage::worker_cleanup, 0)}) ||
      !child_events_equal(second, {event_code(HookStage::worker_initialize, 0),
                                   event_code(HookStage::worker_initialize, 1),
                                   event_code(HookStage::worker_cleanup, 1),
                                   event_code(HookStage::worker_cleanup, 0)})) {
    return false;
  }
  return registry.master_cleanup().has_value() &&
         events_equal(recorder, {event_code(HookStage::master_cleanup, 1),
                                 event_code(HookStage::master_cleanup, 0)});
}

static_assert(dependency_ids.size() == laghu::adapters::dependency_lifecycle_capacity);
static_assert(std::is_trivially_copyable_v<DependencyLifecycleHooks>);
static_assert(std::is_standard_layout_v<DependencyForkEpoch>);

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"adapters.dependency_lifecycle.registration",
                            check_registration_and_fork_boundary},
      laghu::test::TestCase{"adapters.dependency_lifecycle.preflight_rollback",
                            check_preflight_failure_rollback},
      laghu::test::TestCase{"adapters.dependency_lifecycle.preflight_rollback_cleanup_failure",
                            check_preflight_rollback_cleanup_failure},
      laghu::test::TestCase{"adapters.dependency_lifecycle.worker_rollback",
                            check_partial_worker_initialization_rollback},
      laghu::test::TestCase{"adapters.dependency_lifecycle.worker_rollback_cleanup_failure",
                            check_worker_rollback_cleanup_failure},
      laghu::test::TestCase{"adapters.dependency_lifecycle.worker_cleanup_failure",
                            check_worker_cleanup_failure_continues},
      laghu::test::TestCase{"adapters.dependency_lifecycle.master_cleanup_failure",
                            check_master_cleanup_failure},
      laghu::test::TestCase{"adapters.dependency_lifecycle.fork_children",
                            check_independent_child_initialization_and_cleanup_order},
  };
  return laghu::test::run_tests(tests);
}
