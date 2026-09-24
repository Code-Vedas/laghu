// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/os/listener.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <laghu/os/internal/listener.hpp>

namespace laghu::os::internal {
namespace {

[[nodiscard]] int system_socket(void*, int domain, int type, int protocol) noexcept {
  return ::socket(domain, type, protocol);
}

[[nodiscard]] int system_fcntl(void*, int descriptor, int command, int value) noexcept {
  return ::fcntl(descriptor, command, value);
}

[[nodiscard]] int system_setsockopt(void*, int descriptor, int level, int option,
                                    const void* value, socklen_t size) noexcept {
  return ::setsockopt(descriptor, level, option, value, size);
}

[[nodiscard]] int system_getsockopt(void*, int descriptor, int level, int option,
                                    void* value, socklen_t* size) noexcept {
  return ::getsockopt(descriptor, level, option, value, size);
}

[[nodiscard]] int system_bind(void*, int descriptor, const sockaddr* address,
                              socklen_t size) noexcept {
  return ::bind(descriptor, address, size);
}

[[nodiscard]] int system_connect(void*, int descriptor, const sockaddr* address,
                                 socklen_t size) noexcept {
  return ::connect(descriptor, address, size);
}

[[nodiscard]] int system_listen(void*, int descriptor, int backlog) noexcept {
  return ::listen(descriptor, backlog);
}

[[nodiscard]] int system_getsockname(void*, int descriptor, sockaddr* address,
                                     socklen_t* size) noexcept {
  return ::getsockname(descriptor, address, size);
}

[[nodiscard]] int system_path_status(void*, const char* path, bool* exists,
                                     bool* is_socket, std::uint64_t* device,
                                     std::uint64_t* inode) noexcept {
  struct stat status {};
  if (::lstat(path, &status) == 0) {
    *exists = true;
    *is_socket = S_ISSOCK(status.st_mode);
    *device = static_cast<std::uint64_t>(status.st_dev);
    *inode = static_cast<std::uint64_t>(status.st_ino);
    return 0;
  }
  if (errno == ENOENT) {
    *exists = false;
    *is_socket = false;
    *device = 0;
    *inode = 0;
    return 0;
  }
  return -1;
}

[[nodiscard]] int system_chmod(void*, const char* path,
                               std::uint16_t permissions) noexcept {
  return ::chmod(path, static_cast<mode_t>(permissions));
}

[[nodiscard]] int system_unlink(void*, const char* path) noexcept {
  return ::unlink(path);
}

constexpr ListenerOperations default_operations{
    nullptr,          system_socket,      system_fcntl,
    system_setsockopt, system_getsockopt, system_bind,
    system_connect,   system_listen,      system_getsockname,
    system_path_status,
    system_chmod,     system_unlink};

}  // namespace

const ListenerOperations& default_listener_operations() noexcept {
  return default_operations;
}

}  // namespace laghu::os::internal

namespace laghu::os {
namespace {

[[nodiscard]] core::Error operation_error(int native_error,
                                           const char* diagnostic) noexcept {
  return core::Error::from_errno(native_error, diagnostic);
}

class UnixDirectoryLock final {
 public:
  UnixDirectoryLock(const UnixDirectoryLock&) = delete;
  UnixDirectoryLock& operator=(const UnixDirectoryLock&) = delete;
  UnixDirectoryLock(UnixDirectoryLock&&) noexcept = default;
  UnixDirectoryLock& operator=(UnixDirectoryLock&&) noexcept = default;

