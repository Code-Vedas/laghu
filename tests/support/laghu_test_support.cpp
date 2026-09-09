// SPDX-License-Identifier: AGPL-3.0-only
#include "laghu_test_support.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include <arpa/inet.h>
#include <ftw.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace laghu::test {
namespace {

std::atomic<unsigned long> fixture_serial{0};
constexpr int cleanup_descriptor_limit = 8;

[[nodiscard]] bool is_label_character(char character) noexcept {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '_' || character == '-';
}

[[nodiscard]] bool append_text(char* output, std::size_t capacity, std::size_t& size,
                               std::string_view text) noexcept {
  if (text.size() > capacity - size - 1U) {
    return false;
  }
  std::memcpy(output + size, text.data(), text.size());
  size += text.size();
  output[size] = '\0';
  return true;
}

[[nodiscard]] bool append_number(char* output, std::size_t capacity, std::size_t& size,
                                 unsigned long value) noexcept {
  if (size >= capacity) {
    return false;
  }
  const auto conversion = std::to_chars(output + size, output + capacity - 1U, value);
  if (conversion.ec != std::errc{}) {
    return false;
  }
  size = static_cast<std::size_t>(conversion.ptr - output);
  output[size] = '\0';
  return true;
}

void close_fd(int& fd) noexcept {
  if (fd >= 0) {
    (void)::close(fd);
    fd = -1;
  }
}

int remove_tree_entry(const char* path, const struct stat*, int type,
                      struct FTW*) noexcept {
  if (type == FTW_DP || type == FTW_D) {
    return ::rmdir(path);
  }
  if (type == FTW_F || type == FTW_SL || type == FTW_SLN) {
    return ::unlink(path);
  }
  errno = EPERM;
  return -1;
}

[[nodiscard]] bool remove_tree(const char* path) noexcept {
  if (path == nullptr || path[0] == '\0') {
    return true;
  }
  // nftw limits simultaneously open directory descriptors to this explicit
  // fixed value while FTW_PHYS prevents traversal through a test-created link.
  return ::nftw(path, remove_tree_entry, cleanup_descriptor_limit,
                FTW_DEPTH | FTW_PHYS) == 0;
}

[[nodiscard]] FixtureFailure system_failure() noexcept {
  return FixtureFailure{FixtureError::system, errno};
}

[[nodiscard]] bool valid_test_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > 96U) {
    return false;
  }
  for (const char character : name) {
    if (!is_label_character(character) && character != '.') {
      return false;
    }
  }
  return true;
}

void write_all(const char* data, std::size_t size) noexcept {
  while (size > 0U) {
    const ssize_t written = ::write(STDERR_FILENO, data, size);
    if (written > 0) {
      const auto consumed = static_cast<std::size_t>(written);
      data += consumed;
      size -= consumed;
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return;
  }
}

void write_status(std::string_view status, std::string_view name) noexcept {
  std::array<char, 128> line{};
  std::size_t size{};
  if (!append_text(line.data(), line.size(), size, "laghu-test status=") ||
      !append_text(line.data(), line.size(), size, status) ||
      !append_text(line.data(), line.size(), size, " name=") ||
      !append_text(line.data(), line.size(), size, name) ||
      !append_text(line.data(), line.size(), size, "\n")) {
    constexpr std::string_view fallback{"laghu-test status=invalid reason=diagnostic_overflow\n"};
    write_all(fallback.data(), fallback.size());
    return;
  }
  write_all(line.data(), size);
}

[[nodiscard]] std::expected<unsigned short, FixtureFailure> bind_loopback(int fd,
                                                                            int type) noexcept {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    return std::unexpected{system_failure()};
  }
  if (type == SOCK_STREAM && ::listen(fd, 8) != 0) {
    return std::unexpected{system_failure()};
  }
  socklen_t address_size = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) != 0 ||
      address_size != sizeof(address)) {
    return std::unexpected{system_failure()};
  }
  return ntohs(address.sin_port);
}

}  // namespace

TemporaryDirectory::TemporaryDirectory(TemporaryDirectory&& other) noexcept
    : path_size_(other.path_size_) {
  std::memcpy(path_, other.path_, path_size_ + 1U);
  other.path_size_ = 0;
  other.path_[0] = '\0';
}

