// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "laghu_test_support.hpp"

#include <laghu/core/deadlines_cancellation.hpp>
#include <laghu/core/handles.hpp>
#include <laghu/os/file_transfer.hpp>
#include <laghu/os/internal/file_transfer.hpp>

namespace {

using laghu::core::CancellationSource;
using laghu::core::CancellationState;
using laghu::core::FileHandle;
using laghu::core::SocketHandle;
using laghu::os::FileTransferMode;
using laghu::os::FileTransferPath;
using laghu::os::FileTransferRequest;
using laghu::os::FileTransferState;

struct SocketPair final {
  SocketHandle writer;
  SocketHandle reader;
};

[[nodiscard]] bool fixture_file_path(
    const laghu::test::TemporaryDirectory& directory,
    std::array<char, laghu::test::fixture_path_capacity>& path) noexcept {
  const std::string_view root = directory.path();
  constexpr std::string_view suffix{"/transfer.bin"};
  if (root.size() + suffix.size() >= path.size()) {
    return false;
  }
  std::memcpy(path.data(), root.data(), root.size());
  std::memcpy(path.data() + root.size(), suffix.data(), suffix.size());
  return true;
}

[[nodiscard]] laghu::core::Result<SocketPair> make_socket_pair() noexcept {
  int descriptors[2]{};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
    return std::unexpected{laghu::core::Error::from_errno(errno, "socketpair failed")};
  }
  auto writer = SocketHandle::adopt(descriptors[0]);
  auto reader = SocketHandle::adopt(descriptors[1]);
  if (!writer || !reader) {
    static_cast<void>(::close(descriptors[0]));
    static_cast<void>(::close(descriptors[1]));
    return std::unexpected{laghu::core::Error::from_errno(EINVAL, "socket adoption failed")};
  }
  const int flags = ::fcntl(writer->native_handle(), F_GETFL, 0);
  if (flags < 0 || ::fcntl(writer->native_handle(), F_SETFL,
                           flags | O_NONBLOCK) != 0) {
    return std::unexpected{laghu::core::Error::from_errno(
        errno, "socket nonblocking setup failed")};
  }
  return SocketPair{std::move(*writer), std::move(*reader)};
}

[[nodiscard]] laghu::core::Result<SocketPair> make_blocking_socket_pair() noexcept {
  int descriptors[2]{};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
    return std::unexpected{laghu::core::Error::from_errno(errno, "socketpair failed")};
  }
  auto writer = SocketHandle::adopt(descriptors[0]);
  auto reader = SocketHandle::adopt(descriptors[1]);
  if (!writer || !reader) {
    static_cast<void>(::close(descriptors[0]));
    static_cast<void>(::close(descriptors[1]));
    return std::unexpected{laghu::core::Error::from_errno(EINVAL, "socket adoption failed")};
  }
  return SocketPair{std::move(*writer), std::move(*reader)};
}