  [[nodiscard]] static core::Result<UnixDirectoryLock> acquire(
      core::TextView socket_path) noexcept {
    const std::string_view path = socket_path.string_view();
    const std::size_t separator = path.find_last_of('/');
    if (separator == std::string_view::npos || separator + 1U >= path.size()) {
      return std::unexpected{core::Error{core::ErrorDomain::core,
                                         core::ErrorCode::invalid_input, 0,
                                         "Unix listener path has no filename"}};
    }
    const std::string_view parent_text = separator == 0 ? std::string_view{"/"}
                                                        : path.substr(0, separator);
    const auto parent_view = core::TextView::from(parent_text);
    const auto parent =
        parent_view.to_c_string<UnixListenerConfig::path_capacity>();
    if (!parent) {
      return std::unexpected{parent.error()};
    }
    const int directory_descriptor =
        ::open(parent->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_descriptor < 0) {
      return std::unexpected{operation_error(
          errno, "Unix listener parent directory open failed")};
    }
    auto directory = core::FileHandle::adopt(directory_descriptor);
    if (!directory) {
      static_cast<void>(::close(directory_descriptor));
      return std::unexpected{directory.error()};
    }
    struct stat directory_status {};
    if (::fstat(directory_descriptor, &directory_status) != 0) {
      return std::unexpected{operation_error(
          errno, "Unix listener parent directory inspection failed")};
    }
    if (!S_ISDIR(directory_status.st_mode) ||
        directory_status.st_uid != ::geteuid() ||
        (directory_status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      return std::unexpected{core::Error{
          core::ErrorDomain::core, core::ErrorCode::invalid_input, 0,
          "Unix listener parent must be caller-owned and not externally writable"}};
    }
    constexpr char lock_name[] = ".laghu-listener.lock";
    const int lock_descriptor = ::openat(
        directory_descriptor, lock_name,
        O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_descriptor < 0) {
      return std::unexpected{operation_error(errno,
                                              "Unix listener lock open failed")};
    }
    auto lock = core::FileHandle::adopt(lock_descriptor);
    if (!lock) {
      static_cast<void>(::close(lock_descriptor));
      return std::unexpected{lock.error()};
    }
    struct stat lock_status {};
    if (::fstat(lock_descriptor, &lock_status) != 0 ||
        !S_ISREG(lock_status.st_mode) || lock_status.st_uid != ::geteuid()) {
      return std::unexpected{operation_error(
          errno == 0 ? EACCES : errno, "Unix listener lock is not trusted")};
    }
    int lock_result{};
    do {
      lock_result = ::flock(lock_descriptor, LOCK_EX);
    } while (lock_result != 0 && errno == EINTR);
    if (lock_result != 0) {
      return std::unexpected{operation_error(errno,
                                              "Unix listener lock acquisition failed")};
    }
    return UnixDirectoryLock{std::move(*directory), std::move(*lock)};
  }

 private:
  UnixDirectoryLock(core::FileHandle&& directory, core::FileHandle&& lock) noexcept
      : directory_(std::move(directory)), lock_(std::move(lock)) {}

  core::FileHandle directory_{};
  core::FileHandle lock_{};
};

[[nodiscard]] core::Result<void> validate_operations(
    const internal::ListenerOperations& operations) noexcept {
  if (operations.socket == nullptr || operations.fcntl == nullptr ||
      operations.set_socket_option == nullptr ||
      operations.get_socket_option == nullptr || operations.bind == nullptr ||
      operations.connect == nullptr || operations.listen == nullptr ||
      operations.get_socket_name == nullptr ||
      operations.path_status == nullptr || operations.set_path_mode == nullptr ||
      operations.unlink == nullptr) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::invalid_input, 0,
                                       "listener operations are incomplete"}};
  }
  return {};
}

[[nodiscard]] core::Result<int> checked_backlog(std::uint32_t backlog) noexcept {
  if (backlog == 0 || backlog > static_cast<std::uint32_t>(INT_MAX)) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::invalid_range, 0,
                                       "listener backlog is out of range"}};
  }
  return static_cast<int>(backlog);
}

[[nodiscard]] core::Result<void> configure_descriptor(
    int descriptor, const internal::ListenerOperations& operations) noexcept {
  const int status = operations.fcntl(operations.context, descriptor, F_GETFL, 0);
  if (status < 0 || operations.fcntl(operations.context, descriptor, F_SETFL,
                                    status | O_NONBLOCK) != 0) {
    return std::unexpected{operation_error(errno, "listener nonblocking setup failed")};
  }
  const int flags = operations.fcntl(operations.context, descriptor, F_GETFD, 0);
  if (flags < 0 || operations.fcntl(operations.context, descriptor, F_SETFD,
                                   flags | FD_CLOEXEC) != 0) {
    return std::unexpected{operation_error(errno, "listener close-on-exec setup failed")};
  }
  return {};
}

