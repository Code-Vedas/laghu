// SPDX-License-Identifier: AGPL-3.0-only
#include "laghu_test_support.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

[[nodiscard]] bool check_temporary_directory() noexcept {
  std::array<char, laghu::test::fixture_path_capacity> path{};
  std::size_t path_size{};
  {
    auto directory = laghu::test::TemporaryDirectory::create("temporary");
    if (!directory.has_value() || !directory->valid() ||
        directory->path().size() >= path.size()) {
      return false;
    }
    path_size = directory->path().size();
    std::memcpy(path.data(), directory->path().data(), path_size);
    path[path_size] = '\0';
    if (::access(path.data(), F_OK) != 0) {
      return false;
    }
    std::array<char, laghu::test::fixture_path_capacity> nested = path;
    for (int depth = 0; depth < 17; ++depth) {
      std::array<char, laghu::test::fixture_path_capacity> next{};
      const int rendered = std::snprintf(next.data(), next.size(), "%s/level-%d",
                                         nested.data(), depth);
      if (rendered <= 0 || static_cast<std::size_t>(rendered) >= next.size() ||
          ::mkdir(next.data(), 0700) != 0) {
        return false;
      }
      nested = next;
    }
  }
  return path_size != 0U && ::access(path.data(), F_OK) != 0 && errno == ENOENT;
}

[[nodiscard]] bool check_unix_socket() noexcept {
  const auto directory = laghu::test::TemporaryDirectory::create("unix");
  if (!directory.has_value()) {
    return false;
  }
  const auto listener = laghu::test::UnixSocket::bind(*directory, "listener");
  if (!listener.has_value() || listener->fd() < 0 || listener->path().empty()) {
    return false;
  }
  int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (client < 0) {
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (listener->path().size() >= sizeof(address.sun_path)) {
    (void)::close(client);
    return false;
  }
  std::memcpy(address.sun_path, listener->path().data(), listener->path().size() + 1U);
  const auto address_size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                                   listener->path().size() + 1U);
  const bool connected = ::connect(client, reinterpret_cast<const sockaddr*>(&address), address_size) == 0;
  const int accepted = connected ? ::accept(listener->fd(), nullptr, nullptr) : -1;
  (void)::close(client);
  if (accepted < 0) {
    return false;
  }
  (void)::close(accepted);
  return true;
}

[[nodiscard]] bool check_loopback_tcp() noexcept {
  const auto listener = laghu::test::LoopbackTcpListener::create();
  if (!listener.has_value() || listener->fd() < 0 || listener->port() == 0U) {
    return false;
  }
  int client = ::socket(AF_INET, SOCK_STREAM, 0);
  if (client < 0) {
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(listener->port());
  const bool connected = ::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
  const int accepted = connected ? ::accept(listener->fd(), nullptr, nullptr) : -1;
  (void)::close(client);
  if (accepted < 0) {
    return false;
  }
  (void)::close(accepted);
  return true;
}

[[nodiscard]] bool check_loopback_udp() noexcept {
  const auto receiver = laghu::test::LoopbackUdpSocket::bind();
  if (!receiver.has_value() || receiver->fd() < 0 || receiver->port() == 0U) {
    return false;
  }
  int sender = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (sender < 0) {
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(receiver->port());
  constexpr std::array<char, 4> message{'u', 'd', 'p', '!'};
  const bool sent = ::sendto(sender, message.data(), message.size(), 0,
                             reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
                    static_cast<ssize_t>(message.size());
  (void)::close(sender);
  pollfd poll_descriptor{receiver->fd(), POLLIN, 0};
  if (!sent || ::poll(&poll_descriptor, 1, 1000) != 1 ||
      (poll_descriptor.revents & POLLIN) == 0) {
    return false;
  }
  std::array<char, message.size()> received{};
  const ssize_t received_size = ::recvfrom(receiver->fd(), received.data(), received.size(), 0, nullptr, nullptr);
  return received_size == static_cast<ssize_t>(message.size()) && received == message;
}

struct ConcurrentResult final {
  bool created{};
  std::array<char, laghu::test::fixture_path_capacity> path{};
  std::size_t path_size{};
};

void* create_isolated_fixture(void* argument) noexcept {
  auto& result = *static_cast<ConcurrentResult*>(argument);
  const auto directory = laghu::test::TemporaryDirectory::create("concurrent");
  if (!directory.has_value() || directory->path().size() >= result.path.size()) {
    return nullptr;
  }
  result.path_size = directory->path().size();
  std::memcpy(result.path.data(), directory->path().data(), result.path_size);
  result.path[result.path_size] = '\0';
  result.created = ::access(result.path.data(), F_OK) == 0;
  return nullptr;
}

[[nodiscard]] bool check_concurrent_isolation() noexcept {
  ConcurrentResult first{};
  ConcurrentResult second{};
  pthread_t first_thread{};
  pthread_t second_thread{};
  if (::pthread_create(&first_thread, nullptr, create_isolated_fixture, &first) != 0) {
    return false;
  }
  if (::pthread_create(&second_thread, nullptr, create_isolated_fixture, &second) != 0) {
    (void)::pthread_join(first_thread, nullptr);
    return false;
  }
  if (::pthread_join(first_thread, nullptr) != 0 || ::pthread_join(second_thread, nullptr) != 0) {
    return false;
  }
  return first.created && second.created && first.path_size != 0U && second.path_size != 0U &&
         std::strcmp(first.path.data(), second.path.data()) != 0 &&
         ::access(first.path.data(), F_OK) != 0 && ::access(second.path.data(), F_OK) != 0;
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"temporary_directory.cleanup", check_temporary_directory},
      laghu::test::TestCase{"unix_socket.connection", check_unix_socket},
      laghu::test::TestCase{"loopback_tcp.connection", check_loopback_tcp},
      laghu::test::TestCase{"loopback_udp.datagram", check_loopback_udp},
      laghu::test::TestCase{"temporary_directory.concurrent_isolation", check_concurrent_isolation},
  };
  return laghu::test::run_tests(tests);
}
