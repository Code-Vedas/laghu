// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include <laghu/core/contract.hpp>
#include <laghu/core/handles.hpp>

namespace laghu::os::internal {

enum class EpollInterest : std::uint8_t {
  readable = 1U << 0U,
  writable = 1U << 1U,
};

class EpollInterests final {
 public:
  explicit constexpr EpollInterests(EpollInterest interest) noexcept
      : bits_(static_cast<std::uint8_t>(interest)) {}
  constexpr EpollInterests& add(EpollInterest interest) noexcept {
    bits_ |= static_cast<std::uint8_t>(interest);
    return *this;
  }
  [[nodiscard]] constexpr bool contains(EpollInterest interest) const noexcept {
    return (bits_ & static_cast<std::uint8_t>(interest)) != 0U;
  }

 private:
  std::uint8_t bits_;
};

enum class EpollNotification : std::uint8_t {
  readable = 1U << 0U,
  writable = 1U << 1U,
  error = 1U << 2U,
  hangup = 1U << 3U,
};

class EpollNotifications final {
 public:
  constexpr EpollNotifications() noexcept = default;
  constexpr EpollNotifications& add(EpollNotification notification) noexcept {
    bits_ |= static_cast<std::uint8_t>(notification);
    return *this;
  }
  [[nodiscard]] constexpr bool contains(EpollNotification notification) const noexcept {
    return (bits_ & static_cast<std::uint8_t>(notification)) != 0U;
  }

 private:
  std::uint8_t bits_{};
};

struct EpollEvent final {
  std::uint64_t token;
  EpollNotifications notifications;
};

struct EpollWaitResult final {
  std::size_t event_count;
  std::uint32_t wait_calls;
  bool saturated;
};

class EpollDispatcher final {
 public:
  static constexpr std::size_t maximum_events = 256;

  EpollDispatcher() noexcept = default;
  EpollDispatcher(const EpollDispatcher&) = delete;
  EpollDispatcher& operator=(const EpollDispatcher&) = delete;
  EpollDispatcher(EpollDispatcher&&) noexcept = default;
  EpollDispatcher& operator=(EpollDispatcher&&) noexcept = default;
  ~EpollDispatcher() = default;

  [[nodiscard]] static core::Result<EpollDispatcher> create() noexcept;
  [[nodiscard]] core::Result<void> add(int descriptor, std::uint64_t token,
                                       EpollInterests interests) const noexcept;
  [[nodiscard]] core::Result<void> modify(int descriptor, std::uint64_t token,
                                          EpollInterests interests) const noexcept;
  [[nodiscard]] core::Result<void> remove(int descriptor) const noexcept;
  [[nodiscard]] core::Result<EpollWaitResult> wait(
      std::span<EpollEvent> output, std::chrono::nanoseconds maximum_wait,
      std::uint32_t maximum_wait_calls) const noexcept;

 private:
  explicit EpollDispatcher(core::FileHandle&& descriptor) noexcept
      : descriptor_(std::move(descriptor)) {}

  core::FileHandle descriptor_{};
};

}  // namespace laghu::os::internal
