// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <laghu/core/handles.hpp>
#include <laghu/core/views.hpp>

namespace laghu::core {

namespace internal {
struct MappingOperations;
class MappedRegionTestAccess;
}  // namespace internal

enum class MappingAccess : std::uint8_t {
  read_only,
  read_write,
};

enum class MappingFlush : std::uint8_t {
  synchronous,
  asynchronous,
};

class MappedRegionView final {
 public:
  constexpr MappedRegionView() noexcept = default;

  [[nodiscard]] constexpr bool is_valid() const noexcept { return address_ != nullptr; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr bool is_writable() const noexcept { return writable_; }

  [[nodiscard]] Result<ByteView> readable() const noexcept;
  [[nodiscard]] Result<MutableByteView> writable() const noexcept;
  [[nodiscard]] Result<MappedRegionView> slice(std::size_t offset,
                                                std::size_t length) const noexcept;

 private:
  friend class MappedRegion;

  constexpr MappedRegionView(std::byte* address, std::size_t size, bool writable) noexcept
      : address_(address), size_(size), writable_(writable) {}

  std::byte* address_{nullptr};
  std::size_t size_{};
  bool writable_{};
};

class MappedRegion final {
 public:
  MappedRegion() noexcept = default;
  MappedRegion(const MappedRegion&) = delete;
  MappedRegion& operator=(const MappedRegion&) = delete;
  MappedRegion(MappedRegion&& other) noexcept;
  MappedRegion& operator=(MappedRegion&& other) noexcept;
  ~MappedRegion();

  // The source must be exactly offset + size bytes. The region takes ownership
  // of file only after mmap succeeds; a failed call leaves file with the caller.
  [[nodiscard]] static Result<MappedRegion> map_file(FileHandle&& file, std::size_t size,
                                                      MappingAccess access,
                                                      std::uint64_t offset = 0) noexcept;

  // The named shared-memory object must already exist at exactly size bytes.
  // Its name must begin with one slash and contain no additional slashes.
  template <std::size_t NameCapacity>
  [[nodiscard]] static Result<MappedRegion> map_shared_memory(
      const StaticCString<NameCapacity>& name, std::size_t size, MappingAccess access) noexcept {
    return map_shared_memory_name(name.c_str(), name.size(), size, access);
  }

  [[nodiscard]] constexpr bool is_mapped() const noexcept { return address_ != nullptr; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr MappingAccess access() const noexcept { return access_; }

  [[nodiscard]] Result<MappedRegionView> view() const noexcept;
  [[nodiscard]] Result<void> flush(std::size_t offset, std::size_t length,
                                   MappingFlush mode) noexcept;

  // This changes the entire mapping. All previously returned views are invalid
  // after a successful call and must not be used.
  [[nodiscard]] Result<void> protect(MappingAccess access) noexcept;

  // release unmaps the region before returning its owned descriptor. On an
  // unmap failure, this region retains both its mapping and descriptor.
  [[nodiscard]] Result<FileHandle> release() noexcept;
  [[nodiscard]] Result<void> close() noexcept;

 private:
  friend class internal::MappedRegionTestAccess;

  MappedRegion(std::byte* address, std::size_t size, MappingAccess access, FileHandle&& file,
               const internal::MappingOperations* operations) noexcept;

  [[nodiscard]] static Result<MappedRegion> map_with_operations(
      FileHandle&& file, std::size_t size, MappingAccess access, std::uint64_t offset,
      const internal::MappingOperations& operations) noexcept;
  [[nodiscard]] static Result<MappedRegion> map_shared_memory_name(const char* name,
                                                                    std::size_t name_length,
                                                                    std::size_t size,
                                                                    MappingAccess access) noexcept;
  void move_from(MappedRegion&& other) noexcept;

  std::byte* address_{nullptr};
  std::size_t size_{};
  MappingAccess access_{MappingAccess::read_only};
  FileHandle file_{};
  const internal::MappingOperations* operations_{nullptr};
};

static_assert(!std::is_copy_constructible_v<MappedRegion>);
static_assert(!std::is_copy_assignable_v<MappedRegion>);
static_assert(std::is_nothrow_move_constructible_v<MappedRegion>);
static_assert(std::is_nothrow_move_assignable_v<MappedRegion>);

}  // namespace laghu::core
