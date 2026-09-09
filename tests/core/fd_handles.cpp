// SPDX-License-Identifier: AGPL-3.0-only
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <laghu/core/handles.hpp>

#include <laghu/core/internal/descriptor_operations.hpp>

std::size_t allocation_attempts{};

void* operator new(std::size_t size) {
  ++allocation_attempts;
  if (void* memory = std::malloc(size); memory != nullptr) {
    return memory;
  }
  std::abort();
}

void* operator new[](std::size_t size) {
  ++allocation_attempts;
  if (void* memory = std::malloc(size); memory != nullptr) {
    return memory;
  }
  std::abort();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

struct CloseRecorder final {
  int calls{};
  int last_descriptor{-1};
  int result{};
  int error{};
};

CloseRecorder* active_recorder{};

[[nodiscard]] int record_close(int descriptor) noexcept {
  if (active_recorder == nullptr) {
    errno = EINVAL;
    return -1;
  }
  ++active_recorder->calls;
  active_recorder->last_descriptor = descriptor;
  errno = active_recorder->error;
  return active_recorder->result;
}

[[nodiscard]] bool check(bool condition) noexcept { return condition; }

[[nodiscard]] bool is_closed(int descriptor) noexcept {
  errno = 0;
  return ::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF;
}

[[nodiscard]] bool check_invalid_and_adoption() noexcept {
  laghu::core::FileHandle file;
  laghu::core::SocketHandle socket;
  const auto invalid_file = laghu::core::FileHandle::adopt(-1);
  const auto invalid_socket = laghu::core::SocketHandle::adopt(-1);
  return !file.is_valid() && !socket.is_valid() && !file.borrow().is_valid() &&
         !socket.borrow().is_valid() && file.close().has_value() && socket.close().has_value() &&
         !invalid_file.has_value() && !invalid_socket.has_value() &&
         invalid_file.error().code() == laghu::core::ErrorCode::invalid_input &&
         invalid_socket.error().native_code() == EINVAL;
}

[[nodiscard]] bool check_injected_lifecycle() noexcept {
  CloseRecorder recorder{};
  active_recorder = &recorder;
  const laghu::core::internal::DescriptorOperations operations{record_close};

  auto original = laghu::core::internal::HandleTestAccess::adopt_file(41, operations);
  auto moved{std::move(original)};
  if (!check(!original.is_valid() && moved.is_valid() && recorder.calls == 0)) {
    active_recorder = nullptr;
    return false;
  }
  if (!check(moved.close().has_value() && !moved.is_valid() && recorder.calls == 1 &&
             recorder.last_descriptor == 41 && moved.close().has_value() && recorder.calls == 1)) {
    active_recorder = nullptr;
    return false;
  }

  auto self = laghu::core::internal::HandleTestAccess::adopt_file(42, operations);
  if (!check(self.reset(std::move(self)).has_value() && self.is_valid() && recorder.calls == 1 &&
             self.close().has_value() && recorder.calls == 2)) {
    active_recorder = nullptr;
    return false;
  }

  auto same_first = laghu::core::internal::HandleTestAccess::adopt_file(43, operations);
  auto same_second = laghu::core::internal::HandleTestAccess::adopt_file(43, operations);
  if (!check(same_first.reset(std::move(same_second)).has_value() && same_first.is_valid() &&
             !same_second.is_valid() && recorder.calls == 2 && same_first.close().has_value() &&
             recorder.calls == 3)) {
    active_recorder = nullptr;
    return false;
  }

  auto assigned = laghu::core::internal::HandleTestAccess::adopt_file(44, operations);
  auto replacement = laghu::core::internal::HandleTestAccess::adopt_file(45, operations);
  assigned = std::move(replacement);
  if (!check(!replacement.is_valid() && assigned.native_handle() == 45 && recorder.calls == 4 &&
             recorder.last_descriptor == 44 && assigned.close().has_value() && recorder.calls == 5)) {
    active_recorder = nullptr;
    return false;
  }

  active_recorder = nullptr;
  return true;
}

[[nodiscard]] bool check_failure_lifecycle() noexcept {
  CloseRecorder recorder{0, -1, -1, EINTR};
  active_recorder = &recorder;
  const laghu::core::internal::DescriptorOperations operations{record_close};
  auto interrupted = laghu::core::internal::HandleTestAccess::adopt_file(51, operations);
  const auto interrupted_result = interrupted.close();
  if (!check(!interrupted_result.has_value() && !interrupted.is_valid() && recorder.calls == 1 &&
             recorder.last_descriptor == 51 && interrupted_result.error().native_code() == EINTR &&
             interrupted.close().has_value() && recorder.calls == 1)) {
    active_recorder = nullptr;
    return false;
  }

  recorder.result = -1;
  recorder.error = EIO;
  auto old = laghu::core::internal::HandleTestAccess::adopt_file(52, operations);
  auto replacement = laghu::core::internal::HandleTestAccess::adopt_file(53, operations);
  const auto reset_result = old.reset(std::move(replacement));
  if (!check(!reset_result.has_value() && reset_result.error().native_code() == EIO &&
             old.is_valid() && old.native_handle() == 53 && !replacement.is_valid() &&
             recorder.calls == 2 && recorder.last_descriptor == 52)) {
    active_recorder = nullptr;
    return false;
  }
  recorder.result = 0;
  if (!check(old.close().has_value() && recorder.calls == 3 && recorder.last_descriptor == 53)) {
    active_recorder = nullptr;
    return false;
  }

  recorder.result = -1;
  recorder.error = EINTR;
  {
    auto best_effort = laghu::core::internal::HandleTestAccess::adopt_file(54, operations);
    if (!check(best_effort.is_valid())) {
      active_recorder = nullptr;
      return false;
    }
  }
  const bool destructor_closed_once = recorder.calls == 4 && recorder.last_descriptor == 54;
  active_recorder = nullptr;
  return destructor_closed_once;
}

[[nodiscard]] bool check_native_file_and_socket() noexcept {
  int pipe_descriptors[2]{};
  if (::pipe(pipe_descriptors) != 0) {
    return false;
  }
  auto adopted_file = laghu::core::FileHandle::adopt(pipe_descriptors[0]);
  if (!adopted_file.has_value()) {
    static_cast<void>(::close(pipe_descriptors[0]));
    static_cast<void>(::close(pipe_descriptors[1]));
    return false;
  }
  auto file{std::move(*adopted_file)};
  const auto borrowed_file = file.borrow();
  const int released = file.release();
  const bool release_keeps_open = borrowed_file.is_valid() && released == pipe_descriptors[0] &&
                                  !file.is_valid() && ::fcntl(released, F_GETFD) != -1;
  static_cast<void>(::close(released));
  static_cast<void>(::close(pipe_descriptors[1]));
  if (!release_keeps_open) {
    return false;
  }

  const int socket_descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_descriptor < 0) {
    return false;
  }
  auto adopted_socket = laghu::core::SocketHandle::adopt(socket_descriptor);
  if (!adopted_socket.has_value()) {
    static_cast<void>(::close(socket_descriptor));
    return false;
  }
  auto socket{std::move(*adopted_socket)};
  const auto borrowed_socket = socket.borrow();
  const auto socket_close = socket.close();
  return borrowed_socket.is_valid() && socket_close.has_value() && !socket.is_valid() &&
         is_closed(socket_descriptor);
}

static_assert(!std::is_copy_constructible_v<laghu::core::FileHandle>);
static_assert(!std::is_copy_assignable_v<laghu::core::FileHandle>);
static_assert(std::is_nothrow_move_constructible_v<laghu::core::FileHandle>);
static_assert(std::is_nothrow_move_assignable_v<laghu::core::FileHandle>);
static_assert(!std::is_convertible_v<laghu::core::FileHandle, laghu::core::SocketHandle>);
static_assert(!std::is_convertible_v<laghu::core::BorrowedFileHandle,
                                     laghu::core::BorrowedSocketHandle>);

}  // namespace

int main() {
  const std::size_t allocation_attempts_before = allocation_attempts;
  if (!check(check_invalid_and_adoption())) {
    return 1;
  }
  if (!check(check_injected_lifecycle())) {
    return 2;
  }
  if (!check(check_failure_lifecycle())) {
    return 3;
  }
  if (!check(check_native_file_and_socket())) {
    return 4;
  }
  return allocation_attempts == allocation_attempts_before ? 0 : 5;
}