[[nodiscard]] core::Result<void> set_option(
    int descriptor, int level, int option, bool enabled,
    const internal::ListenerOperations& operations, const char* diagnostic) noexcept {
  const int value = enabled ? 1 : 0;
  if (operations.set_socket_option(operations.context, descriptor, level, option,
                                   &value, sizeof(value)) != 0) {
    return std::unexpected{operation_error(errno, diagnostic)};
  }
  return {};
}

[[nodiscard]] core::Result<void> configure_tcp(
    int descriptor, bool ipv6, const TcpListenerOptions& options,
    const internal::ListenerOperations& operations) noexcept {
  if (ipv6) {
    if (const auto result = set_option(descriptor, IPPROTO_IPV6, IPV6_V6ONLY, true,
                                       operations, "IPv6-only listener setup failed");
        !result) {
      return result;
    }
  }
  if (options.reuse_address) {
    if (const auto result = set_option(descriptor, SOL_SOCKET, SO_REUSEADDR, true,
                                       operations, "listener reuse-address setup failed");
        !result) {
      return result;
    }
  }
  if (options.reuse_port) {
#if defined(SO_REUSEPORT)
    if (const auto result = set_option(descriptor, SOL_SOCKET, SO_REUSEPORT, true,
                                       operations, "listener reuse-port setup failed");
        !result) {
      return result;
    }
#else
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::unavailable_capability, 0,
                                       "listener reuse-port is unavailable"}};
#endif
  }
  if (options.keep_alive) {
    if (const auto result = set_option(descriptor, SOL_SOCKET, SO_KEEPALIVE, true,
                                       operations, "listener keepalive setup failed");
        !result) {
      return result;
    }
  }
  if (options.tcp_no_delay) {
    if (const auto result = set_option(descriptor, IPPROTO_TCP, TCP_NODELAY, true,
                                       operations, "listener TCP no-delay setup failed");
        !result) {
      return result;
    }
  }
  return {};
}

template <class Address>
[[nodiscard]] core::Result<core::SocketHandle> create_internet_socket(
    const Address& address, socklen_t address_size, bool ipv6,
    const TcpListenerOptions& options,
    const internal::ListenerOperations& operations) noexcept {
  const auto backlog = checked_backlog(options.backlog);
  if (!backlog) {
    return std::unexpected{backlog.error()};
  }
  if (const auto valid = validate_operations(operations); !valid) {
    return std::unexpected{valid.error()};
  }
  const int descriptor = operations.socket(
      operations.context, ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
  if (descriptor < 0) {
    return std::unexpected{operation_error(errno, "listener socket creation failed")};
  }
  auto socket = core::SocketHandle::adopt(descriptor);
  if (!socket) {
    static_cast<void>(::close(descriptor));
    return std::unexpected{socket.error()};
  }
  if (const auto configured = configure_descriptor(descriptor, operations); !configured) {
    return std::unexpected{configured.error()};
  }
  if (const auto configured = configure_tcp(
          descriptor, ipv6, options, operations);
      !configured) {
    return std::unexpected{configured.error()};
  }
  if (operations.bind(operations.context, descriptor,
                      reinterpret_cast<const sockaddr*>(&address), address_size) != 0) {
    return std::unexpected{operation_error(errno, "listener bind failed")};
  }
  if (operations.listen(operations.context, descriptor, *backlog) != 0) {
    return std::unexpected{operation_error(errno, "listener listen failed")};
  }
  return socket;
}

}  // namespace

Listener::Listener(core::SocketHandle&& socket, ListenerKind kind) noexcept
    : socket_(std::move(socket)), kind_(kind),
      operations_(&internal::default_listener_operations()) {}

Listener::Listener(Listener&& other) noexcept { move_from(std::move(other)); }

Listener& Listener::operator=(Listener&& other) noexcept {
  if (this != &other) {
    static_cast<void>(close());
    move_from(std::move(other));
  }
  return *this;
}

Listener::~Listener() { static_cast<void>(close()); }

core::Result<Listener> Listener::create_ipv4(
    const Ipv4ListenerConfig& config) noexcept {
  return create_ipv4(config, internal::default_listener_operations());
}

