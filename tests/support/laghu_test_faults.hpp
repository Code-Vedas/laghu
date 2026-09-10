// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>

#include <laghu/core/bounded_buffer.hpp>
#include <laghu/core/internal/descriptor_operations.hpp>
#include <laghu/core/internal/mapping_operations.hpp>
#include <laghu/os/internal/io_operations.hpp>

namespace laghu::test {

enum class FailurePoint : unsigned char {
  allocation_buffer_acquire = 1,
  descriptor_close = 2,
  mapping_map = 3,
  mapping_unmap = 4,
  os_read = 5,
  os_write = 6,
};

enum class FailureActionKind : unsigned char { none, errno_failure, short_io };

struct FailureAction final {
  FailureActionKind kind{};
  int native_error{};
  std::size_t short_count{};
};

class FailurePlan final {
 public:
  [[nodiscard]] bool fail_allocation_on(FailurePoint point, std::size_t nth_call) noexcept;
  [[nodiscard]] bool fail_syscall_on(FailurePoint point, std::size_t nth_call,
                                     int native_error) noexcept;
  [[nodiscard]] bool short_io_on(FailurePoint point, std::size_t nth_call,
                                  std::size_t byte_count) noexcept;
  [[nodiscard]] FailureAction next(FailurePoint point) noexcept;

 private:
  struct Rule final {
    FailurePoint point{};
    FailureAction action{};
    std::size_t nth_call{};
    std::size_t calls{};
    bool active{};
  };

  [[nodiscard]] bool set(FailurePoint point, std::size_t nth_call,
                         FailureAction action) noexcept;

  std::array<Rule, 12> rules{};
};

struct FaultInjectedBufferSource final {
  FailurePlan* plan{};
  void* context{};
  laghu::core::BufferBlockSource::Acquire acquire{};
  laghu::core::BufferBlockSource::Release release{};

  [[nodiscard]] laghu::core::BufferBlockSource block_source() noexcept;
};

struct FaultInjectedIoOperations final {
  FailurePlan* plan{};
  const laghu::os::internal::IoOperations* underlying{};

  [[nodiscard]] laghu::os::internal::IoOperations operations() noexcept;
};

struct FaultInjectedDescriptorOperations final {
  FailurePlan* plan{};
  const laghu::core::internal::DescriptorOperations* underlying{};

  [[nodiscard]] laghu::core::internal::DescriptorOperations operations() noexcept;
};

struct FaultInjectedMappingOperations final {
  FailurePlan* plan{};
  const laghu::core::internal::MappingOperations* underlying{};

  [[nodiscard]] laghu::core::internal::MappingOperations operations() noexcept;
};

}  // namespace laghu::test
