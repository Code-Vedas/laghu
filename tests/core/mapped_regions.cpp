// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <laghu/core/mapped_regions.hpp>
#include <laghu/core/shared_offsets.hpp>

#include <laghu/core/internal/mapping_operations.hpp>

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

using laghu::core::ByteView;
using laghu::core::ErrorCode;
using laghu::core::FileHandle;
using laghu::core::MappedRegion;
using laghu::core::MappingAccess;
using laghu::core::MappingFlush;
using laghu::core::MutableByteView;
using laghu::core::Result;
using laghu::core::SharedOffset;
using laghu::core::StaticCString;
using laghu::core::TextView;
using laghu::core::internal::MappedRegionTestAccess;
using laghu::core::internal::MappingOperations;

constexpr std::size_t mapping_size = 4096;
constexpr const char shared_memory_name[] = "/laghu-core-mapped-regions";

struct SharedRecord final {
  std::uint64_t value;
};

struct SharedRecordTag final {};

struct ChildResult final {
  std::uintptr_t address;
  std::uint64_t value;
};

static_assert(std::is_standard_layout_v<SharedRecord>);
static_assert(std::is_trivially_copyable_v<SharedRecord>);

[[nodiscard]] bool is_closed(int descriptor) noexcept {
  errno = 0;
  return ::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF;
}

[[nodiscard]] int create_file() noexcept {
  char path[] = "/tmp/laghu-mapped-regions.XXXXXX";
  const int descriptor = ::mkstemp(path);
  if (descriptor >= 0) {
    static_cast<void>(::unlink(path));
  }
  return descriptor;
}

[[nodiscard]] bool resize_file(int descriptor, std::size_t size) noexcept {
  const auto narrowed = laghu::core::checked_narrow<off_t>(size);
  return narrowed.has_value() && ::ftruncate(descriptor, *narrowed) == 0;
}

[[nodiscard]] bool check_file_mapping_lifecycle() noexcept {
  const int descriptor = create_file();
  if (descriptor < 0 || !resize_file(descriptor, mapping_size)) {
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
    return false;
  }
  auto file = FileHandle::adopt(descriptor);
  if (!file.has_value()) {
    static_cast<void>(::close(descriptor));
    return false;
  }
  auto mapped = MappedRegion::map_file(std::move(*file), mapping_size, MappingAccess::read_write);
  if (!mapped.has_value() || file->is_valid()) {
    return false;
  }
  auto region = std::move(*mapped);
  if (mapped->is_mapped()) {
    return false;
  }
  const auto view = region.view();
  if (!view.has_value() || !view->is_writable()) {
    return false;
  }
  const auto writable = view->writable();
  if (!writable.has_value() || writable->size() != mapping_size) {
    return false;
  }
  writable->span()[0] = std::byte{0x42};
  writable->span()[mapping_size - 1] = std::byte{0x24};
  const auto slice = view->slice(1, 5);
  const auto invalid_slice = view->slice(mapping_size, 1);
  if (!slice.has_value() || slice->size() != 5 || invalid_slice.has_value() ||
      invalid_slice.error().code() != ErrorCode::invalid_range ||
      !region.flush(1, 2, MappingFlush::synchronous).has_value() ||
      !region.flush(1, 2, MappingFlush::asynchronous).has_value() ||
      region.flush(0, 0, MappingFlush::synchronous).has_value() ||
      region.flush(1, std::numeric_limits<std::size_t>::max(), MappingFlush::synchronous)
          .has_value()) {
    return false;
  }
  if (!region.protect(MappingAccess::read_only).has_value() ||
      region.access() != MappingAccess::read_only) {
    return false;
  }
  const auto read_only_view = region.view();
  if (!read_only_view.has_value() || read_only_view->writable().has_value() ||
      !region.protect(MappingAccess::read_write).has_value()) {
    return false;
  }
  const int released_descriptor = descriptor;
  auto released = region.release();
  return released.has_value() && !region.is_mapped() && released->is_valid() &&
         released->native_handle() == released_descriptor && released->close().has_value() &&
         is_closed(released_descriptor) && !region.release().has_value();
}

