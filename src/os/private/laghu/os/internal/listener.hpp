// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>

#include <sys/socket.h>

#include <laghu/os/listener.hpp>

namespace laghu::os::internal {

using SocketFunction = int (*)(void*, int, int, int) noexcept;
using FcntlFunction = int (*)(void*, int, int, int) noexcept;
using SetSocketOptionFunction = int (*)(void*, int, int, int, const void*,
                                        socklen_t) noexcept;
using GetSocketOptionFunction = int (*)(void*, int, int, int, void*,
                                        socklen_t*) noexcept;
using BindFunction = int (*)(void*, int, const sockaddr*, socklen_t) noexcept;
using ConnectFunction = int (*)(void*, int, const sockaddr*, socklen_t) noexcept;
using ListenFunction = int (*)(void*, int, int) noexcept;
using GetSocketNameFunction = int (*)(void*, int, sockaddr*, socklen_t*) noexcept;
using PathStatusFunction = int (*)(void*, const char*, bool*, bool*, std::uint64_t*,
                                   std::uint64_t*) noexcept;
using PathModeFunction = int (*)(void*, const char*, std::uint16_t) noexcept;
using UnlinkFunction = int (*)(void*, const char*) noexcept;

struct ListenerOperations final {
  void* context;
  SocketFunction socket;
  FcntlFunction fcntl;
  SetSocketOptionFunction set_socket_option;
  GetSocketOptionFunction get_socket_option;
  BindFunction bind;
  ConnectFunction connect;
  ListenFunction listen;
  GetSocketNameFunction get_socket_name;
  PathStatusFunction path_status;
  PathModeFunction set_path_mode;
  UnlinkFunction unlink;
};

[[nodiscard]] const ListenerOperations& default_listener_operations() noexcept;

class ListenerTestAccess final {
 public:
  [[nodiscard]] static core::Result<Listener> create_ipv4(
      const Ipv4ListenerConfig& config,
      const ListenerOperations& operations) noexcept {
    return Listener::create_ipv4(config, operations);
  }

  [[nodiscard]] static core::Result<Listener> create_unix(
      const UnixListenerConfig& config,
      const ListenerOperations& operations) noexcept {
    return Listener::create_unix(config, operations);
  }
};

}  // namespace laghu::os::internal