core::Result<Listener> Listener::create_ipv4(
    const Ipv4ListenerConfig& config,
    const internal::ListenerOperations& operations) noexcept {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(config.port);
  address.sin_addr = std::bit_cast<in_addr>(config.address);
  auto socket = create_internet_socket(address, sizeof(address), false,
                                       config.options, operations);
  if (!socket) {
    return std::unexpected{socket.error()};
  }
  Listener listener{std::move(*socket), ListenerKind::ipv4_tcp};
  listener.operations_ = &operations;
  return listener;
}

core::Result<Listener> Listener::create_ipv6(
    const Ipv6ListenerConfig& config) noexcept {
  return create_ipv6(config, internal::default_listener_operations());
}

core::Result<Listener> Listener::create_ipv6(
    const Ipv6ListenerConfig& config,
    const internal::ListenerOperations& operations) noexcept {
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_port = htons(config.port);
  address.sin6_scope_id = config.scope_id;
  address.sin6_addr = std::bit_cast<in6_addr>(config.address);
  auto socket = create_internet_socket(address, sizeof(address), true,
                                       config.options, operations);
  if (!socket) {
    return std::unexpected{socket.error()};
  }
  Listener listener{std::move(*socket), ListenerKind::ipv6_tcp};
  listener.operations_ = &operations;
  return listener;
}

core::Result<Listener> Listener::create_unix(
    const UnixListenerConfig& config) noexcept {
  return create_unix(config, internal::default_listener_operations());
}

core::Result<Listener> Listener::create_unix(
    const UnixListenerConfig& config,
    const internal::ListenerOperations& operations) noexcept {
  const auto backlog = checked_backlog(config.backlog);
  if (!backlog) {
    return std::unexpected{backlog.error()};
  }
  if (config.permissions > 0777U) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::invalid_range, 0,
                                       "Unix listener permissions are out of range"}};
  }
  const auto path = config.path.to_c_string<UnixListenerConfig::path_capacity>();
  if (!path || path->empty() || path->view().front() != '/') {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::invalid_input, 0,
                                       "Unix listener path must be a bounded absolute path"}};
  }
  if (const auto valid = validate_operations(operations); !valid) {
    return std::unexpected{valid.error()};
  }
  auto directory_lock = UnixDirectoryLock::acquire(config.path);
  if (!directory_lock) {
    return std::unexpected{directory_lock.error()};
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::ranges::copy(path->view(), address.sun_path);
  const auto address_size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                                   path->size() + 1U);
#if defined(__APPLE__) || defined(__FreeBSD__)
  address.sun_len = static_cast<decltype(address.sun_len)>(address_size);
