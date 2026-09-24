// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <atomic>
#include <cstdint>

#include <laghu/core/contract.hpp>
#include <laghu/core/handles.hpp>

namespace laghu::os {

namespace internal {
class WakeupTestAccess;
}

enum class WakeupMechanism : std::uint8_t {
  event_counter,
  pipe,
};

enum class WakeupNotifyResult : std::uint8_t {
  signaled,
  coalesced,
};

struct WakeupObservation final {
  bool observed;
  bool pending;
};

class WakeupChannel final {
 public:
  WakeupChannel(const WakeupChannel&) = delete;
  WakeupChannel& operator=(const WakeupChannel&) = delete;
  WakeupChannel(WakeupChannel&& other) noexcept;
  WakeupChannel& operator=(WakeupChannel&& other) = delete;
  ~WakeupChannel();

  [[nodiscard]] static core::Result<WakeupChannel> create() noexcept;

  [[nodiscard]] core::Result<WakeupNotifyResult> notify() noexcept;
  [[nodiscard]] core::Result<WakeupObservation> consume() noexcept;
  // The returned descriptor is borrowed and remains valid until close() begins.
  // Callers must unregister it from their event backend before closing the channel.
  [[nodiscard]] core::Result<int> notification_descriptor() const noexcept;
  [[nodiscard]] WakeupMechanism mechanism() const noexcept { return mechanism_; }
  [[nodiscard]] core::Result<void> close() noexcept;

 private:
  friend class internal::WakeupTestAccess;

  class OperationGuard final {
   public:
    explicit OperationGuard(const WakeupChannel& channel) noexcept : channel_(&channel) {}
    OperationGuard(const OperationGuard&) = delete;
    OperationGuard& operator=(const OperationGuard&) = delete;
    ~OperationGuard() { channel_->end_operation(); }

   private:
    const WakeupChannel* channel_;
  };

  WakeupChannel(core::FileHandle&& read_handle, core::FileHandle&& write_handle,
                WakeupMechanism mechanism) noexcept;

  [[nodiscard]] bool begin_operation() const noexcept;
  void end_operation() const noexcept;
  [[nodiscard]] int write_descriptor() const noexcept;

  core::FileHandle read_handle_;
  core::FileHandle write_handle_;
  WakeupMechanism mechanism_;
  std::atomic<bool> pending_{false};
  std::atomic<bool> closing_{false};
  mutable std::atomic<std::uint32_t> active_operations_{0};
};

}  // namespace laghu::os
