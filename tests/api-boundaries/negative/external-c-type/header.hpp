// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <sys/socket.h>

namespace laghu::core {
[[nodiscard]] sockaddr leaked_socket_address() noexcept;
}  // namespace laghu::core