[[nodiscard]] bool check_fixed_size_and_destructor() noexcept {
  const int too_small_descriptor = create_file();
  if (too_small_descriptor < 0 || !resize_file(too_small_descriptor, mapping_size - 1)) {
    if (too_small_descriptor >= 0) {
      static_cast<void>(::close(too_small_descriptor));
    }
    return false;
  }
  auto too_small_file = FileHandle::adopt(too_small_descriptor);
  if (!too_small_file.has_value()) {
    static_cast<void>(::close(too_small_descriptor));
    return false;
  }
  const auto rejected = MappedRegion::map_file(std::move(*too_small_file), mapping_size,
                                                MappingAccess::read_write);
  if (rejected.has_value() || !too_small_file->is_valid() ||
      !too_small_file->close().has_value()) {
    return false;
  }

  const int descriptor = create_file();
  if (descriptor < 0 || !resize_file(descriptor, mapping_size)) {
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
    return false;
  }
  auto file = FileHandle::adopt(descriptor);
  if (!file.has_value()) {
    static_cast<void>(::close(descriptor));
    return false;
  }
  {
    auto mapped = MappedRegion::map_file(std::move(*file), mapping_size,
                                          MappingAccess::read_write);
    if (!mapped.has_value()) {
      return false;
    }
    [[maybe_unused]] MappedRegion region{std::move(*mapped)};
  }
  return is_closed(descriptor);
}

struct FakeState final {
  int map_calls{};
  int unmap_calls{};
  int flush_calls{};
  int protect_calls{};
  int map_result{};
  int unmap_result{};
  int flush_result{};
  int protect_result{};
  int error{EIO};
  std::uint64_t file_size{mapping_size};
  void* last_address{};
  std::size_t last_size{};
  MappingFlush last_flush{MappingFlush::synchronous};
  std::array<std::byte, mapping_size * 2> storage{};
};

FakeState* fake_state{};

[[nodiscard]] void* fake_map(int, std::size_t, std::uint64_t, MappingAccess) noexcept {
  ++fake_state->map_calls;
  if (fake_state->map_result != 0) {
    errno = fake_state->error;
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
  }
  return fake_state->storage.data();
}

[[nodiscard]] int fake_unmap(void* address, std::size_t size) noexcept {
  ++fake_state->unmap_calls;
  fake_state->last_address = address;
  fake_state->last_size = size;
  errno = fake_state->error;
  return fake_state->unmap_result;
}

[[nodiscard]] int fake_flush(void* address, std::size_t size, MappingFlush mode) noexcept {
  ++fake_state->flush_calls;
  fake_state->last_address = address;
  fake_state->last_size = size;
  fake_state->last_flush = mode;
  errno = fake_state->error;
  return fake_state->flush_result;
}

[[nodiscard]] int fake_protect(void* address, std::size_t size, MappingAccess) noexcept {
  ++fake_state->protect_calls;
  fake_state->last_address = address;
  fake_state->last_size = size;
  errno = fake_state->error;
  return fake_state->protect_result;
}

[[nodiscard]] long fake_page_size() noexcept { return 4096; }

[[nodiscard]] int fake_file_size(int, std::uint64_t* output) noexcept {
  *output = fake_state->file_size;
  return 0;
}

[[nodiscard]] int fake_open_shared_memory(const char*, MappingAccess) noexcept {
  errno = ENOENT;
  return -1;
}

[[nodiscard]] MappingOperations fake_operations() noexcept {
  return MappingOperations{fake_map,
                           fake_unmap,
                           fake_flush,
                           fake_protect,
                           fake_page_size,
                           fake_file_size,
                           fake_open_shared_memory,
                           reinterpret_cast<void*>(static_cast<std::uintptr_t>(1))};
}

[[nodiscard]] Result<FileHandle> disposable_file() noexcept {
  const int descriptor = ::open("/dev/null", O_RDONLY);
  if (descriptor < 0) {
    return std::unexpected{laghu::core::Error::from_errno(errno, "test descriptor open failed")};
  }
  return FileHandle::adopt(descriptor);
}

