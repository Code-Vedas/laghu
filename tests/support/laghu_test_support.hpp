// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace laghu::test {

inline constexpr std::size_t fixture_path_capacity = 4096;

enum class FixtureError : unsigned char {
  invalid_label,
  path_too_long,
  system,
};

struct FixtureFailure final {
  FixtureError error;
  int native_code;
};

class TemporaryDirectory final {
 public:
  TemporaryDirectory() noexcept = default;
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  TemporaryDirectory(TemporaryDirectory&& other) noexcept;
  TemporaryDirectory& operator=(TemporaryDirectory&& other) noexcept;
  ~TemporaryDirectory();

  [[nodiscard]] static std::expected<TemporaryDirectory, FixtureFailure> create(
      std::string_view label) noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::string_view path() const noexcept;

 private:
  friend class UnixSocket;

  char path_[fixture_path_capacity]{};
  std::size_t path_size_{};
};

class UnixSocket final {
 public:
  UnixSocket() noexcept = default;
  UnixSocket(const UnixSocket&) = delete;
  UnixSocket& operator=(const UnixSocket&) = delete;
  UnixSocket(UnixSocket&& other) noexcept;
  UnixSocket& operator=(UnixSocket&& other) noexcept;
  ~UnixSocket();

  [[nodiscard]] static std::expected<UnixSocket, FixtureFailure> bind(
      const TemporaryDirectory& directory, std::string_view name) noexcept;
  [[nodiscard]] int fd() const noexcept;
  [[nodiscard]] std::string_view path() const noexcept;

 private:
  int fd_{-1};
  char path_[fixture_path_capacity]{};
  std::size_t path_size_{};
};

class LoopbackTcpListener final {
 public:
  LoopbackTcpListener() noexcept = default;
  LoopbackTcpListener(const LoopbackTcpListener&) = delete;
  LoopbackTcpListener& operator=(const LoopbackTcpListener&) = delete;
  LoopbackTcpListener(LoopbackTcpListener&& other) noexcept;
  LoopbackTcpListener& operator=(LoopbackTcpListener&& other) noexcept;
  ~LoopbackTcpListener();

  [[nodiscard]] static std::expected<LoopbackTcpListener, FixtureFailure> create() noexcept;
  [[nodiscard]] int fd() const noexcept;
  [[nodiscard]] unsigned short port() const noexcept;

 private:
  int fd_{-1};
  unsigned short port_{};
};

class LoopbackUdpSocket final {
 public:
  LoopbackUdpSocket() noexcept = default;
  LoopbackUdpSocket(const LoopbackUdpSocket&) = delete;
  LoopbackUdpSocket& operator=(const LoopbackUdpSocket&) = delete;
  LoopbackUdpSocket(LoopbackUdpSocket&& other) noexcept;
  LoopbackUdpSocket& operator=(LoopbackUdpSocket&& other) noexcept;
  ~LoopbackUdpSocket();

  [[nodiscard]] static std::expected<LoopbackUdpSocket, FixtureFailure> bind() noexcept;
  [[nodiscard]] int fd() const noexcept;
  [[nodiscard]] unsigned short port() const noexcept;

 private:
  int fd_{-1};
  unsigned short port_{};
};

using TestFunction = bool (*)() noexcept;

struct TestCase final {
  std::string_view name;
  TestFunction function;
};

struct TestRunOptions final {
  bool verbose{};
};

enum class TestExitCode : int {
  success = 0,
  test_failed = 1,
  invalid_suite = 2,
};

[[nodiscard]] int run_tests(std::span<const TestCase> tests,
                            TestRunOptions options = {}) noexcept;

}  // namespace laghu::test
