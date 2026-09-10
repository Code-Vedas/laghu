// SPDX-License-Identifier: AGPL-3.0-only
#include "laghu_test_faults.hpp"

#include <cerrno>
#include <limits>

namespace laghu::test {
namespace {

[[nodiscard]] bool is_allocation_point(FailurePoint point) noexcept {
  return point == FailurePoint::allocation_buffer_acquire;
}

[[nodiscard]] bool is_io_point(FailurePoint point) noexcept {
  return point == FailurePoint::os_read || point == FailurePoint::os_write;
}

[[nodiscard]] int fault_close(void* context, int descriptor) noexcept {
  auto& operations = *static_cast<FaultInjectedDescriptorOperations*>(context);
  if (operations.plan == nullptr || operations.underlying == nullptr ||
      operations.underlying->close == nullptr) {
    errno = EINVAL;
    return -1;
  }
  const FailureAction action = operations.plan->next(FailurePoint::descriptor_close);
  if (action.kind == FailureActionKind::errno_failure) {
    errno = action.native_error;
    return -1;
  }
  return operations.underlying->close(operations.underlying->context, descriptor);
}

[[nodiscard]] laghu::core::Result<laghu::core::MutableByteView> fault_acquire(
    void* context, std::size_t capacity) noexcept {
  auto& source = *static_cast<FaultInjectedBufferSource*>(context);
  if (source.plan == nullptr || source.acquire == nullptr) {
    return std::unexpected{laghu::core::Error{laghu::core::ErrorDomain::core,
                                               laghu::core::ErrorCode::invalid_state, 0,
                                               "test block source is incomplete"}};
  }
  const FailureAction action = source.plan->next(FailurePoint::allocation_buffer_acquire);
  if (action.kind == FailureActionKind::errno_failure) {
    return std::unexpected{laghu::core::Error::from_errno(action.native_error,
                                                            "planned allocation failure")};
  }
  return source.acquire(source.context, capacity);
}

void fault_release(void* context, laghu::core::MutableByteView block) noexcept {
  auto& source = *static_cast<FaultInjectedBufferSource*>(context);
  if (source.release != nullptr) {
    source.release(source.context, block);
  }
}

[[nodiscard]] ssize_t fault_read(void* context, int descriptor, void* output,
                                 std::size_t capacity) noexcept {
  auto& operations = *static_cast<FaultInjectedIoOperations*>(context);
  if (operations.plan == nullptr || operations.underlying == nullptr ||
      operations.underlying->read == nullptr) {
    errno = EINVAL;
    return -1;
  }
  const FailureAction action = operations.plan->next(FailurePoint::os_read);
  if (action.kind == FailureActionKind::errno_failure) {
    errno = action.native_error;
    return -1;
  }
  const std::size_t requested = action.kind == FailureActionKind::short_io
                                    ? (action.short_count < capacity ? action.short_count : capacity)
                                    : capacity;
  return operations.underlying->read(operations.underlying->context, descriptor, output, requested);
}

[[nodiscard]] ssize_t fault_write(void* context, int descriptor, const void* input,
                                  std::size_t size) noexcept {
  auto& operations = *static_cast<FaultInjectedIoOperations*>(context);
  if (operations.plan == nullptr || operations.underlying == nullptr ||
      operations.underlying->write == nullptr) {
    errno = EINVAL;
    return -1;
  }
  const FailureAction action = operations.plan->next(FailurePoint::os_write);
  if (action.kind == FailureActionKind::errno_failure) {
    errno = action.native_error;
    return -1;
  }
  const std::size_t requested = action.kind == FailureActionKind::short_io
                                    ? (action.short_count < size ? action.short_count : size)
                                    : size;
  return operations.underlying->write(operations.underlying->context, descriptor, input, requested);
}

}  // namespace

bool FailurePlan::set(FailurePoint point, std::size_t nth_call, FailureAction action) noexcept {
  if (nth_call == 0) {
    return false;
  }
  for (Rule& rule : rules) {
    if (rule.active && rule.point == point) {
      rule = Rule{point, action, nth_call, 0, true};
      return true;
    }
  }
  for (Rule& rule : rules) {
    if (!rule.active) {
      rule = Rule{point, action, nth_call, 0, true};
      return true;
    }
  }
  return false;
}

bool FailurePlan::fail_allocation_on(FailurePoint point, std::size_t nth_call) noexcept {
  if (!is_allocation_point(point)) {
    return false;
  }
  return set(point, nth_call, FailureAction{FailureActionKind::errno_failure, ENOMEM, 0});
}

bool FailurePlan::fail_syscall_on(FailurePoint point, std::size_t nth_call,
                                  int native_error) noexcept {
  return (is_io_point(point) || point == FailurePoint::descriptor_close) && native_error != 0 &&
         set(point, nth_call, FailureAction{FailureActionKind::errno_failure, native_error, 0});
}

bool FailurePlan::short_io_on(FailurePoint point, std::size_t nth_call,
                              std::size_t byte_count) noexcept {
  return is_io_point(point) &&
         set(point, nth_call, FailureAction{FailureActionKind::short_io, 0, byte_count});
}

FailureAction FailurePlan::next(FailurePoint point) noexcept {
  for (Rule& rule : rules) {
    if (rule.active && rule.point == point) {
      ++rule.calls;
      return rule.calls == rule.nth_call ? rule.action : FailureAction{};
    }
  }
  return {};
}

laghu::core::BufferBlockSource FaultInjectedBufferSource::block_source() noexcept {
  return laghu::core::BufferBlockSource{this, fault_acquire, fault_release};
}

laghu::os::internal::IoOperations FaultInjectedIoOperations::operations() noexcept {
  return laghu::os::internal::IoOperations{this, fault_read, fault_write};
}

laghu::core::internal::DescriptorOperations
FaultInjectedDescriptorOperations::operations() noexcept {
  return laghu::core::internal::DescriptorOperations{this, fault_close};
}

}  // namespace laghu::test
