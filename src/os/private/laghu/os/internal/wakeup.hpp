// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <laghu/os/wakeup.hpp>

namespace laghu::os::internal {

class WakeupTestAccess final {
 public:
  [[nodiscard]] static int write_descriptor(const WakeupChannel& channel) noexcept {
    return channel.write_descriptor();
  }
};

}  // namespace laghu::os::internal