[[nodiscard]] bool check_injected_failures_and_metadata() noexcept {
  FakeState state{};
  fake_state = &state;
  const MappingOperations operations = fake_operations();
  auto first_file = disposable_file();
  if (!first_file.has_value()) {
    fake_state = nullptr;
    return false;
  }
  state.map_result = -1;
  const auto map_failure = MappedRegionTestAccess::map(std::move(*first_file), mapping_size,
                                                        MappingAccess::read_write, 0, operations);
  if (map_failure.has_value() || !first_file->is_valid() || state.map_calls != 1 ||
      !first_file->close().has_value()) {
    fake_state = nullptr;
    return false;
  }
  auto unaligned_file = disposable_file();
  if (!unaligned_file.has_value()) {
    fake_state = nullptr;
    return false;
  }
  const auto unaligned = MappedRegionTestAccess::map(std::move(*unaligned_file), mapping_size,
                                                      MappingAccess::read_write, 1, operations);
  if (unaligned.has_value() || !unaligned_file->is_valid() || state.map_calls != 1 ||
      !unaligned_file->close().has_value()) {
    fake_state = nullptr;
    return false;
  }

  auto second_file = disposable_file();
  if (!second_file.has_value()) {
    fake_state = nullptr;
    return false;
  }
  state.map_result = 0;
  auto mapped = MappedRegionTestAccess::map(std::move(*second_file), mapping_size,
                                            MappingAccess::read_write, 0, operations);
  if (!mapped.has_value() || second_file->is_valid()) {
    fake_state = nullptr;
    return false;
  }
  auto region = std::move(*mapped);
  if (!region.flush(1, 2, MappingFlush::asynchronous).has_value() || state.flush_calls != 1 ||
      state.last_address != state.storage.data() || state.last_size != 4096 ||
      state.last_flush != MappingFlush::asynchronous) {
    fake_state = nullptr;
    return false;
  }
  state.flush_result = -1;
  if (region.flush(1, 2, MappingFlush::synchronous).has_value() || !region.is_mapped() ||
      state.flush_calls != 2) {
    fake_state = nullptr;
    return false;
  }
  state.flush_result = 0;
  state.protect_result = -1;
  const auto protection_failure = region.protect(MappingAccess::read_only);
  if (protection_failure.has_value() || region.access() != MappingAccess::read_write ||
      state.protect_calls != 1) {
    fake_state = nullptr;
    return false;
  }
  state.protect_result = 0;
  if (!region.protect(MappingAccess::read_only).has_value() ||
      region.access() != MappingAccess::read_only || state.protect_calls != 2) {
    fake_state = nullptr;
    return false;
  }
  state.unmap_result = -1;
  const auto release_failure = region.release();
  if (release_failure.has_value() || !region.is_mapped() || state.unmap_calls != 1) {
    fake_state = nullptr;
    return false;
  }
  state.unmap_result = 0;
  auto released = region.release();
  const bool success = released.has_value() && !region.is_mapped() && state.unmap_calls == 2 &&
                       released->close().has_value();
  fake_state = nullptr;
  return success;
}

[[nodiscard]] Result<StaticCString<64>> shared_name() noexcept {
  const auto text = TextView::from(shared_memory_name, std::strlen(shared_memory_name));
  if (!text.has_value()) {
    return std::unexpected{text.error()};
  }
  return text->to_c_string<64>();
}

[[nodiscard]] bool write_child_result(int descriptor, const ChildResult& result) noexcept {
  const auto* bytes = static_cast<const std::byte*>(static_cast<const void*>(&result));
  std::size_t written{};
  while (written < sizeof(result)) {
    const ssize_t outcome = ::write(descriptor, bytes + written, sizeof(result) - written);
    if (outcome <= 0) {
      return false;
    }
    written += static_cast<std::size_t>(outcome);
  }
  return true;
}

[[nodiscard]] bool read_child_result(int descriptor, ChildResult& result) noexcept {
  auto* bytes = static_cast<std::byte*>(static_cast<void*>(&result));
  std::size_t read{};
  while (read < sizeof(result)) {
    const ssize_t outcome = ::read(descriptor, bytes + read, sizeof(result) - read);
    if (outcome <= 0) {
      return false;
    }
    read += static_cast<std::size_t>(outcome);
  }
  return true;
}

