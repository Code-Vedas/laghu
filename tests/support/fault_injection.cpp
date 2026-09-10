// SPDX-License-Identifier: AGPL-3.0-only
#include "laghu_test_faults.hpp"
#include "laghu_test_support.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>

#include <fcntl.h>
#include <unistd.h>

namespace {

using laghu::core::BoundedBuffer;
using laghu::core::ByteView;
using laghu::core::ErrorCode;
using laghu::core::ErrorDomain;
using laghu::core::MemoryBudget;
using laghu::core::MappedRegion;
using laghu::core::MappingAccess;
using laghu::core::MappingFlush;
using laghu::core::MutableByteView;
using laghu::core::Result;
using laghu::core::Retryability;
using laghu::core::WorkerId;
using laghu::core::internal::HandleTestAccess;
using laghu::core::internal::MappedRegionTestAccess;
using laghu::core::internal::MappingOperations;

constexpr std::size_t mapping_size = 4096;

struct FixedBlockSource final {
  std::array<std::array<std::byte, 16>, 2> blocks{};
  std::size_t acquire_calls{};
  std::size_t release_calls{};
  std::size_t next{};
};

[[nodiscard]] Result<MutableByteView> acquire(void* context, std::size_t minimum) noexcept {
  auto& source = *static_cast<FixedBlockSource*>(context);
  ++source.acquire_calls;
  if (source.next == source.blocks.size() || minimum > source.blocks[source.next].size()) {
    return std::unexpected{laghu::core::Error{laghu::core::ErrorDomain::core,
                                               laghu::core::ErrorCode::exhaustion, 0,
                                               "test source exhausted"}};
  }
  return MutableByteView::from(std::span<std::byte>{source.blocks[source.next++]}.first(minimum));
}

void release(void* context, MutableByteView) noexcept {
  ++static_cast<FixedBlockSource*>(context)->release_calls;
}

struct MappingState final {
  std::array<std::byte, mapping_size> storage{};
  int map_calls{};
  int unmap_calls{};
  int flush_calls{};
  int protect_calls{};
  int page_size_calls{};
  int file_size_calls{};
  int open_shared_memory_calls{};
};

[[nodiscard]] void* map(void* context, int, std::size_t, std::uint64_t,
                        MappingAccess) noexcept {
  auto& state = *static_cast<MappingState*>(context);
  ++state.map_calls;
  return state.storage.data();
}

[[nodiscard]] int unmap(void* context, void*, std::size_t) noexcept {
  ++static_cast<MappingState*>(context)->unmap_calls;
  return 0;
}

[[nodiscard]] int mapping_noop(void* context, void*, std::size_t, MappingFlush) noexcept {
  ++static_cast<MappingState*>(context)->flush_calls;
  return 0;
}

[[nodiscard]] int mapping_protect(void* context, void*, std::size_t, MappingAccess) noexcept {
  ++static_cast<MappingState*>(context)->protect_calls;
  return 0;
}

[[nodiscard]] long mapping_page_size(void* context) noexcept {
  ++static_cast<MappingState*>(context)->page_size_calls;
  return static_cast<long>(mapping_size);
}

[[nodiscard]] int mapping_file_size(void* context, int, std::uint64_t* output) noexcept {
  ++static_cast<MappingState*>(context)->file_size_calls;
  *output = mapping_size;
  return 0;
}

[[nodiscard]] int mapping_open_shared_memory(void* context, const char*, MappingAccess) noexcept {
  ++static_cast<MappingState*>(context)->open_shared_memory_calls;
  return 73;
}

[[nodiscard]] MappingOperations mapping_operations(MappingState& state) noexcept {
  return MappingOperations{&state,
                           map,
                           unmap,
                           mapping_noop,
                           mapping_protect,
                           mapping_page_size,
                           mapping_file_size,
                           mapping_open_shared_memory,
                           reinterpret_cast<void*>(static_cast<std::uintptr_t>(1))};
}

[[nodiscard]] Result<laghu::core::FileHandle> disposable_file() noexcept {
  const int descriptor = ::open("/dev/null", O_RDONLY);
  if (descriptor < 0) {
    return std::unexpected{laghu::core::Error::from_errno(errno, "test descriptor open failed")};
  }
  return laghu::core::FileHandle::adopt(descriptor);
}

[[nodiscard]] bool check_allocation_rollback() noexcept {
  const auto worker = WorkerId::from_uint64(7);
  if (!worker.has_value()) {
    return false;
  }
  FixedBlockSource source{};
  laghu::test::FailurePlan plan;
  if (!plan.fail_allocation_on(laghu::test::FailurePoint::allocation_buffer_acquire, 2)) {
    return false;
  }
  laghu::test::FaultInjectedBufferSource injected{&plan, &source, acquire, release};
  MemoryBudget budget{*worker, 32};
  BoundedBuffer buffer{*worker, budget, injected.block_source(), 4, 16};
  const auto first = buffer.reserve(*worker, 4);
  if (!first.has_value()) {
    return false;
  }
  first->data()[0] = std::byte{0x2a};
  if (!buffer.commit(*worker, 1).has_value()) {
    return false;
  }
  const auto failed = buffer.reserve(*worker, 15);
  if (failed.has_value() || failed.error().native_code() != ENOMEM || buffer.size() != 1 ||
      buffer.capacity() != 4 || source.acquire_calls != 1 || source.release_calls != 0) {
    return false;
  }
  const auto readable = buffer.readable(*worker);
  return readable.has_value() && readable->size() == 1 &&
         readable->data()[0] == std::byte{0x2a} && *budget.charged(*worker) == 4;
}

[[nodiscard]] bool check_incomplete_block_source_rejected() noexcept {
  const auto worker = WorkerId::from_uint64(8);
  if (!worker.has_value()) {
    return false;
  }
  FixedBlockSource source{};
  laghu::test::FailurePlan plan;
  MemoryBudget budget{*worker, 32};

  laghu::test::FaultInjectedBufferSource missing_acquire{&plan, &source, nullptr, release};
  const auto acquire_source = missing_acquire.block_source();
  BoundedBuffer acquire_buffer{*worker, budget, acquire_source, 4, 16};
  const auto acquire_result = acquire_buffer.reserve(*worker, 4);
  if (acquire_source.acquire != nullptr || acquire_source.release == nullptr ||
      acquire_result.has_value() || acquire_result.error().code() != ErrorCode::invalid_input ||
      source.acquire_calls != 0 || source.release_calls != 0 || acquire_buffer.capacity() != 0 ||
      *budget.charged(*worker) != 0) {
    return false;
  }

  laghu::test::FaultInjectedBufferSource missing_release{&plan, &source, acquire, nullptr};
  const auto release_source = missing_release.block_source();
  BoundedBuffer release_buffer{*worker, budget, release_source, 4, 16};
  const auto release_result = release_buffer.reserve(*worker, 4);
  return release_source.acquire != nullptr && release_source.release == nullptr &&
         !release_result.has_value() && release_result.error().code() == ErrorCode::invalid_input &&
         source.acquire_calls == 0 && source.release_calls == 0 && release_buffer.capacity() == 0 &&
         *budget.charged(*worker) == 0;
}

[[nodiscard]] bool check_short_io_and_eintr() noexcept {
  int pipe_descriptors[2]{};
  if (::pipe(pipe_descriptors) != 0) {
    return false;
  }
  laghu::test::FailurePlan plan;
  const auto& native = laghu::os::internal::default_io_operations();
  laghu::test::FaultInjectedIoOperations injected{&plan, &native};
  const auto operations = injected.operations();
  constexpr std::array input{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const auto view = ByteView::from(std::span<const std::byte>{input});
  if (!view.has_value() || !plan.short_io_on(laghu::test::FailurePoint::os_write, 1, 2)) {
    static_cast<void>(::close(pipe_descriptors[0]));
    static_cast<void>(::close(pipe_descriptors[1]));
    return false;
  }
  const auto partial = laghu::os::internal::write_once(pipe_descriptors[1], *view, operations);
  if (!partial.has_value() || *partial != 2) {
    static_cast<void>(::close(pipe_descriptors[0]));
    static_cast<void>(::close(pipe_descriptors[1]));
    return false;
  }
  std::array<std::byte, 2> received{};
  const ssize_t read_count = ::read(pipe_descriptors[0], received.data(), received.size());
  const bool short_write = read_count == 2 && received[0] == input[0] && received[1] == input[1];
  const ssize_t source_write =
      short_write ? ::write(pipe_descriptors[1], input.data(), input.size()) : -1;
  if (source_write != static_cast<ssize_t>(input.size()) ||
      !plan.short_io_on(laghu::test::FailurePoint::os_read, 1, 2)) {
    static_cast<void>(::close(pipe_descriptors[0]));
    static_cast<void>(::close(pipe_descriptors[1]));
    return false;
  }
  std::array short_output{std::byte{0xfe}, std::byte{0xfe}, std::byte{0xfe}};
  const auto short_output_view = MutableByteView::from(std::span<std::byte>{short_output});
  const auto short_read = short_output_view.has_value()
                              ? laghu::os::internal::read_once(pipe_descriptors[0],
                                                                *short_output_view, operations)
                              : Result<std::size_t>{std::unexpected{short_output_view.error()}};
  const bool short_read_correct = short_read.has_value() && *short_read == 2 &&
                                  short_output[0] == input[0] && short_output[1] == input[1] &&
                                  short_output[2] == std::byte{0xfe};
  if (!plan.fail_syscall_on(laghu::test::FailurePoint::os_read, 1, EINTR)) {
    static_cast<void>(::close(pipe_descriptors[0]));
    static_cast<void>(::close(pipe_descriptors[1]));
    return false;
  }
  std::array<std::byte, 1> output{};
  const auto output_view = MutableByteView::from(std::span<std::byte>{output});
  const auto interrupted = output_view.has_value()
                               ? laghu::os::internal::read_once(pipe_descriptors[0], *output_view,
                                                                 operations)
                               : Result<std::size_t>{std::unexpected{output_view.error()}};
  static_cast<void>(::close(pipe_descriptors[0]));
  static_cast<void>(::close(pipe_descriptors[1]));
  return short_write && short_read_correct && !interrupted.has_value() &&
         interrupted.error().native_code() == EINTR;
}

[[nodiscard]] bool check_descriptor_close_error() noexcept {
  int descriptors[2]{};
  if (::pipe(descriptors) != 0) {
    return false;
  }
  laghu::test::FailurePlan plan;
  const auto& native = laghu::core::internal::default_descriptor_operations();
  laghu::test::FaultInjectedDescriptorOperations injected{&plan, &native};
  if (!plan.fail_syscall_on(laghu::test::FailurePoint::descriptor_close, 1, EAGAIN)) {
    static_cast<void>(::close(descriptors[0]));
    static_cast<void>(::close(descriptors[1]));
    return false;
  }
  const auto operations = injected.operations();
  auto handle = HandleTestAccess::adopt_file(descriptors[0], operations);
  const auto close_result = handle.close();
  const bool closed_once = !close_result.has_value() &&
                           close_result.error().domain() == ErrorDomain::posix &&
                           close_result.error().code() == ErrorCode::io &&
                           close_result.error().retryability() == Retryability::may_retry &&
                           close_result.error().native_code() == EAGAIN && !handle.is_valid();
  const int cleanup_read = ::close(descriptors[0]);
  const int cleanup_write = ::close(descriptors[1]);
  return closed_once && cleanup_read == 0 && cleanup_write == 0;
}

[[nodiscard]] bool check_mapping_failures_preserve_state() noexcept {
  MappingState state{};
  const MappingOperations native = mapping_operations(state);
  laghu::test::FailurePlan map_plan;
  if (!map_plan.fail_syscall_on(laghu::test::FailurePoint::mapping_map, 2, EIO)) {
    return false;
  }
  laghu::test::FaultInjectedMappingOperations map_injected{&map_plan, &native};
  const MappingOperations mapped_operations = map_injected.operations();

  auto first_file = disposable_file();
  if (!first_file.has_value()) {
    return false;
  }
  auto first = MappedRegionTestAccess::map(std::move(*first_file), mapping_size,
                                           MappingAccess::read_write, 0, mapped_operations);
  if (!first.has_value() || first_file->is_valid() || state.page_size_calls != 1 ||
      state.file_size_calls != 1) {
    return false;
  }
  MappedRegion first_region{std::move(*first)};
  if (!first_region.flush(0, mapping_size, MappingFlush::synchronous).has_value() ||
      !first_region.protect(MappingAccess::read_only).has_value() ||
      mapped_operations.open_shared_memory(mapped_operations.context, "/laghu-test",
                                           MappingAccess::read_only) != 73 ||
      state.flush_calls != 1 || state.protect_calls != 1 || state.page_size_calls != 2 ||
      state.open_shared_memory_calls != 1) {
    return false;
  }
  auto released = first_region.release();
  if (!released.has_value() || !released->close().has_value() || state.map_calls != 1 ||
      state.unmap_calls != 1) {
    return false;
  }

  auto second_file = disposable_file();
  if (!second_file.has_value()) {
    return false;
  }
  const auto failed_map = MappedRegionTestAccess::map(std::move(*second_file), mapping_size,
                                                       MappingAccess::read_write, 0,
                                                       mapped_operations);
  if (failed_map.has_value() || failed_map.error().native_code() != EIO ||
      !second_file->is_valid() || state.map_calls != 1 || !second_file->close().has_value()) {
    return false;
  }

  laghu::test::FailurePlan unmap_plan;
  if (!unmap_plan.fail_syscall_on(laghu::test::FailurePoint::mapping_unmap, 1, EBUSY)) {
    return false;
  }
  laghu::test::FaultInjectedMappingOperations unmap_injected{&unmap_plan, &native};
  const MappingOperations unmapped_operations = unmap_injected.operations();
  auto third_file = disposable_file();
  if (!third_file.has_value()) {
    return false;
  }
  auto third = MappedRegionTestAccess::map(std::move(*third_file), mapping_size,
                                           MappingAccess::read_write, 0, unmapped_operations);
  if (!third.has_value() || third_file->is_valid()) {
    return false;
  }
  MappedRegion third_region{std::move(*third)};
  const auto failed_unmap = third_region.release();
  if (failed_unmap.has_value() || failed_unmap.error().native_code() != EBUSY ||
      !third_region.is_mapped() || state.unmap_calls != 1) {
    return false;
  }
  auto third_released = third_region.release();
  laghu::test::FaultInjectedMappingOperations incomplete{&unmap_plan, nullptr};
  const MappingOperations incomplete_operations = incomplete.operations();
  errno = 0;
  const bool incomplete_rejected = incomplete_operations.page_size(incomplete_operations.context) == -1 &&
                                   errno == EINVAL;
  return third_released.has_value() && third_released->close().has_value() &&
         !third_region.is_mapped() && state.map_calls == 2 && state.unmap_calls == 2 &&
         incomplete_rejected;
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"fault_injection.allocation_rollback", check_allocation_rollback},
      laghu::test::TestCase{"fault_injection.incomplete_block_source",
                            check_incomplete_block_source_rejected},
      laghu::test::TestCase{"fault_injection.short_io_and_eintr", check_short_io_and_eintr},
      laghu::test::TestCase{"fault_injection.descriptor_close_error", check_descriptor_close_error},
      laghu::test::TestCase{"fault_injection.mapping_failures_preserve_state",
                            check_mapping_failures_preserve_state},
  };
  return laghu::test::run_tests(tests);
}
