// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <laghu/core/handles.hpp>
#include <laghu/core/views.hpp>

namespace laghu::os {

namespace internal {
struct ListenerOperations;
class ListenerTestAccess;
}  // namespace internal

enum class ListenerKind : std::uint8_t {
  ipv4_tcp,
  ipv6_tcp,
  unix_stream,
};

struct TcpListenerOptions final {
  std::uint32_t backlog;
  bool reuse_address;
  bool reuse_port;
  bool keep_alive;
  bool tcp_no_delay;
};

struct Ipv4ListenerConfig final {
  std::array<std::uint8_t, 4> address;
  std::uint16_t port;
  TcpListenerOptions options;
};

struct Ipv6ListenerConfig final {
  std::array<std::uint8_t, 16> address;
  std::uint16_t port;
  std::uint32_t scope_id;
  TcpListenerOptions options;
};

struct UnixListenerConfig final {
  static constexpr std::size_t path_capacity = 104;

  core::TextView path;
  std::uint32_t backlog;
  std::uint16_t permissions;
  bool remove_stale_socket;
};

class Listener final {
 public:
  Listener() noexcept = default;
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  Listener(Listener&& other) noexcept;
  Listener& operator=(Listener&& other) noexcept;
  ~Listener();

  [[nodiscard]] static core::Result<Listener> create_ipv4(
      const Ipv4ListenerConfig& config) noexcept;
  [[nodiscard]] static core::Result<Listener> create_ipv6(
      const Ipv6ListenerConfig& config) noexcept;
  [[nodiscard]] static core::Result<Listener> create_unix(
      const UnixListenerConfig& config) noexcept;
  [[nodiscard]] static core::Result<Listener> adopt(
      core::SocketHandle&& socket, ListenerKind expected_kind) noexcept;

  [[nodiscard]] core::BorrowedSocketHandle borrow() const noexcept {
    return socket_.borrow();
  }
  [[nodiscard]] ListenerKind kind() const noexcept { return kind_; }
  [[nodiscard]] core::Result<void> close() noexcept;

 private:
  friend class internal::ListenerTestAccess;

  Listener(core::SocketHandle&& socket, ListenerKind kind) noexcept;
  [[nodiscard]] static core::Result<Listener> create_ipv4(
      const Ipv4ListenerConfig& config,
      const internal::ListenerOperations& operations) noexcept;
  [[nodiscard]] static core::Result<Listener> create_ipv6(
      const Ipv6ListenerConfig& config,
      const internal::ListenerOperations& operations) noexcept;
  [[nodiscard]] static core::Result<Listener> create_unix(
      const UnixListenerConfig& config,
      const internal::ListenerOperations& operations) noexcept;
  [[nodiscard]] static core::Result<Listener> adopt(
      core::SocketHandle&& socket, ListenerKind expected_kind,
      const internal::ListenerOperations& operations) noexcept;

  void move_from(Listener&& other) noexcept;

  core::SocketHandle socket_{};
  ListenerKind kind_{ListenerKind::ipv4_tcp};
  std::array<char, UnixListenerConfig::path_capacity> unix_path_{};
  bool owns_unix_path_{};
  const internal::ListenerOperations* operations_{};
};

}  // namespace laghu::os