[[nodiscard]] laghu::core::Result<FileHandle> make_file(
    const laghu::test::TemporaryDirectory& directory,
    std::span<const std::byte> bytes, std::uint64_t sparse_prefix = 0) noexcept {
  std::array<char, laghu::test::fixture_path_capacity> path{};
  if (!fixture_file_path(directory, path)) {
    return std::unexpected{laghu::core::Error::from_errno(ENAMETOOLONG, "fixture path too long")};
  }
  const int descriptor = ::open(path.data(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    return std::unexpected{laghu::core::Error::from_errno(errno, "fixture open failed")};
  }
  if (sparse_prefix > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
      ::pwrite(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(sparse_prefix)) !=
          static_cast<ssize_t>(bytes.size())) {
    const int native_error = errno == 0 ? EIO : errno;
    static_cast<void>(::close(descriptor));
    return std::unexpected{laghu::core::Error::from_errno(native_error, "fixture write failed")};
  }
  return FileHandle::adopt(descriptor);
}

[[nodiscard]] bool read_exact(int descriptor, std::span<std::byte> output) noexcept {
  std::size_t offset{};
  while (offset < output.size()) {
    const ssize_t result = ::read(descriptor, output.data() + offset,
                                  output.size() - offset);
    if (result > 0) {
      offset += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] bool check_real_modes_and_offset() noexcept {
  constexpr std::array payload{std::byte{'a'}, std::byte{'b'}, std::byte{'c'},
                               std::byte{'d'}, std::byte{'e'}, std::byte{'f'}};
  for (const auto mode : {FileTransferMode::generic, FileTransferMode::kernel}) {
    const auto directory = laghu::test::TemporaryDirectory::create(
        mode == FileTransferMode::generic ? "file-transfer-generic"
                                          : "file-transfer-kernel");
    if (!directory) {
      return false;
    }
    auto file = make_file(*directory, payload,
                          mode == FileTransferMode::generic ? 0U : 4096U);
    auto sockets = make_socket_pair();
    CancellationState state;
    CancellationSource source{state};
    const std::uint64_t offset =
        mode == FileTransferMode::generic ? 2U : 4098U;
    if (!file || !sockets) {
      return false;
    }
    const auto result = laghu::os::transfer_file(
        file->borrow(), sockets->writer.borrow(),
        FileTransferRequest{offset, 3,
                            8U, mode},
        source.token());
    const FileTransferPath expected_path =
        mode == FileTransferMode::generic ? FileTransferPath::generic
                                          : FileTransferPath::kernel;
#if defined(__linux__)
    const std::uint32_t expected_calls =
        mode == FileTransferMode::generic ? 3U : 5U;
#else
    const std::uint32_t expected_calls =
        mode == FileTransferMode::kernel ? 3U : 4U;
#endif
    std::array<std::byte, 3> received{};
    if (!result || result->bytes != 3 || result->next_offset != offset + 3U ||
        result->state != FileTransferState::progress ||
        result->path != expected_path ||
        result->syscalls != expected_calls ||
        !read_exact(sockets->reader.native_handle(), received) ||
        received[0] != std::byte{'c'} || received[2] != std::byte{'e'}) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool check_direct_requirements() noexcept {
  const auto directory =
      laghu::test::TemporaryDirectory::create("file-transfer-direct-requirements");
  alignas(4096) std::array<std::byte, 4096> payload{};
  if (!directory) {
    return false;
  }
  auto file = make_file(*directory, payload);
  auto sockets = make_socket_pair();
  CancellationState state;
  CancellationSource source{state};
  if (!file || !sockets) {
    return false;
  }
  const auto unavailable = laghu::os::transfer_file(
      file->borrow(), sockets->writer.borrow(),
      FileTransferRequest{0, payload.size(), 8, FileTransferMode::direct},
      source.token());
  if (unavailable ||
      unavailable.error().code() != laghu::core::ErrorCode::unavailable_capability) {
    return false;
  }
  return true;
}

[[nodiscard]] bool check_direct_transfer() noexcept {
#if defined(__linux__) && defined(O_DIRECT)
  const auto directory =
      laghu::test::TemporaryDirectory::create("file-transfer-direct");
  alignas(4096) std::array<std::byte, 4096> payload{};
  for (std::size_t index = 0; index < payload.size(); ++index) {
    payload[index] = static_cast<std::byte>(index & 0xffU);
  }
  if (!directory) {
    return false;
  }
  auto ordinary_file = make_file(*directory, payload);
  std::array<char, laghu::test::fixture_path_capacity> path{};
  if (!ordinary_file || !ordinary_file->close() ||
      !fixture_file_path(*directory, path)) {
    return false;
  }
  const int descriptor = ::open(path.data(), O_RDONLY | O_CLOEXEC | O_DIRECT);
  if (descriptor < 0) {
    return false;
  }
  auto direct_file = FileHandle::adopt(descriptor);
  auto sockets = make_socket_pair();
  CancellationState state;
  CancellationSource source{state};
  if (!direct_file || !sockets) {
    return false;
  }
  const auto unaligned = laghu::os::transfer_file(
      direct_file->borrow(), sockets->writer.borrow(),
      FileTransferRequest{1, payload.size(), 8, FileTransferMode::direct},
      source.token());
  if (unaligned ||
      unaligned.error().code() != laghu::core::ErrorCode::invalid_range) {
    return false;
  }
  const auto result = laghu::os::transfer_file(
      direct_file->borrow(), sockets->writer.borrow(),
      FileTransferRequest{0, payload.size(), 8, FileTransferMode::direct},
      source.token());
  alignas(4096) std::array<std::byte, 4096> received{};
  return result && result->path == FileTransferPath::direct &&
         result->bytes == payload.size() &&
         read_exact(sockets->reader.native_handle(), received) &&
         received == payload;
#else
  return true;
#endif
}

struct Injected final {
  std::uint32_t pread_calls{};
  std::uint32_t send_calls{};
  std::uint32_t kernel_calls{};
  std::uint32_t prepare_calls{};
  int kernel_error{};
  std::size_t kernel_bytes{};
  std::uint32_t interrupted_sends{};
  CancellationSource* cancel_after_read{};
};

[[nodiscard]] ssize_t injected_pread(void* context, int descriptor, void* output,
                                     std::size_t size, std::uint64_t) noexcept {
  auto& state = *static_cast<Injected*>(context);
  ++state.pread_calls;
  static_cast<void>(descriptor);
  std::memset(output, 'x', size);
  if (state.cancel_after_read != nullptr) {
    static_cast<void>(state.cancel_after_read->cancel());
  }
  return static_cast<ssize_t>(size);
}

[[nodiscard]] ssize_t injected_send(void* context, int, const void*,
                                    std::size_t size, int) noexcept {
  auto& state = *static_cast<Injected*>(context);
  ++state.send_calls;
  if (state.interrupted_sends != 0U) {
    --state.interrupted_sends;
    errno = EINTR;
    return -1;
  }
  return static_cast<ssize_t>(size);
}

[[nodiscard]] int injected_kernel(void* context, int, int, std::uint64_t,
                                  std::size_t, std::uint32_t maximum_calls,
                                  std::size_t* transferred,
                                  std::uint32_t* calls) noexcept {
  *calls = 0;
  if (maximum_calls == 0) {
    return 1;
  }
  auto& state = *static_cast<Injected*>(context);
  ++state.kernel_calls;
  *calls = 1U;
  *transferred = state.kernel_bytes;
  if (state.kernel_error != 0) {
    errno = state.kernel_error;
    return -1;
  }
  return 0;
}

[[nodiscard]] int injected_prepare(void* context, int,
                                   std::uint32_t maximum_calls,
                                   std::uint32_t* calls) noexcept {
  const auto& state = *static_cast<Injected*>(context);
  *calls = std::min(maximum_calls, state.prepare_calls);
  if (maximum_calls < state.prepare_calls) {
    return 1;
  }
  return 0;
}

[[nodiscard]] laghu::os::internal::FileTransferOperations operations(
    Injected& state) noexcept {
  return {&state, injected_pread, injected_send, injected_kernel, injected_prepare};
}

[[nodiscard]] bool check_direct_cancellation_and_interrupted_send() noexcept {
#if defined(__linux__) && defined(O_DIRECT)
  const auto directory = laghu::test::TemporaryDirectory::create(
      "file-transfer-direct-injected");
  alignas(4096) std::array<std::byte, 4096> payload{};
  if (!directory) {
    return false;
  }
  auto ordinary_file = make_file(*directory, payload);
  std::array<char, laghu::test::fixture_path_capacity> path{};
  if (!ordinary_file || !ordinary_file->close() ||
      !fixture_file_path(*directory, path)) {
    return false;
  }
  const int descriptor = ::open(path.data(), O_RDONLY | O_CLOEXEC | O_DIRECT);
  if (descriptor < 0) {
    return false;
  }
  auto direct_file = FileHandle::adopt(descriptor);
  auto cancelled_sockets = make_socket_pair();
  CancellationState cancelled_state;
  CancellationSource cancelled_source{cancelled_state};
  Injected cancelled{};
  cancelled.cancel_after_read = &cancelled_source;
  auto cancelled_operations = operations(cancelled);
  if (!direct_file || !cancelled_sockets) {
    return false;
  }
  const auto stopped = laghu::os::internal::FileTransferTestAccess::transfer(
      direct_file->borrow(), cancelled_sockets->writer.borrow(),
      FileTransferRequest{0, payload.size(), 8, FileTransferMode::direct},
      cancelled_source.token(), cancelled_operations);
  if (stopped || stopped.error().code() != laghu::core::ErrorCode::cancellation ||
      cancelled.pread_calls != 1U || cancelled.send_calls != 0U) {
    return false;
  }

  auto retry_sockets = make_socket_pair();
  CancellationState retry_state;
  CancellationSource retry_source{retry_state};
  Injected interrupted{};
  interrupted.interrupted_sends = 1U;
  auto interrupted_operations = operations(interrupted);
  if (!retry_sockets) {
    return false;
  }
  const auto retried = laghu::os::internal::FileTransferTestAccess::transfer(
      direct_file->borrow(), retry_sockets->writer.borrow(),
      FileTransferRequest{0, payload.size(), 8, FileTransferMode::direct},
      retry_source.token(), interrupted_operations);
  return retried && retried->state == FileTransferState::progress &&
         retried->path == FileTransferPath::direct &&
         retried->bytes == payload.size() && interrupted.pread_calls == 1U &&
         interrupted.send_calls == 2U && interrupted.interrupted_sends == 0U;
#else
  return true;
#endif
}

[[nodiscard]] bool check_preparation_budget_accounting() noexcept {
  const auto directory = laghu::test::TemporaryDirectory::create(
      "file-transfer-preparation-budget");
  constexpr std::array payload{std::byte{'x'}};
  if (!directory) {
    return false;
  }
  auto file = make_file(*directory, payload);
  auto sockets = make_socket_pair();
  CancellationState state;
  CancellationSource source{state};
  if (!file || !sockets) {
    return false;
  }
  Injected exhausted{};
  exhausted.prepare_calls = 2U;
  auto exhausted_operations = operations(exhausted);
  const auto budget = laghu::os::internal::FileTransferTestAccess::transfer(
      file->borrow(), sockets->writer.borrow(),
      FileTransferRequest{0, 1, 1, FileTransferMode::generic}, source.token(),
      exhausted_operations);
  return budget && budget->state == FileTransferState::budget_exhausted &&
         budget->syscalls == 1U && exhausted.pread_calls == 0U &&
         exhausted.send_calls == 0U;
}

[[nodiscard]] bool check_fallback_and_cancellation() noexcept {
  const auto directory = laghu::test::TemporaryDirectory::create("file-transfer-injected");
  constexpr std::array payload{std::byte{'x'}};
  if (!directory) {
    return false;
  }
  auto file = make_file(*directory, payload);
  auto sockets = make_socket_pair();
  CancellationState active_state;
  CancellationSource active_source{active_state};
  Injected fallback{};
  fallback.kernel_error = ENOSYS;
  auto fallback_operations = operations(fallback);
  if (!file || !sockets) {
    return false;
  }
  const auto result = laghu::os::internal::FileTransferTestAccess::transfer(
      file->borrow(), sockets->writer.borrow(),
      FileTransferRequest{0, 1, 3, FileTransferMode::automatic},
      active_source.token(), fallback_operations);
  if (!result || result->path != FileTransferPath::generic || result->bytes != 1 ||
      result->syscalls != 3U ||
      fallback.kernel_calls != 1 || fallback.pread_calls != 1 ||
      fallback.send_calls != 1) {
    return false;
  }

  CancellationState cancelled_state;
  CancellationSource cancelled_source{cancelled_state};
  static_cast<void>(cancelled_source.cancel());
  Injected cancelled{};
  auto cancelled_operations = operations(cancelled);
  const auto stopped = laghu::os::internal::FileTransferTestAccess::transfer(
      file->borrow(), sockets->writer.borrow(),
      FileTransferRequest{0, 1, 1, FileTransferMode::kernel},
      cancelled_source.token(), cancelled_operations);
  return !stopped && stopped.error().code() == laghu::core::ErrorCode::cancellation &&
         cancelled.kernel_calls == 0;
}

[[nodiscard]] bool check_large_transfer() noexcept {
  const auto directory = laghu::test::TemporaryDirectory::create("file-transfer-large");
  std::array<std::byte, 65536> payload{};
  for (std::size_t index = 0; index < payload.size(); ++index) {
    payload[index] = static_cast<std::byte>(index & 0xffU);
  }
  if (!directory) {
    return false;
  }
  auto file = make_file(*directory, payload);
  auto sockets = make_socket_pair();
  CancellationState state;
  CancellationSource source{state};
  if (!file || !sockets) {
    return false;
  }
  std::uint64_t offset{};
  std::size_t consumed{};
  std::array<std::byte, 16384> received{};
  while (consumed < payload.size()) {
    const auto result = laghu::os::transfer_file(
        file->borrow(), sockets->writer.borrow(),
        FileTransferRequest{offset, payload.size() - consumed,
                            4, FileTransferMode::generic},
        source.token());
    if (!result || result->state != FileTransferState::progress ||
        result->bytes == 0 || result->bytes > received.size() ||
        !read_exact(sockets->reader.native_handle(),
                    std::span<std::byte>{received}.first(result->bytes))) {
      return false;
    }
    for (std::size_t index = 0; index < result->bytes; ++index) {
      if (received[index] != payload[consumed + index]) {
        return false;
      }
    }
    consumed += result->bytes;
    offset = result->next_offset;
  }
  return consumed == payload.size() && offset == payload.size();
}

[[nodiscard]] bool check_bounds_and_capabilities() noexcept {
  const auto capabilities = laghu::os::file_transfer_capabilities();
  const auto directory = laghu::test::TemporaryDirectory::create("file-transfer-bounds");
  constexpr std::array payload{std::byte{'x'}};
  if (!directory) {
    return false;
  }
  auto file = make_file(*directory, payload);
  auto sockets = make_socket_pair();
  CancellationState state;
  CancellationSource source{state};
  if (!file || !sockets) {
    return false;
  }
  const auto overflow = laghu::os::transfer_file(
      file->borrow(), sockets->writer.borrow(),
      FileTransferRequest{std::numeric_limits<std::uint64_t>::max(), 1, 1,
                          FileTransferMode::generic},
      source.token());
  return capabilities.kernel_transfer && !overflow &&
         overflow.error().code() == laghu::core::ErrorCode::overflow;
}

[[nodiscard]] bool check_kernel_destination_requirements() noexcept {
  const auto directory =
      laghu::test::TemporaryDirectory::create("file-transfer-destination");
  constexpr std::array payload{std::byte{'x'}};
  if (!directory) {
    return false;
  }
  auto file = make_file(*directory, payload);
  auto sockets = make_blocking_socket_pair();
  CancellationState state;
  CancellationSource source{state};
  if (!file || !sockets) {
    return false;
  }
  const auto result = laghu::os::transfer_file(
      file->borrow(), sockets->writer.borrow(),
      FileTransferRequest{0, 1, 4, FileTransferMode::kernel}, source.token());
  return !result && result.error().code() == laghu::core::ErrorCode::invalid_input;
}

[[nodiscard]] bool check_kernel_closed_peer() noexcept {
#if defined(__linux__)
  const auto directory =
      laghu::test::TemporaryDirectory::create("file-transfer-closed-peer");
  constexpr std::array payload{std::byte{'x'}};
  if (!directory) {
    return false;
  }
  auto file = make_file(*directory, payload);
  auto sockets = make_socket_pair();
  CancellationState state;
  CancellationSource source{state};
  if (!file || !sockets || !sockets->reader.close()) {
    return false;
  }
  const auto result = laghu::os::transfer_file(
      file->borrow(), sockets->writer.borrow(),
      FileTransferRequest{0, 1, 6, FileTransferMode::kernel}, source.token());
  return !result && result.error().code() == laghu::core::ErrorCode::io;
#else
  return true;
#endif
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"os.file_transfer.real_modes_offset_sparse", check_real_modes_and_offset},
      laghu::test::TestCase{"os.file_transfer.direct_requirements",
                            check_direct_requirements},
      laghu::test::TestCase{"os.file_transfer.direct", check_direct_transfer},
      laghu::test::TestCase{"os.file_transfer.direct_cancel_eintr",
                            check_direct_cancellation_and_interrupted_send},
      laghu::test::TestCase{"os.file_transfer.fallback_cancellation", check_fallback_and_cancellation},
      laghu::test::TestCase{"os.file_transfer.preparation_budget",
                            check_preparation_budget_accounting},
      laghu::test::TestCase{"os.file_transfer.large", check_large_transfer},
      laghu::test::TestCase{"os.file_transfer.bounds_capabilities", check_bounds_and_capabilities},
      laghu::test::TestCase{"os.file_transfer.kernel_destination_requirements",
                            check_kernel_destination_requirements},
      laghu::test::TestCase{"os.file_transfer.kernel_closed_peer",
                            check_kernel_closed_peer},
  };
  return laghu::test::run_tests(tests);
}
