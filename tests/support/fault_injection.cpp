// SPDX-License-Identifier: AGPL-3.0-only
#include "laghu_test_faults.hpp"
#include "laghu_test_support.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>

#include <unistd.h>

namespace {

using laghu::core::BoundedBuffer;
using laghu::core::ByteView;
using laghu::core::MemoryBudget;
using laghu::core::MutableByteView;
using laghu::core::Result;
using laghu::core::WorkerId;

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

[[nodiscard]] bool check_short_io_and_eintr() noexcept {
  int pipe_descriptors[2]{};
  if (::pipe(pipe_descriptors) != 0) {
    return false;
  }
  laghu::test::FailurePlan plan;
  const auto& native = laghu::os::internal::default_io_operations();
  laghu::test::FaultInjectedIoOperations injected{&plan, &native};
  constexpr std::array input{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const auto view = ByteView::from(std::span<const std::byte>{input});
  if (!view.has_value() || !plan.short_io_on(laghu::test::FailurePoint::os_write, 1, 2)) {
    static_cast<void>(::close(pipe_descriptors[0]));
    static_cast<void>(::close(pipe_descriptors[1]));
    return false;
  }
  const auto partial = laghu::os::internal::write_once(pipe_descriptors[1], *view,
                                                        injected.operations());
  std::array<std::byte, 2> received{};
  const ssize_t read_count = ::read(pipe_descriptors[0], received.data(), received.size());
  const bool short_write = partial.has_value() && *partial == 2 && read_count == 2 &&
                           received[0] == input[0] && received[1] == input[1];
  if (!plan.fail_syscall_on(laghu::test::FailurePoint::os_read, 1, EINTR)) {
    static_cast<void>(::close(pipe_descriptors[0]));
    static_cast<void>(::close(pipe_descriptors[1]));
    return false;
  }
  std::array<std::byte, 1> output{};
  const auto output_view = MutableByteView::from(std::span<std::byte>{output});
  const auto interrupted = output_view.has_value()
                               ? laghu::os::internal::read_once(pipe_descriptors[0], *output_view,
                                                                 injected.operations())
                               : Result<std::size_t>{std::unexpected{output_view.error()}};
  static_cast<void>(::close(pipe_descriptors[0]));
  static_cast<void>(::close(pipe_descriptors[1]));
  return short_write && !interrupted.has_value() && interrupted.error().native_code() == EINTR;
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"fault_injection.allocation_rollback", check_allocation_rollback},
      laghu::test::TestCase{"fault_injection.short_io_and_eintr", check_short_io_and_eintr},
  };
  return laghu::test::run_tests(tests);
}
