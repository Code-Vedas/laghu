// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

#include <laghu/core/contract.hpp>

namespace laghu::runtime {

class EventSource final {
 public:
  [[nodiscard]] static constexpr core::Result<EventSource> from_native_handle(
      int handle) noexcept {
    if (handle < 0) {
      return std::unexpected{core::Error{core::ErrorDomain::core,
                                         core::ErrorCode::invalid_input, 0,
                                         "event source handle must be nonnegative"}};
    }
    return EventSource{handle};
  }

  [[nodiscard]] constexpr int native_handle() const noexcept { return handle_; }

 private:
  explicit constexpr EventSource(int handle) noexcept : handle_(handle) {}

  int handle_;
};

class EventToken final {
 public:
  [[nodiscard]] static constexpr core::Result<EventToken> from_uint64(
      std::uint64_t value) noexcept {
    if (value == 0) {
      return std::unexpected{core::Error{core::ErrorDomain::core,
                                         core::ErrorCode::invalid_input, 0,
                                         "event token must be nonzero"}};
    }
    return EventToken{value};
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

 private:
  explicit constexpr EventToken(std::uint64_t value) noexcept : value_(value) {}

  std::uint64_t value_;
};

enum class EventInterest : std::uint8_t {
  readable = 1U << 0U,
  writable = 1U << 1U,
};

class EventInterests final {
 public:
  constexpr EventInterests() noexcept = default;
  explicit constexpr EventInterests(EventInterest interest) noexcept
      : bits_(static_cast<std::uint8_t>(interest)) {}

  constexpr EventInterests& add(EventInterest interest) noexcept {
    bits_ |= static_cast<std::uint8_t>(interest);
    return *this;
  }

  [[nodiscard]] constexpr bool contains(EventInterest interest) const noexcept {
    return (bits_ & static_cast<std::uint8_t>(interest)) != 0;
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }

 private:
  std::uint8_t bits_{0};
};

enum class EventNotification : std::uint8_t {
  readable = 1U << 0U,
  writable = 1U << 1U,
  error = 1U << 2U,
  hangup = 1U << 3U,
  wakeup = 1U << 4U,
  completion = 1U << 5U,
};

class EventNotifications final {
 public:
  explicit constexpr EventNotifications(EventNotification notification) noexcept
      : bits_(static_cast<std::uint8_t>(notification)) {}

  constexpr EventNotifications& add(EventNotification notification) noexcept {
    bits_ |= static_cast<std::uint8_t>(notification);
    return *this;
  }

  [[nodiscard]] constexpr bool contains(EventNotification notification) const noexcept {
    return (bits_ & static_cast<std::uint8_t>(notification)) != 0;
  }

 private:
  std::uint8_t bits_;
};

class Event final {
 public:
  constexpr Event(EventToken token, EventNotifications notifications) noexcept
      : token_(token), notifications_(notifications) {}

  [[nodiscard]] constexpr EventToken token() const noexcept { return token_; }
  [[nodiscard]] constexpr EventNotifications notifications() const noexcept {
    return notifications_;
  }

 private:
  EventToken token_;
  EventNotifications notifications_;
};

struct EventBackendCapabilities final {
  bool readiness;
  bool wakeups;
  bool completions;
};

using RegisterEventSource = core::Result<void> (*)(
    void* context, EventSource source, EventToken token,
    EventInterests interests) noexcept;
using ModifyEventSource = core::Result<void> (*)(
    void* context, EventSource source, EventToken token,
    EventInterests interests) noexcept;
using RemoveEventSource = core::Result<void> (*)(
    void* context, EventSource source, EventToken token) noexcept;
using WaitForEvents = core::Result<std::size_t> (*)(
    void* context, std::span<Event> output,
    std::chrono::nanoseconds maximum_wait) noexcept;
using QueryEventBackendCapabilities = core::Result<EventBackendCapabilities> (*)(
    void* context) noexcept;

struct EventBackendOperations final {
  RegisterEventSource register_source{};
  ModifyEventSource modify_source{};
  RemoveEventSource remove_source{};
  WaitForEvents wait{};
  QueryEventBackendCapabilities capabilities{};
};

class EventBackend final {
 public:
  EventBackend(const EventBackend&) = delete;
  EventBackend& operator=(const EventBackend&) = delete;
  EventBackend(EventBackend&&) noexcept = default;
  EventBackend& operator=(EventBackend&&) noexcept = default;
  ~EventBackend() = default;

  [[nodiscard]] static core::Result<EventBackend> create(
      void* context, const EventBackendOperations& operations) noexcept;

  [[nodiscard]] core::Result<void> register_source(
      EventSource source, EventToken token, EventInterests interests) const noexcept;
  [[nodiscard]] core::Result<void> modify_source(
      EventSource source, EventToken token, EventInterests interests) const noexcept;
  [[nodiscard]] core::Result<void> remove_source(
      EventSource source, EventToken token) const noexcept;
  [[nodiscard]] core::Result<std::size_t> wait(
      std::span<Event> output, std::chrono::nanoseconds maximum_wait) const noexcept;
  [[nodiscard]] core::Result<EventBackendCapabilities> capabilities() const noexcept;

 private:
  constexpr EventBackend(void* context, const EventBackendOperations& operations) noexcept
      : context_(context), operations_(operations) {}

  void* context_;
  EventBackendOperations operations_;
};

}  // namespace laghu::runtime
