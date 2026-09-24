// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "laghu_test_support.hpp"

#include <laghu/os/internal/listener.hpp>
#include <laghu/os/listener.hpp>

namespace {

using laghu::core::SocketHandle;
using laghu::core::TextView;
using laghu::os::Ipv4ListenerConfig;
using laghu::os::Ipv6ListenerConfig;
using laghu::os::Listener;
using laghu::os::ListenerKind;
using laghu::os::TcpListenerOptions;
using laghu::os::UnixListenerConfig;
using laghu::os::internal::ListenerOperations;
using laghu::os::internal::ListenerTestAccess;

enum class InjectedFailure : std::uint8_t {
  none,
  set_socket_option,
  bind,
  listen,
  path_status,
};

struct FaultContext final {
  const ListenerOperations* underlying;
  InjectedFailure failure;
};

[[nodiscard]] FaultContext& fault(void* context) noexcept {
  return *static_cast<FaultContext*>(context);
}

[[nodiscard]] int forward_socket(void* context, int domain, int type,
                                 int protocol) noexcept {
  auto& state = fault(context);
  return state.underlying->socket(state.underlying->context, domain, type, protocol);
}

[[nodiscard]] int forward_fcntl(void* context, int descriptor, int command,
                                int value) noexcept {
  auto& state = fault(context);
  return state.underlying->fcntl(state.underlying->context, descriptor, command, value);
}

[[nodiscard]] int fault_setsockopt(void* context, int descriptor, int level,
                                   int option, const void* value,
                                   socklen_t size) noexcept {
  auto& state = fault(context);
  if (state.failure == InjectedFailure::set_socket_option) {
    errno = ENOPROTOOPT;
    return -1;
  }
  return state.underlying->set_socket_option(state.underlying->context, descriptor,
                                              level, option, value, size);
}

[[nodiscard]] int forward_getsockopt(void* context, int descriptor, int level,
                                     int option, void* value,
                                     socklen_t* size) noexcept {
  auto& state = fault(context);
  return state.underlying->get_socket_option(state.underlying->context, descriptor,
                                              level, option, value, size);
}

[[nodiscard]] int fault_bind(void* context, int descriptor,
                             const sockaddr* address, socklen_t size) noexcept {
  auto& state = fault(context);
  if (state.failure == InjectedFailure::bind) {
    errno = EADDRINUSE;
    return -1;
  }
  return state.underlying->bind(state.underlying->context, descriptor, address, size);
}

[[nodiscard]] int forward_connect(void* context, int descriptor,
                                  const sockaddr* address,
                                  socklen_t size) noexcept {
  auto& state = fault(context);
  return state.underlying->connect(state.underlying->context, descriptor, address,
                                    size);
}

[[nodiscard]] int fault_listen(void* context, int descriptor, int backlog) noexcept {
  auto& state = fault(context);
  if (state.failure == InjectedFailure::listen) {
    errno = EIO;
    return -1;
  }
  return state.underlying->listen(state.underlying->context, descriptor, backlog);
}

[[nodiscard]] int forward_getsockname(void* context, int descriptor,
                                      sockaddr* address, socklen_t* size) noexcept {
  auto& state = fault(context);
  return state.underlying->get_socket_name(state.underlying->context, descriptor,
                                            address, size);
}

[[nodiscard]] int fault_path_status(void* context, const char* path, bool* exists,
                                    bool* is_socket) noexcept {
  auto& state = fault(context);
  if (state.failure == InjectedFailure::path_status) {
    errno = EACCES;
    return -1;
  }
  return state.underlying->path_status(state.underlying->context, path, exists,
                                       is_socket);
}

[[nodiscard]] int forward_chmod(void* context, const char* path,
                                std::uint16_t permissions) noexcept {
  auto& state = fault(context);
  return state.underlying->set_path_mode(state.underlying->context, path,
                                         permissions);
}

[[nodiscard]] int forward_unlink(void* context, const char* path) noexcept {
  auto& state = fault(context);
  return state.underlying->unlink(state.underlying->context, path);
}

[[nodiscard]] ListenerOperations injected_operations(FaultContext& context) noexcept {
  return ListenerOperations{&context,
                            forward_socket,
                            forward_fcntl,
                            fault_setsockopt,
                            forward_getsockopt,
                            fault_bind,
                            forward_connect,
                            fault_listen,
                            forward_getsockname,
                            fault_path_status,
                            forward_chmod,
                            forward_unlink};
}

[[nodiscard]] TcpListenerOptions options(bool reuse_address = true) noexcept {
  return TcpListenerOptions{16, reuse_address, false, true, true};
}

[[nodiscard]] bool descriptor_is_configured(int descriptor) noexcept {
  const int status = ::fcntl(descriptor, F_GETFL, 0);
  const int flags = ::fcntl(descriptor, F_GETFD, 0);
  return status >= 0 && (status & O_NONBLOCK) != 0 && flags >= 0 &&
         (flags & FD_CLOEXEC) != 0;
}

[[nodiscard]] bool check_ipv4_listener() noexcept {
  auto listener = Listener::create_ipv4(
      Ipv4ListenerConfig{{127, 0, 0, 1}, 0, options()});
  if (!listener || listener->kind() != ListenerKind::ipv4_tcp ||
      !descriptor_is_configured(listener->borrow().native_handle())) {
    return false;
  }
  sockaddr_storage address{};
  socklen_t size = sizeof(address);
  return ::getsockname(listener->borrow().native_handle(),
                       reinterpret_cast<sockaddr*>(&address), &size) == 0 &&
         address.ss_family == AF_INET;
}

[[nodiscard]] bool check_ipv6_listener() noexcept {
  std::array<std::uint8_t, 16> loopback{};
  loopback.back() = 1;
  auto listener = Listener::create_ipv6(
      Ipv6ListenerConfig{loopback, 0, 0, options()});
  if (!listener || listener->kind() != ListenerKind::ipv6_tcp) {
    return false;
  }
  int ipv6_only{};
  socklen_t size = sizeof(ipv6_only);
  return ::getsockopt(listener->borrow().native_handle(), IPPROTO_IPV6,
                      IPV6_V6ONLY, &ipv6_only, &size) == 0 && ipv6_only == 1;
}

[[nodiscard]] bool build_path(const laghu::test::TemporaryDirectory& directory,
                              const char* name,
                              std::array<char, UnixListenerConfig::path_capacity>& path,
                              std::size_t& size) noexcept {
  const std::size_t directory_size = directory.path().size();
  const std::size_t name_size = std::strlen(name);
  size = directory_size + 1U + name_size;
  if (size >= path.size()) {
    return false;
  }
  std::memcpy(path.data(), directory.path().data(), directory_size);
  path[directory_size] = '/';
  std::memcpy(path.data() + directory_size + 1U, name, name_size + 1U);
  return true;
}

[[nodiscard]] bool check_unix_listener() noexcept {
  const auto directory = laghu::test::TemporaryDirectory::create("typed-listener");
  std::array<char, UnixListenerConfig::path_capacity> path{};
  std::size_t size{};
  if (!directory || !build_path(*directory, "listener.sock", path, size)) {
    return false;
  }
  auto listener = Listener::create_unix(
      UnixListenerConfig{TextView::from({path.data(), size}), 8, 0600, false});
  struct stat status {};
  if (!listener || ::lstat(path.data(), &status) != 0 ||
      !S_ISSOCK(status.st_mode) || (status.st_mode & 0777U) != 0600U) {
    return false;
  }
  const auto closed = listener->close();
  return closed && ::lstat(path.data(), &status) != 0 && errno == ENOENT;
}

[[nodiscard]] bool check_safe_stale_cleanup() noexcept {
  const auto directory = laghu::test::TemporaryDirectory::create("listener-stale");
  std::array<char, UnixListenerConfig::path_capacity> path{};
  std::size_t size{};
  if (!directory || !build_path(*directory, "occupied", path, size)) {
    return false;
  }
  const int file = ::open(path.data(), O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (file < 0) {
    return false;
  }
  static_cast<void>(::close(file));
  const auto listener = Listener::create_unix(
      UnixListenerConfig{TextView::from({path.data(), size}), 8, 0600, true});
  struct stat status {};
  const bool retained = ::lstat(path.data(), &status) == 0 && S_ISREG(status.st_mode);
  static_cast<void>(::unlink(path.data()));
  return !listener && retained;
}

[[nodiscard]] bool check_active_unix_listener_is_retained() noexcept {
  const auto directory = laghu::test::TemporaryDirectory::create("listener-active");
  std::array<char, UnixListenerConfig::path_capacity> path{};
  std::size_t size{};
  if (!directory || !build_path(*directory, "active-sock", path, size)) {
    return false;
  }
  const auto active = laghu::test::UnixSocket::bind(*directory, "active-sock");
  if (!active) {
    return false;
  }
  const auto replacement = Listener::create_unix(
      UnixListenerConfig{TextView::from({path.data(), size}), 8, 0600, true});
  struct stat status {};
  return !replacement && ::lstat(path.data(), &status) == 0 &&
         S_ISSOCK(status.st_mode);
}

[[nodiscard]] bool check_stale_unix_listener_is_replaced() noexcept {
  const auto directory = laghu::test::TemporaryDirectory::create("listener-replace");
  std::array<char, UnixListenerConfig::path_capacity> path{};
  std::size_t size{};
  if (!directory || !build_path(*directory, "stale.sock", path, size)) {
    return false;
  }
  int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (descriptor < 0) {
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.data(), size + 1U);
  const auto address_size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                                   size + 1U);
#if defined(__APPLE__) || defined(__FreeBSD__)
  address.sun_len = static_cast<decltype(address.sun_len)>(address_size);
#endif
  const bool bound = ::bind(descriptor, reinterpret_cast<const sockaddr*>(&address),
                            address_size) == 0;
  static_cast<void>(::close(descriptor));
  if (!bound) {
    return false;
  }
  auto replacement = Listener::create_unix(
      UnixListenerConfig{TextView::from({path.data(), size}), 8, 0600, true});
  return replacement && replacement->close();
}

[[nodiscard]] bool check_adoption() noexcept {
  const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
  if (descriptor < 0) {
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(descriptor, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 ||
      ::listen(descriptor, 8) != 0) {
    static_cast<void>(::close(descriptor));
    return false;
  }
  auto socket = SocketHandle::adopt(descriptor);
  if (!socket) {
    static_cast<void>(::close(descriptor));
    return false;
  }
  auto listener = Listener::adopt(std::move(*socket), ListenerKind::ipv4_tcp);
  return listener && descriptor_is_configured(listener->borrow().native_handle());
}

[[nodiscard]] bool check_faults() noexcept {
  const auto& underlying = laghu::os::internal::default_listener_operations();
  constexpr std::array failures{InjectedFailure::set_socket_option,
                                InjectedFailure::bind,
                                InjectedFailure::listen};
  for (const auto failure : failures) {
    FaultContext context{&underlying, failure};
    const ListenerOperations operations = injected_operations(context);
    const auto listener = ListenerTestAccess::create_ipv4(
        Ipv4ListenerConfig{{127, 0, 0, 1}, 0, options()}, operations);
    if (listener) {
      return false;
    }
  }
  const auto directory = laghu::test::TemporaryDirectory::create("listener-fault");
  std::array<char, UnixListenerConfig::path_capacity> path{};
  std::size_t size{};
  if (!directory || !build_path(*directory, "listener.sock", path, size)) {
    return false;
  }
  FaultContext context{&underlying, InjectedFailure::path_status};
  const ListenerOperations operations = injected_operations(context);
  return !ListenerTestAccess::create_unix(
      UnixListenerConfig{TextView::from({path.data(), size}), 8, 0600, false},
      operations);
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"os.listener.ipv4", check_ipv4_listener},
      laghu::test::TestCase{"os.listener.ipv6", check_ipv6_listener},
      laghu::test::TestCase{"os.listener.unix", check_unix_listener},
      laghu::test::TestCase{"os.listener.stale_cleanup", check_safe_stale_cleanup},
      laghu::test::TestCase{"os.listener.active_retained",
                            check_active_unix_listener_is_retained},
      laghu::test::TestCase{"os.listener.stale_replaced",
                            check_stale_unix_listener_is_replaced},
      laghu::test::TestCase{"os.listener.adoption", check_adoption},
      laghu::test::TestCase{"os.listener.faults", check_faults},
  };
  return laghu::test::run_tests(tests);
}
