// SPDX-License-Identifier: AGPL-3.0-only
#include <cerrno>
#include <cstdint>
#include <limits>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <laghu/core/mapped_regions.hpp>

#include <laghu/core/internal/mapping_operations.hpp>

namespace laghu::core {
namespace {

[[nodiscard]] Error mapping_error(ErrorCode code, const char* diagnostic) noexcept {
  return Error{ErrorDomain::core, code, 0, diagnostic};
}

[[nodiscard]] bool is_valid_access(MappingAccess access) noexcept {
  return access == MappingAccess::read_only || access == MappingAccess::read_write;
}

[[nodiscard]] bool is_valid_flush(MappingFlush mode) noexcept {
  return mode == MappingFlush::synchronous || mode == MappingFlush::asynchronous;
}

[[nodiscard]] void* map_region(void*, int descriptor, std::size_t size, std::uint64_t offset,
                                MappingAccess access) noexcept {
  const int protection = access == MappingAccess::read_write ? PROT_READ | PROT_WRITE : PROT_READ;
  return ::mmap(nullptr, size, protection, MAP_SHARED, descriptor, static_cast<off_t>(offset));
}

[[nodiscard]] int unmap_region(void*, void* address, std::size_t size) noexcept {
  return ::munmap(address, size);
}

[[nodiscard]] int flush_region(void*, void* address, std::size_t size, MappingFlush mode) noexcept {
  const int flags = mode == MappingFlush::synchronous ? MS_SYNC : MS_ASYNC;
  return ::msync(address, size, flags);
}

[[nodiscard]] int protect_region(void*, void* address, std::size_t size, MappingAccess access) noexcept {
  const int protection = access == MappingAccess::read_write ? PROT_READ | PROT_WRITE : PROT_READ;
  return ::mprotect(address, size, protection);
}

[[nodiscard]] long mapping_page_size(void*) noexcept { return ::sysconf(_SC_PAGESIZE); }

[[nodiscard]] int mapping_file_size(void*, int descriptor, std::uint64_t* output) noexcept {
  struct stat information {};
  if (::fstat(descriptor, &information) != 0) {
    return -1;
  }
  if (information.st_size < 0) {
    errno = EOVERFLOW;
    return -1;
  }
  *output = static_cast<std::uint64_t>(information.st_size);
  return 0;
}

[[nodiscard]] int open_shared_memory(void*, const char* name, MappingAccess access) noexcept {
  const int flags = access == MappingAccess::read_write ? O_RDWR : O_RDONLY;
  return ::shm_open(name, flags, 0);
}

const internal::MappingOperations default_operations{
    nullptr,             map_region,          unmap_region,        flush_region,       protect_region,
    mapping_page_size,   mapping_file_size,   open_shared_memory, MAP_FAILED,
};

[[nodiscard]] Result<void> validate_mapping_request(FileHandle& file, std::size_t size,
                                                     MappingAccess access, std::uint64_t offset,
                                                     const internal::MappingOperations& operations) noexcept {
  if (!file.is_valid()) {
    return std::unexpected{mapping_error(ErrorCode::invalid_input,
                                         "mapping requires an owned file descriptor")};
  }
  if (size == 0) {
    return std::unexpected{mapping_error(ErrorCode::invalid_range,
                                         "mapping size must be nonzero")};
  }
  if (!is_valid_access(access)) {
    return std::unexpected{mapping_error(ErrorCode::invalid_input,
                                         "mapping access is invalid")};
  }
  if (operations.map == nullptr || operations.unmap == nullptr || operations.flush == nullptr ||
      operations.protect == nullptr || operations.page_size == nullptr ||
      operations.file_size == nullptr || operations.failed_mapping == nullptr) {
    return std::unexpected{mapping_error(ErrorCode::invalid_state,
                                         "mapping operations are incomplete")};
  }
  const long page_size = operations.page_size(operations.context);
  if (page_size <= 0) {
    return std::unexpected{Error::from_errno(errno, "mapping page size query failed")};
  }
  const auto page_size_u64 = checked_narrow<std::uint64_t>(page_size);
  if (!page_size_u64.has_value()) {
    return std::unexpected{page_size_u64.error()};
  }
  if (offset % *page_size_u64 != 0) {
    return std::unexpected{mapping_error(ErrorCode::invalid_range,
                                         "mapping offset must be page aligned")};
  }
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    return std::unexpected{mapping_error(ErrorCode::overflow,
                                         "mapping offset exceeds POSIX offset range")};
  }
  std::uint64_t source_size{};
  if (operations.file_size(operations.context, file.native_handle(), &source_size) != 0) {
    return std::unexpected{Error::from_errno(errno, "mapping source size query failed")};
  }
  const auto size_u64 = checked_narrow<std::uint64_t>(size);
  if (!size_u64.has_value()) {
    return std::unexpected{size_u64.error()};
  }
  const auto expected_size = checked_add(offset, *size_u64);
  if (!expected_size.has_value()) {
    return std::unexpected{expected_size.error()};
  }
  if (source_size != *expected_size) {
    return std::unexpected{mapping_error(ErrorCode::invalid_range,
                                         "mapping source does not have the required fixed size")};
  }
  return {};
}

[[nodiscard]] Result<std::pair<void*, std::size_t>> page_rounded_range(
    std::byte* mapping_address, std::size_t mapping_size,
    const internal::MappingOperations* operations, std::size_t offset,
    std::size_t length) noexcept {
  if (mapping_address == nullptr || operations == nullptr) {
    return std::unexpected{mapping_error(ErrorCode::invalid_state, "mapping is closed")};
  }
  if (length == 0) {
    return std::unexpected{mapping_error(ErrorCode::invalid_range,
                                         "mapping operation range must be nonzero")};
  }
  if (const auto range = checked_range(offset, length, mapping_size); !range.has_value()) {
    return std::unexpected{range.error()};
  }

  const long page_size = operations->page_size(operations->context);
  if (page_size <= 0) {
    return std::unexpected{Error::from_errno(errno, "mapping page size query failed")};
  }
  const auto page = checked_narrow<std::size_t>(page_size);
  if (!page.has_value()) {
    return std::unexpected{page.error()};
  }
  const std::size_t start = offset - offset % *page;
  const auto end = checked_add(offset, length);
  if (!end.has_value()) {
    return std::unexpected{end.error()};
  }
  const std::size_t remainder = *end % *page;
  const auto rounded_end = remainder == 0 ? *end : checked_add(*end, *page - remainder);
  if (!rounded_end.has_value()) {
    return std::unexpected{rounded_end.error()};
  }
  const std::size_t mapping_remainder = mapping_size % *page;
  const auto rounded_mapping_size = mapping_remainder == 0
                                        ? mapping_size
                                        : checked_add(mapping_size, *page - mapping_remainder);
  if (!rounded_mapping_size.has_value()) {
    return std::unexpected{rounded_mapping_size.error()};
  }
  if (*rounded_end > *rounded_mapping_size) {
    return std::unexpected{mapping_error(ErrorCode::invalid_range,
                                         "mapping operation exceeds mapped pages")};
  }
  const auto base = reinterpret_cast<std::uintptr_t>(mapping_address);
  const auto address = checked_add(base, start);
  if (!address.has_value()) {
    return std::unexpected{address.error()};
  }
  return std::pair<void*, std::size_t>{reinterpret_cast<void*>(*address), *rounded_end - start};
}

}  // namespace