TemporaryDirectory& TemporaryDirectory::operator=(TemporaryDirectory&& other) noexcept {
  if (this != &other) {
    (void)remove_tree(path_);
    path_size_ = other.path_size_;
    std::memcpy(path_, other.path_, path_size_ + 1U);
    other.path_size_ = 0;
    other.path_[0] = '\0';
  }
  return *this;
}

TemporaryDirectory::~TemporaryDirectory() { (void)remove_tree(path_); }

std::expected<TemporaryDirectory, FixtureFailure> TemporaryDirectory::create(
    std::string_view label) noexcept {
  if (label.empty() || label.size() > 64U) {
    return std::unexpected{FixtureFailure{FixtureError::invalid_label, 0}};
  }
  for (const char character : label) {
    if (!is_label_character(character)) {
      return std::unexpected{FixtureFailure{FixtureError::invalid_label, 0}};
    }
  }

  const char* temporary_root = ::getenv("TMPDIR");
  if (temporary_root == nullptr || temporary_root[0] != '/') {
    temporary_root = "/tmp";
  }
  TemporaryDirectory directory;
  if (!append_text(directory.path_, sizeof(directory.path_), directory.path_size_, temporary_root) ||
      !append_text(directory.path_, sizeof(directory.path_), directory.path_size_, "/laghu-") ||
      !append_text(directory.path_, sizeof(directory.path_), directory.path_size_, label) ||
      !append_text(directory.path_, sizeof(directory.path_), directory.path_size_, "-") ||
      !append_number(directory.path_, sizeof(directory.path_), directory.path_size_,
                     static_cast<unsigned long>(::getpid())) ||
      !append_text(directory.path_, sizeof(directory.path_), directory.path_size_, "-") ||
      !append_number(directory.path_, sizeof(directory.path_), directory.path_size_,
                     fixture_serial.fetch_add(1U, std::memory_order_relaxed)) ||
      !append_text(directory.path_, sizeof(directory.path_), directory.path_size_, "-XXXXXX")) {
    return std::unexpected{FixtureFailure{FixtureError::path_too_long, 0}};
  }
  if (::mkdtemp(directory.path_) == nullptr) {
    return std::unexpected{system_failure()};
  }
  directory.path_size_ = std::strlen(directory.path_);
  return directory;
}

bool TemporaryDirectory::valid() const noexcept { return path_size_ != 0U; }

std::string_view TemporaryDirectory::path() const noexcept { return {path_, path_size_}; }

UnixSocket::UnixSocket(UnixSocket&& other) noexcept
    : fd_(other.fd_), path_size_(other.path_size_) {
  std::memcpy(path_, other.path_, path_size_ + 1U);
  other.fd_ = -1;
  other.path_size_ = 0;
  other.path_[0] = '\0';
}

UnixSocket& UnixSocket::operator=(UnixSocket&& other) noexcept {
  if (this != &other) {
    close_fd(fd_);
    if (path_size_ != 0U) {
      (void)::unlink(path_);
    }
    fd_ = other.fd_;
    path_size_ = other.path_size_;
    std::memcpy(path_, other.path_, path_size_ + 1U);
    other.fd_ = -1;
    other.path_size_ = 0;
    other.path_[0] = '\0';
  }
  return *this;
}

UnixSocket::~UnixSocket() {
  close_fd(fd_);
  if (path_size_ != 0U) {
    (void)::unlink(path_);
  }
}

std::expected<UnixSocket, FixtureFailure> UnixSocket::bind(
    const TemporaryDirectory& directory, std::string_view name) noexcept {
  if (!directory.valid() || name.empty() || name.size() > 64U) {
    return std::unexpected{FixtureFailure{FixtureError::invalid_label, 0}};
  }
  for (const char character : name) {
    if (!is_label_character(character)) {
      return std::unexpected{FixtureFailure{FixtureError::invalid_label, 0}};
    }
  }
  UnixSocket socket;
  if (!append_text(socket.path_, sizeof(socket.path_), socket.path_size_, directory.path()) ||
      !append_text(socket.path_, sizeof(socket.path_), socket.path_size_, "/") ||
      !append_text(socket.path_, sizeof(socket.path_), socket.path_size_, name)) {
    return std::unexpected{FixtureFailure{FixtureError::path_too_long, 0}};
  }
  if (socket.path_size_ >= sizeof(sockaddr_un{}.sun_path)) {
    return std::unexpected{FixtureFailure{FixtureError::path_too_long, 0}};
  }
  socket.fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket.fd_ < 0) {
    return std::unexpected{system_failure()};
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket.path_, socket.path_size_ + 1U);
  const auto address_size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socket.path_size_ + 1U);
  if (::bind(socket.fd_, reinterpret_cast<const sockaddr*>(&address), address_size) != 0 ||
      ::listen(socket.fd_, 8) != 0) {
    return std::unexpected{system_failure()};
  }
  return socket;
}

