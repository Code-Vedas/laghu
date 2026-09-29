// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <sys/socket.h>
#include <unistd.h>

#include <laghu/core/clocks.hpp>
#include <laghu/core/handles.hpp>
#include <laghu/core/views.hpp>
#include <laghu/os/internal/socket_drain.hpp>

#include <laghu/benchmark/internal/workload.hpp>

const std::string_view laghu::benchmark::internal::workload_name{"socket-drain-batching"};
const std::uint64_t laghu::benchmark::internal::operations_per_interval = 1312U;

namespace {

struct Fixture final {
  std::size_t chunk_size;
  std::uint64_t calls{};
};

[[nodiscard]] int accept_unused(void*, int) noexcept { errno = EAGAIN; return -1; }
[[nodiscard]] ssize_t read_chunk(void* context, int, void*, std::size_t size) noexcept {
  auto& fixture = *static_cast<Fixture*>(context);
  ++fixture.calls;
  return static_cast<ssize_t>(std::min(size, fixture.chunk_size));
}
[[nodiscard]] ssize_t write_unused(void*, int, const void*, std::size_t) noexcept {
  errno = EAGAIN; return -1;
}
[[nodiscard]] laghu::core::Result<laghu::core::MonotonicInstant> now(void*) noexcept {
  return 0U;
}
[[nodiscard]] laghu::core::Result<laghu::core::RealtimeInstant> realtime(void*) noexcept {
  return 0;
}

template <std::uint32_t Batch>
[[nodiscard]] laghu::core::Result<std::uint64_t> run_strategy(
    std::uint64_t checksum) noexcept {
  constexpr std::size_t repetitions = 32U;
  std::array<std::byte, 4096> storage{};
  const auto output = laghu::core::MutableByteView::from(storage);
  int descriptors[2]{};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
    return std::unexpected{laghu::core::Error::from_errno(errno)};
  }
  auto handle = laghu::core::SocketHandle::adopt(descriptors[0]);
  auto peer = laghu::core::SocketHandle::adopt(descriptors[1]);
  if (!output || !handle || !peer) return std::unexpected{laghu::core::Error{
      laghu::core::ErrorDomain::core, laghu::core::ErrorCode::invalid_state}};
  Fixture fixture{64U};
  const laghu::os::internal::SocketDrainOperations operations{
      &fixture, accept_unused, read_chunk, write_unused,
      {nullptr, now, realtime}};
  for (std::size_t iteration = 0; iteration < repetitions; ++iteration) {
    const auto result = laghu::os::internal::SocketDrainTestAccess::read(
        handle->borrow(), *output,
        {storage.size(), Batch, std::chrono::seconds{1}}, operations);
    if (!result || result->state != laghu::os::SocketDrainState::budget_exhausted) {
      return std::unexpected{laghu::core::Error{
          laghu::core::ErrorDomain::core, laghu::core::ErrorCode::invalid_state,
          0, "socket drain benchmark invariant failed"}};
    }
    checksum ^= result->bytes + result->operations;
  }
  return checksum ^ fixture.calls;
}

}  // namespace

laghu::core::Result<std::uint64_t> laghu::benchmark::internal::run_workload(
    std::uint64_t seed, WorkloadCounters& counters) noexcept {
  std::uint64_t checksum = seed;
  for (const auto strategy : {run_strategy<1>, run_strategy<8>, run_strategy<32>}) {
    const auto result = strategy(checksum);
    if (!result) return std::unexpected{result.error()};
    checksum = *result;
  }
  static_cast<void>(counters);
  return checksum;
}