namespace internal {

const MappingOperations& default_mapping_operations() noexcept { return default_operations; }

MappedRegion MappedRegionTestAccess::adopt(std::byte* address, std::size_t size,
                                           MappingAccess access, FileHandle&& file,
                                           const MappingOperations& operations) noexcept {
  return MappedRegion{address, size, access, static_cast<FileHandle&&>(file), &operations};
}

Result<MappedRegion> MappedRegionTestAccess::map(FileHandle&& file, std::size_t size,
                                                  MappingAccess access, std::uint64_t offset,
                                                  const MappingOperations& operations) noexcept {
  return MappedRegion::map_with_operations(static_cast<FileHandle&&>(file), size, access, offset,
                                           operations);
}

}  // namespace internal

Result<ByteView> MappedRegionView::readable() const noexcept {
  if (!is_valid()) {
    return std::unexpected{mapping_error(ErrorCode::invalid_state, "mapped view is invalid")};
  }
  return ByteView::from(std::span<const std::byte>{address_, size_});
}

Result<MutableByteView> MappedRegionView::writable() const noexcept {
  if (!is_valid()) {
    return std::unexpected{mapping_error(ErrorCode::invalid_state, "mapped view is invalid")};
  }
  if (!writable_) {
    return std::unexpected{mapping_error(ErrorCode::invalid_state, "mapped view is read only")};
  }
  return MutableByteView::from(std::span<std::byte>{address_, size_});
}

