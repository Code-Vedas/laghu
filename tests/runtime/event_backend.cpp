// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

#include "laghu_test_support.hpp"

#include <laghu/runtime/event_backend.hpp>

namespace {

using laghu::core::Error;
using laghu::core::ErrorCode;
using laghu::core::ErrorDomain;
using laghu::core::Result;
using laghu::runtime::Event;
using laghu::runtime::EventBackend;
using laghu::runtime::EventBackendCapabilities;
using laghu::runtime::EventBackendOperations;
using laghu::runtime::EventInterest;
using laghu::runtime::EventInterests;
using laghu::runtime::EventNotification;
using laghu::runtime::EventNotifications;
using laghu::runtime::EventSource;
using laghu::runtime::EventToken;
using laghu::runtime::EventWaitResult;

struct Registration final {
  int source;
  std::uint64_t token;
  EventInterests interests;
};

struct FakeBackend final {
  std::array<Registration, 2> registrations;
  std::size_t size{0};
  bool overproduce{false};
};

[[nodiscard]] Result<void> fail(ErrorCode code, std::string_view diagnostic) noexcept {
  return std::unexpected{Error{ErrorDomain::core, code, 0, diagnostic}};
}

[[nodiscard]] std::size_t find_registration(const FakeBackend& backend,
                                            EventSource source,
                                            EventToken token) noexcept {
  for (std::size_t index = 0; index < backend.size; ++index) {
    if (backend.registrations[index].source == source.native_handle() &&
        backend.registrations[index].token == token.value()) {
      return index;
    }
  }
  return backend.size;
}

[[nodiscard]] Result<void> register_source(void* context, EventSource source,
                                           EventToken token,
                                           EventInterests interests) noexcept {
  auto& backend = *static_cast<FakeBackend*>(context);
  if (find_registration(backend, source, token) != backend.size) {
    return fail(ErrorCode::invalid_state, "duplicate fake registration");
  }
  if (backend.size == backend.registrations.size()) {
    return fail(ErrorCode::exhaustion, "fake registration capacity exhausted");
  }
  backend.registrations[backend.size] = Registration{source.native_handle(), token.value(), interests};
  ++backend.size;
  return {};
}

[[nodiscard]] Result<void> modify_source(void* context, EventSource source,
                                         EventToken token,
                                         EventInterests interests) noexcept {
  auto& backend = *static_cast<FakeBackend*>(context);
  const std::size_t index = find_registration(backend, source, token);
  if (index == backend.size) {
    return fail(ErrorCode::invalid_state, "missing fake registration");
  }
  backend.registrations[index].interests = interests;
  return {};
}

[[nodiscard]] Result<void> remove_source(void* context, EventSource source,
                                         EventToken token) noexcept {
  auto& backend = *static_cast<FakeBackend*>(context);
  const std::size_t index = find_registration(backend, source, token);
  if (index == backend.size) {
    return fail(ErrorCode::invalid_state, "missing fake registration");
  }
  --backend.size;
  if (index != backend.size) {
    backend.registrations[index] = backend.registrations[backend.size];
  }
  return {};
}

[[nodiscard]] Result<EventWaitResult> wait(void* context, std::span<Event> output,
                                           std::chrono::nanoseconds) noexcept {
  auto& backend = *static_cast<FakeBackend*>(context);
  if (backend.overproduce) {
    return EventWaitResult{output.size() + 1, true};
  }
  if (backend.size == 0 || output.empty()) {
    return EventWaitResult{0, false};
  }
  const auto token = EventToken::from_uint64(backend.registrations[0].token);
  if (!token) {
    return std::unexpected{token.error()};
  }
  EventNotifications notifications{EventNotification::readable};
  notifications.add(EventNotification::writable)
      .add(EventNotification::error)
      .add(EventNotification::hangup)
      .add(EventNotification::wakeup)
      .add(EventNotification::completion);
  output[0] = Event{*token, notifications};
  return EventWaitResult{1, false};
}

[[nodiscard]] Result<EventBackendCapabilities> capabilities(void*) noexcept {
  return EventBackendCapabilities{true, true, true};
}

constexpr EventBackendOperations operations{
    register_source, modify_source, remove_source, wait, capabilities};

[[nodiscard]] bool check_lifecycle() noexcept {
  FakeBackend state{};
  auto backend = EventBackend::create(&state, operations);
  const auto source = EventSource::from_native_handle(7);
  const auto token = EventToken::from_uint64(41);
  if (!backend || !source || !token) {
    return false;
  }

  EventInterests readable{EventInterest::readable};
  if (!backend->register_source(*source, *token, readable) || state.size != 1 ||
      !state.registrations[0].interests.contains(EventInterest::readable)) {
    return false;
  }
  const auto duplicate = backend->register_source(*source, *token, readable);
  EventInterests writable{EventInterest::writable};
  if (duplicate || duplicate.error().code() != ErrorCode::invalid_state ||
      !backend->modify_source(*source, *token, writable) ||
      !state.registrations[0].interests.contains(EventInterest::writable)) {
    return false;
  }

  std::array<Event, 1> events{Event{*token, EventNotifications{EventNotification::error}}};
  const auto count = backend->wait(events, std::chrono::milliseconds{1});
  const auto caps = backend->capabilities();
  if (!count || count->event_count != 1 || count->saturated ||
      events[0].token().value() != token->value() || !caps ||
      !caps->readiness || !caps->wakeups || !caps->completions) {
    return false;
  }
  for (const auto notification : {EventNotification::readable, EventNotification::writable,
                                  EventNotification::error, EventNotification::hangup,
                                  EventNotification::wakeup, EventNotification::completion}) {
    if (!events[0].notifications().contains(notification)) {
      return false;
    }
  }

  if (!backend->remove_source(*source, *token) || state.size != 0) {
    return false;
  }
  const auto missing = backend->remove_source(*source, *token);
  return !missing && missing.error().code() == ErrorCode::invalid_state;
}

[[nodiscard]] bool check_contract_bounds() noexcept {
  FakeBackend state{};
  auto backend = EventBackend::create(&state, operations);
  const auto source = EventSource::from_native_handle(9);
  const auto token = EventToken::from_uint64(99);
  if (!backend || !source || !token) {
    return false;
  }

  const auto empty_interests = backend->register_source(*source, *token, EventInterests{});
  std::span<Event> empty_output;
  const auto empty_wait = backend->wait(empty_output, std::chrono::nanoseconds{0});
  std::array<Event, 1> events{Event{*token, EventNotifications{EventNotification::error}}};
  const auto negative_wait = backend->wait(events, std::chrono::nanoseconds{-1});
  state.overproduce = true;
  const auto overflow = backend->wait(events, std::chrono::nanoseconds{0});
  EventBackendOperations incomplete = operations;
  incomplete.wait = nullptr;
  const auto invalid_backend = EventBackend::create(&state, incomplete);
  EventBackendOperations default_operations;
  const auto default_backend = EventBackend::create(&state, default_operations);

  return !EventSource::from_native_handle(-1) && !EventToken::from_uint64(0) &&
         !empty_interests && empty_interests.error().code() == ErrorCode::invalid_input &&
         !empty_wait && empty_wait.error().code() == ErrorCode::invalid_range &&
         !negative_wait && negative_wait.error().code() == ErrorCode::invalid_input &&
         !overflow && overflow.error().code() == ErrorCode::overflow &&
         !invalid_backend && invalid_backend.error().code() == ErrorCode::invalid_input &&
         !default_backend && default_backend.error().code() == ErrorCode::invalid_input;
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"runtime.event_backend.lifecycle", check_lifecycle},
      laghu::test::TestCase{"runtime.event_backend.bounds", check_contract_bounds},
  };
  return laghu::test::run_tests(tests);
}
