// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <laghu/core/handles.hpp>

namespace laghu::core::internal {

using DescriptorCloseFunction = int (*)(void* context, int) noexcept;

struct DescriptorOperations final {
  void* context;
  DescriptorCloseFunction close;
};

[[nodiscard]] const DescriptorOperations& default_descriptor_operations() noexcept;

class HandleTestAccess final {
 public:
  [[nodiscard]] static FileHandle adopt_file(int descriptor,
                                               const DescriptorOperations& operations) noexcept;
  [[nodiscard]] static SocketHandle adopt_socket(
      int descriptor, const DescriptorOperations& operations) noexcept;
};

}  // namespace laghu::core::internal