Result<MappedRegionView> MappedRegionView::slice(std::size_t offset, std::size_t length) const noexcept {
  if (!is_valid()) {
    return std::unexpected{mapping_error(ErrorCode::invalid_state, "mapped view is invalid")};
  }
  if (const auto range = checked_range(offset, length, size_); !range.has_value()) {
    return std::unexpected{range.error()};
  }
  return MappedRegionView{address_ + offset, length, writable_};
}

MappedRegion::MappedRegion(std::byte* address, std::size_t size, MappingAccess access,
                           FileHandle&& file,
                           const internal::MappingOperations* operations) noexcept
    : address_(address), size_(size), access_(access), file_(static_cast<FileHandle&&>(file)),
      operations_(operations) {}

MappedRegion::MappedRegion(MappedRegion&& other) noexcept {
  move_from(static_cast<MappedRegion&&>(other));
}

MappedRegion& MappedRegion::operator=(MappedRegion&& other) noexcept {
  if (this != &other) {
    if (!close().has_value()) {
      return *this;
    }
    move_from(static_cast<MappedRegion&&>(other));
  }
  return *this;
}

MappedRegion::~MappedRegion() { static_cast<void>(close()); }

Result<MappedRegion> MappedRegion::map_file(FileHandle&& file, std::size_t size,
                                             MappingAccess access, std::uint64_t offset) noexcept {
  return map_with_operations(static_cast<FileHandle&&>(file), size, access, offset,
                             internal::default_mapping_operations());
}

Result<MappedRegion> MappedRegion::map_shared_memory_name(const char* name, std::size_t name_length,
                                                           std::size_t size,
                                                           MappingAccess access) noexcept {
  if (name == nullptr || name_length < 2 || name[0] != '/') {
    return std::unexpected{mapping_error(ErrorCode::invalid_input,
                                         "shared memory name must begin with one slash")};
  }
  for (std::size_t index = 1; index < name_length; ++index) {
    if (name[index] == '/') {
      return std::unexpected{mapping_error(ErrorCode::invalid_input,
                                           "shared memory name cannot contain another slash")};
    }
  }
  if (!is_valid_access(access)) {
    return std::unexpected{mapping_error(ErrorCode::invalid_input,
                                         "mapping access is invalid")};
  }
  const internal::MappingOperations& operations = internal::default_mapping_operations();
  const int descriptor = operations.open_shared_memory(operations.context, name, access);
  if (descriptor < 0) {
    return std::unexpected{Error::from_errno(errno, "shared memory open failed")};
  }
  auto file = FileHandle::adopt(descriptor);
  if (!file.has_value()) {
    static_cast<void>(::close(descriptor));
    return std::unexpected{file.error()};
  }
  return map_with_operations(std::move(*file), size, access, 0, operations);
}

