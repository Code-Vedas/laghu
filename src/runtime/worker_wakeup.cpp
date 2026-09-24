// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/runtime/worker_wakeup.hpp>

#include <new>
#include <utility>

#include <laghu/os/wakeup.hpp>

struct laghu::runtime::WorkerWakeup::State final {
  explicit State(os::WakeupChannel&& value) noexcept : channel(std::move(value)) {}

  os::WakeupChannel channel;
};

namespace {

[[nodiscard]] laghu::core::Error empty_wakeup_error() noexcept {
  return laghu::core::Error{laghu::core::ErrorDomain::core,
                            laghu::core::ErrorCode::invalid_state, 0,
                            "worker wakeup has no state"};
}

}  // namespace

laghu::runtime::WorkerWakeup::WorkerWakeup(WorkerWakeup&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}

laghu::runtime::WorkerWakeup& laghu::runtime::WorkerWakeup::operator=(
    WorkerWakeup&& other) noexcept {
  if (this != &other) {
    destroy();
    state_ = std::exchange(other.state_, nullptr);
  }
  return *this;
}

laghu::runtime::WorkerWakeup::~WorkerWakeup() { destroy(); }

laghu::core::Result<laghu::runtime::WorkerWakeup>
laghu::runtime::WorkerWakeup::create() noexcept {
  auto channel = os::WakeupChannel::create();
  if (!channel) {
    return std::unexpected{channel.error()};
  }
  State* state = new (std::nothrow) State{std::move(*channel)};
  if (state == nullptr) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::exhaustion, 0,
                                       "worker wakeup state allocation failed"}};
  }
  return WorkerWakeup{state};
}

laghu::core::Result<laghu::runtime::WorkerWakeupNotifyResult>
laghu::runtime::WorkerWakeup::notify() noexcept {
  if (state_ == nullptr) {
    return std::unexpected{empty_wakeup_error()};
  }
  const auto result = state_->channel.notify();
  if (!result) {
    return std::unexpected{result.error()};
  }
  return *result == os::WakeupNotifyResult::signaled
             ? WorkerWakeupNotifyResult::signaled
             : WorkerWakeupNotifyResult::coalesced;
}

laghu::core::Result<laghu::runtime::WorkerWakeupObservation>
laghu::runtime::WorkerWakeup::consume() noexcept {
  if (state_ == nullptr) {
    return std::unexpected{empty_wakeup_error()};
  }
  const auto result = state_->channel.consume();
  if (!result) {
    return std::unexpected{result.error()};
  }
  return WorkerWakeupObservation{result->observed, result->pending};
}

laghu::core::Result<laghu::runtime::EventSource>
laghu::runtime::WorkerWakeup::event_source() const noexcept {
  if (state_ == nullptr) {
    return std::unexpected{empty_wakeup_error()};
  }
  const auto descriptor = state_->channel.notification_descriptor();
  if (!descriptor) {
    return std::unexpected{descriptor.error()};
  }
  return EventSource::from_native_handle(*descriptor);
}

laghu::core::Result<void> laghu::runtime::WorkerWakeup::close() noexcept {
  if (state_ == nullptr) {
    return {};
  }
  return state_->channel.close();
}

void laghu::runtime::WorkerWakeup::destroy() noexcept {
  if (state_ != nullptr) {
    static_cast<void>(state_->channel.close());
    delete state_;
    state_ = nullptr;
  }
}
