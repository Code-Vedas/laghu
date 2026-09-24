// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>

#include <laghu/core/contract.hpp>
#include <laghu/runtime/event_backend.hpp>

namespace laghu::runtime {

enum class WorkerWakeupNotifyResult : std::uint8_t {
  signaled,
  coalesced,
};

struct WorkerWakeupObservation final {
  bool observed;
  bool pending;
};

class WorkerWakeup final {
 public:
  WorkerWakeup() noexcept = default;
  WorkerWakeup(const WorkerWakeup&) = delete;
  WorkerWakeup& operator=(const WorkerWakeup&) = delete;
  WorkerWakeup(WorkerWakeup&& other) noexcept;
  WorkerWakeup& operator=(WorkerWakeup&& other) noexcept;
  ~WorkerWakeup();

  [[nodiscard]] static core::Result<WorkerWakeup> create() noexcept;

  [[nodiscard]] core::Result<WorkerWakeupNotifyResult> notify() noexcept;
  [[nodiscard]] core::Result<WorkerWakeupObservation> consume() noexcept;
  // The event source borrows the wakeup descriptor and remains valid until
  // close() begins. Unregister it from the event backend before closing.
  [[nodiscard]] core::Result<EventSource> event_source() const noexcept;
  [[nodiscard]] core::Result<void> close() noexcept;

 private:
  struct State;

  explicit WorkerWakeup(State* state) noexcept : state_(state) {}
  void destroy() noexcept;

  State* state_{nullptr};
};

}  // namespace laghu::runtime