int UnixSocket::fd() const noexcept { return fd_; }

std::string_view UnixSocket::path() const noexcept { return {path_, path_size_}; }

LoopbackTcpListener::LoopbackTcpListener(LoopbackTcpListener&& other) noexcept
    : fd_(other.fd_), port_(other.port_) {
  other.fd_ = -1;
  other.port_ = 0;
}

LoopbackTcpListener& LoopbackTcpListener::operator=(LoopbackTcpListener&& other) noexcept {
  if (this != &other) {
    close_fd(fd_);
    fd_ = other.fd_;
    port_ = other.port_;
    other.fd_ = -1;
    other.port_ = 0;
  }
  return *this;
}

LoopbackTcpListener::~LoopbackTcpListener() { close_fd(fd_); }

std::expected<LoopbackTcpListener, FixtureFailure> LoopbackTcpListener::create() noexcept {
  LoopbackTcpListener listener;
  listener.fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener.fd_ < 0) {
    return std::unexpected{system_failure()};
  }
  int enabled = 1;
  if (::setsockopt(listener.fd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
    return std::unexpected{system_failure()};
  }
  const auto port = bind_loopback(listener.fd_, SOCK_STREAM);
  if (!port.has_value()) {
    return std::unexpected{port.error()};
  }
  listener.port_ = *port;
  return listener;
}

int LoopbackTcpListener::fd() const noexcept { return fd_; }

unsigned short LoopbackTcpListener::port() const noexcept { return port_; }

LoopbackUdpSocket::LoopbackUdpSocket(LoopbackUdpSocket&& other) noexcept
    : fd_(other.fd_), port_(other.port_) {
  other.fd_ = -1;
  other.port_ = 0;
}

LoopbackUdpSocket& LoopbackUdpSocket::operator=(LoopbackUdpSocket&& other) noexcept {
  if (this != &other) {
    close_fd(fd_);
    fd_ = other.fd_;
    port_ = other.port_;
    other.fd_ = -1;
    other.port_ = 0;
  }
  return *this;
}

LoopbackUdpSocket::~LoopbackUdpSocket() { close_fd(fd_); }

std::expected<LoopbackUdpSocket, FixtureFailure> LoopbackUdpSocket::bind() noexcept {
  LoopbackUdpSocket socket;
  socket.fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket.fd_ < 0) {
    return std::unexpected{system_failure()};
  }
  const auto port = bind_loopback(socket.fd_, SOCK_DGRAM);
  if (!port.has_value()) {
    return std::unexpected{port.error()};
  }
  socket.port_ = *port;
  return socket;
}

int LoopbackUdpSocket::fd() const noexcept { return fd_; }

unsigned short LoopbackUdpSocket::port() const noexcept { return port_; }

int run_tests(std::span<const TestCase> tests, TestRunOptions options) noexcept {
  for (std::size_t index = 0; index < tests.size(); ++index) {
    if (tests[index].function == nullptr || !valid_test_name(tests[index].name)) {
      write_status("invalid", "test_case");
      return static_cast<int>(TestExitCode::invalid_suite);
    }
    for (std::size_t prior = 0; prior < index; ++prior) {
      if (tests[prior].name == tests[index].name) {
        write_status("invalid", "duplicate_name");
        return static_cast<int>(TestExitCode::invalid_suite);
      }
    }
  }
  for (const TestCase& test : tests) {
    if (!test.function()) {
      write_status("fail", test.name);
      return static_cast<int>(TestExitCode::test_failed);
    }
    if (options.verbose) {
      write_status("pass", test.name);
    }
  }
  return static_cast<int>(TestExitCode::success);
}

}  // namespace laghu::test