#endif
  bool exists{};
  bool is_socket{};
  std::uint64_t existing_device{};
  std::uint64_t existing_inode{};
  if (operations.path_status(operations.context, path->c_str(), &exists,
                             &is_socket, &existing_device, &existing_inode) != 0) {
    return std::unexpected{operation_error(errno, "Unix listener path inspection failed")};
  }
  if (exists && (!is_socket || !config.remove_stale_socket)) {
    return std::unexpected{operation_error(is_socket ? EADDRINUSE : EEXIST,
                                           "Unix listener path is not safely replaceable")};
  }
  if (exists) {
    const int probe = operations.socket(operations.context, AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0) {
      return std::unexpected{operation_error(errno,
                                             "Unix listener stale-path probe failed")};
    }
    auto probe_socket = core::SocketHandle::adopt(probe);
    if (!probe_socket) {
      static_cast<void>(::close(probe));
      return std::unexpected{probe_socket.error()};
    }
    if (operations.connect(operations.context, probe,
                           reinterpret_cast<const sockaddr*>(&address),
                           address_size) == 0) {
      return std::unexpected{operation_error(EADDRINUSE,
                                             "Unix listener path is active")};
    }
    const int probe_error = errno;
    if (probe_error != ECONNREFUSED && probe_error != ENOENT) {
      return std::unexpected{operation_error(
          probe_error, "Unix listener path could not be proven stale")};
    }
    bool still_exists{};
    bool still_socket{};
    std::uint64_t current_device{};
    std::uint64_t current_inode{};
    if (operations.path_status(operations.context, path->c_str(), &still_exists,
                               &still_socket, &current_device,
                               &current_inode) != 0) {
      return std::unexpected{operation_error(
          errno, "Unix listener stale-path revalidation failed")};
    }
    if (still_exists &&
        (!still_socket || current_device != existing_device ||
         current_inode != existing_inode)) {
      return std::unexpected{operation_error(
          EBUSY, "Unix listener path changed during stale cleanup")};
    }
    if (still_exists &&
        operations.unlink(operations.context, path->c_str()) != 0 &&
        errno != ENOENT) {
      return std::unexpected{operation_error(errno,
                                             "stale Unix listener removal failed")};
    }
  }
  const int descriptor = operations.socket(operations.context, AF_UNIX, SOCK_STREAM, 0);
  if (descriptor < 0) {
    return std::unexpected{operation_error(errno, "Unix listener socket creation failed")};
  }
  auto socket = core::SocketHandle::adopt(descriptor);
  if (!socket) {
    static_cast<void>(::close(descriptor));
    return std::unexpected{socket.error()};
  }
  if (const auto configured = configure_descriptor(descriptor, operations); !configured) {
    return std::unexpected{configured.error()};
  }
  if (operations.bind(operations.context, descriptor,
                      reinterpret_cast<const sockaddr*>(&address), address_size) != 0) {
    return std::unexpected{operation_error(errno, "Unix listener bind failed")};
  }
  bool bound_exists{};
  bool bound_is_socket{};
  std::uint64_t bound_device{};
  std::uint64_t bound_inode{};
  if (operations.path_status(operations.context, path->c_str(), &bound_exists,
                             &bound_is_socket, &bound_device,
                             &bound_inode) != 0 ||
      !bound_exists || !bound_is_socket) {
    const int native_error = errno == 0 ? EIO : errno;
    static_cast<void>(operations.unlink(operations.context, path->c_str()));
    return std::unexpected{operation_error(
        native_error, "Unix listener bound path verification failed")};
  }
  if (operations.set_path_mode(operations.context, path->c_str(),
                               config.permissions) != 0) {
    const int native_error = errno;
    static_cast<void>(operations.unlink(operations.context, path->c_str()));
    return std::unexpected{operation_error(native_error,
                                           "Unix listener finalization failed")};
  }
  bool verified_exists{};
  bool verified_is_socket{};
  std::uint64_t verified_device{};
  std::uint64_t verified_inode{};
  if (operations.path_status(operations.context, path->c_str(), &verified_exists,
                             &verified_is_socket, &verified_device,
                             &verified_inode) != 0 ||
      !verified_exists || !verified_is_socket ||
      verified_device != bound_device || verified_inode != bound_inode) {
    const int native_error = errno == 0 ? EBUSY : errno;
    static_cast<void>(operations.unlink(operations.context, path->c_str()));
    return std::unexpected{operation_error(
        native_error, "Unix listener path changed during permission setup")};
  }
  if (operations.listen(operations.context, descriptor, *backlog) != 0) {
    const int native_error = errno;
    static_cast<void>(operations.unlink(operations.context, path->c_str()));
    return std::unexpected{operation_error(native_error,
                                           "Unix listener listen failed")};
  }
  Listener listener{std::move(*socket), ListenerKind::unix_stream};
  std::ranges::copy(path->view(), listener.unix_path_.begin());
  listener.unix_path_size_ = path->size();
  listener.unix_device_ = bound_device;
  listener.unix_inode_ = bound_inode;
  listener.owns_unix_path_ = true;
  listener.operations_ = &operations;
  return listener;
}

core::Result<Listener> Listener::adopt_trusted(
    core::SocketHandle&& socket, ListenerKind expected_kind) noexcept {
  return adopt_trusted(std::move(socket), expected_kind,
                       internal::default_listener_operations());
}

core::Result<Listener> Listener::adopt_trusted(
    core::SocketHandle&& socket, ListenerKind expected_kind,
    const internal::ListenerOperations& operations) noexcept {
  if (!socket.is_valid()) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::invalid_input, 0,
                                       "listener adoption requires a socket"}};
  }
  if (const auto valid = validate_operations(operations); !valid) {
    return std::unexpected{valid.error()};
  }
  int socket_type{};
  socklen_t option_size = sizeof(socket_type);
  if (operations.get_socket_option(operations.context, socket.native_handle(),
                                   SOL_SOCKET, SO_TYPE, &socket_type,
                                   &option_size) != 0) {
    return std::unexpected{operation_error(errno,
                                           "adopted listener type query failed")};
  }
  if (socket_type != SOCK_STREAM) {
    return std::unexpected{operation_error(EPROTOTYPE,
                                           "adopted listener is not a stream socket")};
  }
