// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <laghu/core/deadlines_cancellation.hpp>
#include <laghu/core/handles.hpp>
#include <laghu/os/file_transfer.hpp>

#include <laghu/benchmark/internal/workload.hpp>

#ifndef LAGHU_FILE_TRANSFER_MODE
#error LAGHU_FILE_TRANSFER_MODE must select a FileTransferMode value
#endif
#ifndef LAGHU_FILE_TRANSFER_BENCHMARK_NAME
#error LAGHU_FILE_TRANSFER_BENCHMARK_NAME must name the benchmark workload
#endif

const std::string_view laghu::benchmark::internal::workload_name{
    LAGHU_FILE_TRANSFER_BENCHMARK_NAME};
const std::uint64_t laghu::benchmark::internal::operations_per_interval = 128U;

namespace {

[[nodiscard]] bool write_all(int descriptor, const std::byte* data,
                             std::size_t size) noexcept {
  while (size != 0) {
    const ssize_t written = ::write(descriptor, data, size);
    if (written > 0) {
      const std::size_t consumed = static_cast<std::size_t>(written);
      data += consumed;
      size -= consumed;
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] bool read_all(int descriptor, std::byte* data,
                            std::size_t size) noexcept {
  while (size != 0) {
    const ssize_t read = ::read(descriptor, data, size);
    if (read > 0) {
      const std::size_t consumed = static_cast<std::size_t>(read);
      data += consumed;
      size -= consumed;
      continue;
    }
    if (read < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

}  // namespace

laghu::core::Result<std::uint64_t> laghu::benchmark::internal::run_workload(
    std::uint64_t seed, WorkloadCounters& counters) noexcept {
  std::array<std::byte, 4096> payload{};
  for (std::size_t index = 0; index < payload.size(); ++index) {
    payload[index] = static_cast<std::byte>((seed + index) & 0xffU);
  }
  char path[] = "/tmp/laghu-file-transfer-XXXXXX";
  const int file_descriptor = ::mkstemp(path);
  if (file_descriptor < 0) {
    return std::unexpected{core::Error::from_errno(errno, "benchmark file creation failed")};
  }
  static_cast<void>(::unlink(path));
  auto file = core::FileHandle::adopt(file_descriptor);
  if (!file) {
    static_cast<void>(::close(file_descriptor));
    return std::unexpected{file.error()};
  }
  if (!write_all(file_descriptor, payload.data(), payload.size())) {
    return std::unexpected{core::Error::from_errno(errno, "benchmark file setup failed")};
  }
  int descriptors[2]{};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
    return std::unexpected{core::Error::from_errno(errno, "benchmark socketpair failed")};
  }
  auto output = core::SocketHandle::adopt(descriptors[0]);
  auto input = core::SocketHandle::adopt(descriptors[1]);
  if (!output || !input) {
    static_cast<void>(::close(descriptors[0]));
    static_cast<void>(::close(descriptors[1]));
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::invalid_state, 0,
                                       "benchmark socket adoption failed"}};
  }
  const int output_flags = ::fcntl(output->native_handle(), F_GETFL, 0);
  if (output_flags < 0 ||
      ::fcntl(output->native_handle(), F_SETFL, output_flags | O_NONBLOCK) != 0) {
    return std::unexpected{core::Error::from_errno(
        errno, "benchmark socket nonblocking setup failed")};
  }
  core::CancellationState cancellation_state;
  core::CancellationSource cancellation_source{cancellation_state};
  std::array<std::byte, 4096> received{};
  for (std::uint64_t iteration = 0; iteration < operations_per_interval; ++iteration) {
    std::size_t transferred{};
    std::uint64_t offset{};
    while (transferred < payload.size()) {
      const std::size_t remaining = payload.size() - transferred;
      const auto result = os::transfer_file(
          file->borrow(), output->borrow(),
          os::FileTransferRequest{offset, remaining, 3,
                                  LAGHU_FILE_TRANSFER_MODE},
          cancellation_source.token());
      if (!result || result->state != os::FileTransferState::progress ||
          result->bytes == 0 || result->bytes > remaining ||
          result->next_offset != offset + result->bytes ||
          !read_all(input->native_handle(), received.data() + transferred,
                    result->bytes) ||
          !counters.record_laghu_syscall_events(result->syscalls)) {
        return std::unexpected{core::Error{core::ErrorDomain::core,
                                           core::ErrorCode::invalid_state, 0,
                                           "file transfer benchmark failed"}};
      }
      transferred += result->bytes;
      offset = result->next_offset;
    }
    seed ^= std::to_integer<std::uint64_t>(received[iteration % received.size()]);
  }
  return seed;
}