Result<MappedRegion> MappedRegion::map_with_operations(
    FileHandle&& file, std::size_t size, MappingAccess access, std::uint64_t offset,
    const internal::MappingOperations& operations) noexcept {
  if (const auto validation = validate_mapping_request(file, size, access, offset, operations);
      !validation.has_value()) {
    return std::unexpected{validation.error()};
  }
  void* const address = operations.map(operations.context, file.native_handle(), size, offset, access);
  if (address == nullptr || address == operations.failed_mapping) {
    return std::unexpected{Error::from_errno(errno, "mapping creation failed")};
  }
  return MappedRegion{static_cast<std::byte*>(address), size, access, static_cast<FileHandle&&>(file),
                      &operations};
}

Result<MappedRegionView> MappedRegion::view() const noexcept {
  if (!is_mapped()) {
    return std::unexpected{mapping_error(ErrorCode::invalid_state, "mapping is closed")};
  }
  return MappedRegionView{address_, size_, access_ == MappingAccess::read_write};
}

Result<void> MappedRegion::flush(std::size_t offset, std::size_t length,
                                 MappingFlush mode) noexcept {
  if (!is_valid_flush(mode)) {
    return std::unexpected{mapping_error(ErrorCode::invalid_input, "mapping flush mode is invalid")};
  }
  if (operations_ == nullptr || operations_->flush == nullptr || operations_->page_size == nullptr) {
    return std::unexpected{mapping_error(ErrorCode::invalid_state,
                                         "mapping has no flush operation")};
  }
  const auto range = page_rounded_range(address_, size_, operations_, offset, length);
  if (!range.has_value()) {
    return std::unexpected{range.error()};
  }
  if (operations_->flush(operations_->context, range->first, range->second, mode) != 0) {
    return std::unexpected{Error::from_errno(errno, "mapping flush failed")};
  }
  return {};
}

Result<void> MappedRegion::protect(MappingAccess access) noexcept {
  if (!is_mapped()) {
    return std::unexpected{mapping_error(ErrorCode::invalid_state, "mapping is closed")};
  }
  if (!is_valid_access(access)) {
    return std::unexpected{mapping_error(ErrorCode::invalid_input,
                                         "mapping access is invalid")};
  }
  if (operations_ == nullptr || operations_->protect == nullptr) {
    return std::unexpected{mapping_error(ErrorCode::invalid_state,
                                         "mapping has no protection operation")};
  }
  if (operations_->protect(operations_->context, address_, size_, access) != 0) {
    return std::unexpected{Error::from_errno(errno, "mapping protection change failed")};
  }
  access_ = access;
  return {};
}

Result<FileHandle> MappedRegion::release() noexcept {
  if (!is_mapped()) {
    return std::unexpected{mapping_error(ErrorCode::invalid_state, "mapping is closed")};
  }
  if (operations_ == nullptr || operations_->unmap == nullptr) {
    return std::unexpected{mapping_error(ErrorCode::invalid_state,
                                         "mapping has no unmap operation")};
  }
  if (operations_->unmap(operations_->context, address_, size_) != 0) {
    return std::unexpected{Error::from_errno(errno, "mapping release failed")};
  }
  address_ = nullptr;
  size_ = 0;
  access_ = MappingAccess::read_only;
  operations_ = nullptr;
  return std::move(file_);
}

Result<void> MappedRegion::close() noexcept {
  if (!is_mapped()) {
    return file_.close();
  }
  auto file = release();
  if (!file.has_value()) {
    return std::unexpected{file.error()};
  }
  return file->close();
}

void MappedRegion::move_from(MappedRegion&& other) noexcept {
  address_ = other.address_;
  size_ = other.size_;
  access_ = other.access_;
  file_ = std::move(other.file_);
  operations_ = other.operations_;
  other.address_ = nullptr;
  other.size_ = 0;
  other.access_ = MappingAccess::read_only;
  other.operations_ = nullptr;
}

}  // namespace laghu::core
