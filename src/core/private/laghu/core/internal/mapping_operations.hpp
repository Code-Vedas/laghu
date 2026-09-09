// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>

#include <laghu/core/mapped_regions.hpp>

namespace laghu::core::internal {

using MappingMapFunction = void* (*)(int, std::size_t, std::uint64_t, MappingAccess) noexcept;
using MappingUnmapFunction = int (*)(void*, std::size_t) noexcept;
using MappingFlushFunction = int (*)(void*, std::size_t, MappingFlush) noexcept;
using MappingProtectFunction = int (*)(void*, std::size_t, MappingAccess) noexcept;
using MappingPageSizeFunction = long (*)() noexcept;
using MappingFileSizeFunction = int (*)(int, std::uint64_t*) noexcept;
using SharedMemoryOpenFunction = int (*)(const char*, MappingAccess) noexcept;

struct MappingOperations final {
  MappingMapFunction map;
  MappingUnmapFunction unmap;
  MappingFlushFunction flush;
  MappingProtectFunction protect;
  MappingPageSizeFunction page_size;
  MappingFileSizeFunction file_size;
  SharedMemoryOpenFunction open_shared_memory;
  void* failed_mapping;
};

[[nodiscard]] const MappingOperations& default_mapping_operations() noexcept;

class MappedRegionTestAccess final {
 public:
  [[nodiscard]] static MappedRegion adopt(std::byte* address, std::size_t size,
                                           MappingAccess access, FileHandle&& file,
                                           const MappingOperations& operations) noexcept;
  [[nodiscard]] static Result<MappedRegion> map(FileHandle&& file, std::size_t size,
                                                 MappingAccess access, std::uint64_t offset,
                                                 const MappingOperations& operations) noexcept;
};

}  // namespace laghu::core::internal
