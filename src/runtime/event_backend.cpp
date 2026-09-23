// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/runtime/event_backend.hpp>

namespace laghu::runtime {
namespace {

[[nodiscard]] core::Error invalid_backend(std::string_view diagnostic) noexcept {
  return core::Error{core::ErrorDomain::core, core::ErrorCode::invalid_input, 0,
                     diagnostic};
}

[[nodiscard]] core::Result<void> validate_interests(EventInterests interests) noexcept {
  if (interests.empty()) {
    return std::unexpected{invalid_backend("event interests must not be empty")};
  }
  return {};
}

}  // namespace

core::Result<EventBackend> EventBackend::create(
    void* context, const EventBackendOperations& operations) noexcept {
  if (context == nullptr) {
    return std::unexpected{invalid_backend("event backend context must not be null")};
  }
  if (operations.register_source == nullptr || operations.modify_source == nullptr ||
      operations.remove_source == nullptr || operations.wait == nullptr ||
      operations.capabilities == nullptr) {
    return std::unexpected{invalid_backend("event backend operation must not be null")};
  }
  return EventBackend{context, operations};
}

core::Result<void> EventBackend::register_source(
    EventSource source, EventToken token, EventInterests interests) const noexcept {
  const auto valid = validate_interests(interests);
  if (!valid) {
    return std::unexpected{valid.error()};
  }
  return operations_.register_source(context_, source, token, interests);
}

core::Result<void> EventBackend::modify_source(
    EventSource source, EventToken token, EventInterests interests) const noexcept {
  const auto valid = validate_interests(interests);
  if (!valid) {
    return std::unexpected{valid.error()};
  }
  return operations_.modify_source(context_, source, token, interests);
}

core::Result<void> EventBackend::remove_source(
    EventSource source, EventToken token) const noexcept {
  return operations_.remove_source(context_, source, token);
}

core::Result<EventWaitResult> EventBackend::wait(
    std::span<Event> output, std::chrono::nanoseconds maximum_wait) const noexcept {
  if (output.empty()) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::invalid_range, 0,
                                       "event output capacity must be nonzero"}};
  }
  if (maximum_wait.count() < 0) {
    return std::unexpected{invalid_backend("event wait duration must be nonnegative")};
  }
  auto result = operations_.wait(context_, output, maximum_wait);
  if (result && result->event_count > output.size()) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::overflow, 0,
                                       "event backend exceeded output capacity"}};
  }
  return result;
}

core::Result<EventBackendCapabilities> EventBackend::capabilities() const noexcept {
  return operations_.capabilities(context_);
}

}  // namespace laghu::runtime