[[nodiscard]] bool check_cross_process_shared_offset() noexcept {
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    return false;
  }
  const auto shared_size_result = laghu::core::checked_narrow<std::size_t>(page_size);
  if (!shared_size_result.has_value()) {
    return false;
  }
  const std::size_t shared_size = *shared_size_result;
  static_cast<void>(::shm_unlink(shared_memory_name));
  const int creator = ::shm_open(shared_memory_name, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (creator < 0 || !resize_file(creator, shared_size)) {
    if (creator >= 0) {
      static_cast<void>(::close(creator));
    }
    static_cast<void>(::shm_unlink(shared_memory_name));
    return false;
  }
  const auto name = shared_name();
  if (!name.has_value()) {
    static_cast<void>(::close(creator));
    static_cast<void>(::shm_unlink(shared_memory_name));
    return false;
  }
  struct stat creator_information {};
  const auto expected_size = laghu::core::checked_narrow<off_t>(shared_size);
  if (!expected_size.has_value() || ::fstat(creator, &creator_information) != 0 ||
      creator_information.st_size != *expected_size) {
    static_cast<void>(::close(creator));
    static_cast<void>(::shm_unlink(shared_memory_name));
    return false;
  }
  auto mapped = MappedRegion::map_shared_memory(*name, shared_size, MappingAccess::read_write);
  if (!mapped.has_value()) {
    static_cast<void>(::close(creator));
    static_cast<void>(::shm_unlink(shared_memory_name));
    return false;
  }
  auto region = std::move(*mapped);
  const auto writable = region.view();
  if (!writable.has_value()) {
    static_cast<void>(::close(creator));
    static_cast<void>(::shm_unlink(shared_memory_name));
    return false;
  }
  const auto mutable_bytes = writable->writable();
  if (!mutable_bytes.has_value()) {
    static_cast<void>(::close(creator));
    static_cast<void>(::shm_unlink(shared_memory_name));
    return false;
  }
  constexpr std::size_t record_offset = 64;
  const auto record_offset_value = SharedOffset<SharedRecordTag>::from_raw(record_offset);
  const auto resolved_record = record_offset_value.resolve<SharedRecord>(*mutable_bytes);
  if (!resolved_record.has_value()) {
    static_cast<void>(::close(creator));
    static_cast<void>(::shm_unlink(shared_memory_name));
    return false;
  }
  auto* const record = *resolved_record;
  record->value = 0x8f4c'1020'7744'aa55ULL;
  if (!region.flush(record_offset, sizeof(*record), MappingFlush::synchronous).has_value()) {
    static_cast<void>(::close(creator));
    static_cast<void>(::shm_unlink(shared_memory_name));
    return false;
  }
  const auto parent_address = reinterpret_cast<std::uintptr_t>(mutable_bytes->data());
  int pipe_descriptors[2]{};
  if (::pipe(pipe_descriptors) != 0) {
    static_cast<void>(::close(creator));
    static_cast<void>(::shm_unlink(shared_memory_name));
    return false;
  }
  const pid_t child = ::fork();
  if (child < 0) {
    static_cast<void>(::close(pipe_descriptors[0]));
    static_cast<void>(::close(pipe_descriptors[1]));
    static_cast<void>(::close(creator));
    static_cast<void>(::shm_unlink(shared_memory_name));
    return false;
  }
  if (child == 0) {
    static_cast<void>(::close(pipe_descriptors[0]));
    const bool closed = region.close().has_value();
    void* const guard = ::mmap(reinterpret_cast<void*>(parent_address), shared_size, PROT_NONE,
                               MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    auto child_region = MappedRegion::map_shared_memory(*name, shared_size, MappingAccess::read_only);
    ChildResult child_result{};
    bool valid = closed && guard != MAP_FAILED && child_region.has_value();
    if (valid) {
      const auto child_view = child_region->view();
      valid = child_view.has_value();
      if (valid) {
        const auto bytes = child_view->readable();
        valid = bytes.has_value();
        if (valid) {
          const auto offset = SharedOffset<SharedRecordTag>::from_raw(record_offset);
          const auto resolved = offset.resolve<SharedRecord>(*bytes);
          valid = resolved.has_value();
          if (valid) {
            child_result.address = reinterpret_cast<std::uintptr_t>(bytes->data());
            child_result.value = (*resolved)->value;
          }
        }
      }
    }
    if (guard != MAP_FAILED) {
      static_cast<void>(::munmap(guard, shared_size));
    }
    static_cast<void>(::close(creator));
    const bool wrote = write_child_result(pipe_descriptors[1], child_result);
    static_cast<void>(::close(pipe_descriptors[1]));
    _exit(wrote && valid ? 0 : 1);
  }
  static_cast<void>(::close(pipe_descriptors[1]));
  ChildResult child_result{};
  const bool read = read_child_result(pipe_descriptors[0], child_result);
  static_cast<void>(::close(pipe_descriptors[0]));
  int status{};
  const bool waited = ::waitpid(child, &status, 0) == child;
  const bool child_success = waited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  const std::uint64_t parent_value = record->value;
  const bool region_closed = region.close().has_value();
  static_cast<void>(::close(creator));
  const bool unlinked = ::shm_unlink(shared_memory_name) == 0;
  return read && child_success && region_closed && unlinked &&
         child_result.address != parent_address && child_result.value == parent_value;
}

[[nodiscard]] bool check_no_allocations() noexcept {
  const int descriptor = create_file();
  if (descriptor < 0 || !resize_file(descriptor, mapping_size)) {
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
    return false;
  }
  auto file = FileHandle::adopt(descriptor);
  if (!file.has_value()) {
    static_cast<void>(::close(descriptor));
    return false;
  }
  const std::size_t before = allocation_attempts;
  auto mapped = MappedRegion::map_file(std::move(*file), mapping_size, MappingAccess::read_write);
  if (!mapped.has_value()) {
    return false;
  }
  auto region = std::move(*mapped);
  const auto view = region.view();
  const bool success = view.has_value() && view->slice(0, 1).has_value() &&
                       region.flush(0, 1, MappingFlush::synchronous).has_value() &&
                       region.close().has_value() && allocation_attempts == before;
  return success;
}

}  // namespace

int main() {
  return check_file_mapping_lifecycle() && check_fixed_size_and_destructor() &&
                 check_injected_failures_and_metadata() && check_cross_process_shared_offset() &&
                 check_no_allocations()
             ? 0
             : 1;
}
