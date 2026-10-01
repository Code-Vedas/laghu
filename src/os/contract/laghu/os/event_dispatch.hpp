// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include <laghu/core/contract.hpp>
#include <laghu/core/handles.hpp>

namespace laghu::os {

enum class EventInterest : std::uint8_t {
  readable = 1U << 0U,
  writable = 1U << 1U,
};

class EventInterests final {
 public:
  explicit constexpr EventInterests(EventInterest interest) noexcept
      : bits_(static_cast<std::uint8_t>(interest)) {}
  constexpr EventInterests& add(EventInterest interest) noexcept {
    bits_ |= static_cast<std::uint8_t>(interest);
    return *this;
  }
  [[nodiscard]] constexpr bool contains(EventInterest interest) const noexcept {
    return (bits_ & static_cast<std::uint8_t>(interest)) != 0U;
  }

 private:
  std::uint8_t bits_;
};

enum class EventNotification : std::uint8_t {
  readable = 1U << 0U,
  writable = 1U << 1U,
  error = 1U << 2U,
  hangup = 1U << 3U,
};

class EventNotifications final {
 public:
  constexpr EventNotifications() noexcept = default;
  constexpr EventNotifications& add(EventNotification notification) noexcept {
    bits_ |= static_cast<std::uint8_t>(notification);
    return *this;
  }
  [[nodiscard]] constexpr bool contains(EventNotification notification) const noexcept {
    return (bits_ & static_cast<std::uint8_t>(notification)) != 0U;
  }

 private:
  std::uint8_t bits_{};
};

struct Event final {
  std::uint64_t token;
  EventNotifications notifications;
};

struct EventWaitResult final {
  std::size_t event_count;
  std::uint32_t wait_calls;
  bool saturated;
};

class EventRegistration final {
 public:
  constexpr EventRegistration() noexcept = default;

 private:
  friend class EventDispatcher;

  [[nodiscard]] constexpr bool available() const noexcept {
    return descriptor_ < 0;
  }
  [[nodiscard]] constexpr bool descriptor_matches(int descriptor) const noexcept {
    return descriptor_ == descriptor;
  }
  [[nodiscard]] constexpr bool token_matches(std::uint64_t token) const noexcept {
    return token_ == token;
  }
  constexpr void assign(int descriptor, std::uint64_t token) noexcept {
    descriptor_ = descriptor;
    token_ = token;
  }
  constexpr void clear() noexcept {
    descriptor_ = -1;
    token_ = 0U;
  }

  int descriptor_{-1};
  std::uint64_t token_{};
};

class EventDispatcher final {
 public:
  static constexpr std::size_t maximum_events = 256;

  EventDispatcher(const EventDispatcher&) = delete;
  EventDispatcher& operator=(const EventDispatcher&) = delete;
  EventDispatcher(EventDispatcher&&) noexcept = default;
  EventDispatcher& operator=(EventDispatcher&&) noexcept = default;
  ~EventDispatcher() = default;

  // The dispatcher is worker-affine and not thread-safe. Registration storage
  // is caller-owned, exclusively borrowed, and must outlive the dispatcher.
  // Its size is the registration-capacity bound.
  [[nodiscard]] static core::Result<EventDispatcher> create(
      std::span<EventRegistration> registrations) noexcept;
  [[nodiscard]] core::Result<void> add(
      int descriptor, std::uint64_t token, EventInterests interests) noexcept;
  [[nodiscard]] core::Result<void> modify(
      int descriptor, std::uint64_t expected_token,
      EventInterests interests) noexcept;
  [[nodiscard]] core::Result<void> remove(
      int descriptor, std::uint64_t expected_token) noexcept;
  [[nodiscard]] core::Result<EventWaitResult> wait(
      std::span<Event> output, std::chrono::nanoseconds maximum_wait,
      std::uint32_t maximum_wait_calls) const noexcept;

 private:
  EventDispatcher(core::FileHandle&& descriptor,
                  std::span<EventRegistration> registrations) noexcept
      : descriptor_(std::move(descriptor)), registrations_(registrations) {}

  core::FileHandle descriptor_{};
  std::span<EventRegistration> registrations_{};
};

}  // namespace laghu::os
