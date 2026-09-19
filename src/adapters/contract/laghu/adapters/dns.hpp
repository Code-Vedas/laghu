// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include <laghu/adapters/dependency.hpp>
#include <laghu/core/views.hpp>

namespace laghu::adapters {

enum class DnsAddressFamily : std::uint8_t {
  ipv4,
  ipv6,
};

enum class DnsQueryFamily : std::uint8_t {
  ipv4,
  ipv6,
  any,
};

struct DnsAddress final {
  DnsAddressFamily family{DnsAddressFamily::ipv4};
  std::array<std::byte, 16> bytes{};
  std::uint32_t ttl_seconds{};
};

struct DnsNameserver final {
  DnsAddressFamily family{DnsAddressFamily::ipv4};
  std::array<std::byte, 16> bytes{};
  std::uint16_t port{53};
};

class DnsQueryToken final {
 public:
  constexpr DnsQueryToken() noexcept = default;
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

 private:
  friend class DnsResolver;
  explicit constexpr DnsQueryToken(std::uint64_t value) noexcept : value_(value) {}
  std::uint64_t value_{};
};

struct DnsQueryResult final {
  DnsQueryToken token;
  // Both borrowed fields are valid only for the duration of DnsQueryComplete.
  // Copy owned values inside the callback; retaining this result does not
  // extend either lifetime.
  std::span<const DnsAddress> addresses;
  const core::Error* error{};

  [[nodiscard]] constexpr bool succeeded() const noexcept { return error == nullptr; }
};

using DnsQueryComplete = void (*)(void*, const DnsQueryResult&) noexcept;

struct DnsQuerySink final {
  void* context{};
  DnsQueryComplete complete{};
};

struct DnsResolverLimits final {
  std::size_t maximum_outstanding_queries{};
  std::size_t maximum_result_addresses{};
  std::uint32_t timeout_milliseconds{};
  std::uint32_t attempts{};
};

struct DnsResolverConfig final {
  DnsResolverLimits limits;
  std::span<const DnsNameserver> nameservers;
};

struct DnsSocketInterest final {
  int descriptor{-1};
  bool readable{};
  bool writable{};
};

struct DnsSocketEvent final {
  int descriptor{-1};
  bool readable{};
  bool writable{};
};

struct DnsTimeout final {
  bool active{};
  std::uint64_t milliseconds{};
};

// One resolver belongs to one event-loop owner. Its callbacks are synchronous
// with process_events(), process_timeout(), or cancel() and may destroy or
// replace the resolver. Destruction abandons outstanding queries without
// invoking their completion callbacks.
class DnsResolver final {
 public:
  static constexpr std::size_t maximum_query_capacity = 64;
  static constexpr std::size_t maximum_address_capacity = 32;
  static constexpr std::size_t maximum_socket_capacity = maximum_query_capacity * 16;
  static constexpr std::size_t maximum_nameserver_capacity = 8;

  DnsResolver() noexcept = default;
  DnsResolver(const DnsResolver&) = delete;
  DnsResolver& operator=(const DnsResolver&) = delete;
  DnsResolver(DnsResolver&& other) noexcept;
  DnsResolver& operator=(DnsResolver&& other) noexcept;
  ~DnsResolver();

  [[nodiscard]] static core::Result<DnsResolver> create(
      DnsResolverConfig config, DnsQuerySink sink,
      DependencyLogSink log_sink = {}) noexcept;
  [[nodiscard]] core::Result<DnsQueryToken> resolve(
      core::TextView hostname, DnsQueryFamily family = DnsQueryFamily::any) noexcept;
  [[nodiscard]] core::Result<void> cancel(DnsQueryToken token) noexcept;
  [[nodiscard]] core::Result<std::size_t> socket_interests(
      std::span<DnsSocketInterest> output) const noexcept;
  [[nodiscard]] DnsTimeout next_timeout() const noexcept;
  [[nodiscard]] core::Result<void> process_events(
      std::span<const DnsSocketEvent> events) noexcept;
  [[nodiscard]] core::Result<void> process_timeout() noexcept;
  [[nodiscard]] std::size_t outstanding_queries() const noexcept;

 private:
  explicit DnsResolver(void* state) noexcept : state_(state) {}
  void release() noexcept;
  void* state_{};
};

static_assert(std::is_trivially_copyable_v<DnsAddress>);
static_assert(std::is_trivially_copyable_v<DnsNameserver>);
static_assert(std::is_trivially_copyable_v<DnsQueryToken>);
static_assert(std::is_trivially_copyable_v<DnsQueryResult>);
static_assert(std::is_trivially_copyable_v<DnsSocketInterest>);
static_assert(!std::is_copy_constructible_v<DnsResolver>);

}  // namespace laghu::adapters