#if defined(SO_ACCEPTCONN) && (defined(__linux__) || defined(__FreeBSD__))
  int accepting{};
  option_size = sizeof(accepting);
  if (operations.get_socket_option(operations.context, socket.native_handle(),
                                   SOL_SOCKET, SO_ACCEPTCONN, &accepting,
                                   &option_size) != 0) {
    return std::unexpected{operation_error(errno,
                                           "adopted listener state query failed")};
  }
  if (accepting == 0) {
    return std::unexpected{operation_error(EINVAL,
                                           "adopted socket is not listening")};
  }
#endif
  sockaddr_storage address{};
  socklen_t address_size = sizeof(address);
  if (operations.get_socket_name(operations.context, socket.native_handle(),
                                 reinterpret_cast<sockaddr*>(&address),
                                 &address_size) != 0) {
    return std::unexpected{operation_error(errno, "adopted listener query failed")};
  }
  const int expected_family = expected_kind == ListenerKind::ipv4_tcp
                                  ? AF_INET
                                  : expected_kind == ListenerKind::ipv6_tcp ? AF_INET6 : AF_UNIX;
  if (address.ss_family != expected_family) {
    return std::unexpected{core::Error{core::ErrorDomain::core,
                                       core::ErrorCode::invalid_input, 0,
                                       "adopted listener family does not match its type"}};
  }
  if (const auto configured = configure_descriptor(socket.native_handle(), operations);
      !configured) {
    return std::unexpected{configured.error()};
  }
  Listener listener{std::move(socket), expected_kind};
  listener.operations_ = &operations;
  return listener;
}

core::Result<void> Listener::close() noexcept {
  const auto socket_closed = socket_.close();
  core::Result<void> path_removed{};
  if (owns_unix_path_) {
    const auto path_view = core::TextView::from(unix_path_.data(), unix_path_size_);
    auto directory_lock = path_view
                              ? UnixDirectoryLock::acquire(*path_view)
                              : core::Result<UnixDirectoryLock>{
                                    std::unexpected{path_view.error()}};
    if (!directory_lock || operations_ == nullptr) {
      path_removed = std::unexpected{
          !directory_lock
              ? directory_lock.error()
              : core::Error{core::ErrorDomain::core,
                            core::ErrorCode::invalid_state, 0,
                            "Unix listener has no cleanup operations"}};
    } else {
      bool exists{};
      bool is_socket{};
      std::uint64_t device{};
      std::uint64_t inode{};
      if (operations_->path_status(operations_->context, unix_path_.data(),
                                   &exists, &is_socket, &device, &inode) != 0) {
        path_removed = std::unexpected{operation_error(
            errno, "Unix listener cleanup inspection failed")};
      } else if (!exists) {
        owns_unix_path_ = false;
      } else if (!is_socket || device != unix_device_ || inode != unix_inode_) {
        path_removed = std::unexpected{operation_error(
            EBUSY, "Unix listener path ownership changed before cleanup")};
      } else if (operations_->unlink(operations_->context,
                                     unix_path_.data()) == 0 ||
                 errno == ENOENT) {
        owns_unix_path_ = false;
      } else {
        path_removed = std::unexpected{operation_error(
            errno, "Unix listener cleanup failed")};
      }
    }
  }
  if (!socket_closed) {
    return std::unexpected{socket_closed.error()};
  }
  return path_removed;
}

void Listener::move_from(Listener&& other) noexcept {
  socket_ = std::move(other.socket_);
  kind_ = other.kind_;
  unix_path_ = other.unix_path_;
  unix_path_size_ = std::exchange(other.unix_path_size_, 0);
  unix_device_ = std::exchange(other.unix_device_, 0);
  unix_inode_ = std::exchange(other.unix_inode_, 0);
  owns_unix_path_ = std::exchange(other.owns_unix_path_, false);
  operations_ = std::exchange(other.operations_, nullptr);
}

}  // namespace laghu::os
