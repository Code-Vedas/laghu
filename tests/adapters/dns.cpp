// SPDX-License-Identifier: AGPL-3.0-only
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ares.h>

#include "laghu_test_support.hpp"

#include <laghu/adapters/dns.hpp>

namespace {

using laghu::adapters::DnsAddress;
using laghu::adapters::DnsAddressFamily;
using laghu::adapters::DnsDependencyLifecycle;
using laghu::adapters::DnsNameserver;
using laghu::adapters::DnsQueryFamily;
using laghu::adapters::DnsQueryResult;
using laghu::adapters::DnsResolver;
using laghu::adapters::DnsResolverConfig;
using laghu::adapters::DnsSocketEvent;
using laghu::adapters::DnsSocketInterest;
using laghu::adapters::DnsTimeout;
using laghu::core::ErrorCode;
using laghu::core::TextView;

enum class ReplyKind : std::uint8_t {
  success,
  not_found,
  malformed,
  truncated_then_tcp,
};

DnsDependencyLifecycle test_lifecycle;

struct Completion final {
  std::array<DnsAddress, DnsResolver::maximum_address_capacity> addresses{};
  std::size_t count{};
  ErrorCode error{ErrorCode::invalid_state};
  std::int32_t native_code{};
  laghu::core::DependencyId dependency{laghu::core::DependencyId::none};
  laghu::core::DependencyOperation operation{laghu::core::DependencyOperation::none};
  std::size_t calls{};
  DnsResolver* cancel_on_completion{};
  DnsResolver* submit_on_completion{};
  DnsResolver** destroy_on_completion{};
  bool succeeded{};
  bool completed_token_was_inactive{};
  bool replacement_submitted{};
};

void completed(void* context, const DnsQueryResult& result) noexcept {
  auto& completion = *static_cast<Completion*>(context);
  ++completion.calls;
  completion.succeeded = result.succeeded();
  if (result.error != nullptr) {
    completion.error = result.error->code();
    completion.native_code = result.error->native_code();
    completion.dependency = result.error->dependency_id();
    completion.operation = result.error->dependency_operation();
  }
  completion.count = result.addresses.size();
  for (std::size_t index = 0; index < completion.count; ++index) {
    completion.addresses[index] = result.addresses[index];
  }
  if (completion.cancel_on_completion != nullptr) {
    completion.completed_token_was_inactive =
        !completion.cancel_on_completion->cancel(result.token).has_value();
  }
  if (completion.submit_on_completion != nullptr) {
    completion.replacement_submitted = completion.submit_on_completion->resolve(
        TextView::from("replacement.test"), DnsQueryFamily::ipv4).has_value();
  }
  if (completion.destroy_on_completion != nullptr) {
    delete *completion.destroy_on_completion;
    *completion.destroy_on_completion = nullptr;
  }
}

[[nodiscard]] bool write_all(int descriptor, const std::byte* data,
                             std::size_t size) noexcept {
  while (size != 0U) {
    const ssize_t written = ::write(descriptor, data, size);
    if (written > 0) {
      data += written;
      size -= static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

[[nodiscard]] bool read_all(int descriptor, std::byte* data, std::size_t size) noexcept {
  while (size != 0U) {
    const ssize_t received = ::read(descriptor, data, size);
    if (received > 0) {
      data += received;
      size -= static_cast<std::size_t>(received);
      continue;
    }
    if (received < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

[[nodiscard]] std::size_t question_end(std::span<const std::byte> query) noexcept {
  if (query.size() < 17U) return 0U;
  std::size_t cursor = 12U;
  while (cursor < query.size()) {
    const std::size_t label = std::to_integer<std::uint8_t>(query[cursor]);
    ++cursor;
    if (label == 0U) break;
    if (label > query.size() - cursor) return 0U;
    cursor += label;
  }
  return cursor <= query.size() - 4U ? cursor + 4U : 0U;
}

[[nodiscard]] std::size_t make_response(
    std::span<const std::byte> query, std::span<std::byte> output,
    ReplyKind kind, bool truncated) noexcept {
  const std::size_t end = question_end(query);
  if (end == 0U || end > output.size()) return 0U;
  if (kind == ReplyKind::malformed) {
    output[0] = query[0];
    output[1] = query[1];
    output[2] = std::byte{0x80};
    return 3U;
  }
  std::copy(query.begin(), query.begin() + static_cast<std::ptrdiff_t>(end), output.begin());
  output[2] = truncated ? std::byte{0x83} : std::byte{0x81};
  output[3] = kind == ReplyKind::not_found ? std::byte{0x83} : std::byte{0x80};
  output[6] = std::byte{0};
  output[7] = (kind == ReplyKind::success || kind == ReplyKind::truncated_then_tcp) && !truncated
      ? std::byte{1} : std::byte{0};
  for (std::size_t index = 8; index < 12; ++index) output[index] = std::byte{0};
  if (kind == ReplyKind::not_found || truncated) return end;

  const std::uint16_t query_type = static_cast<std::uint16_t>(
      (std::to_integer<std::uint8_t>(query[end - 4U]) << 8U) |
      std::to_integer<std::uint8_t>(query[end - 3U]));
  const std::size_t address_size = query_type == 28U ? 16U : 4U;
  if (end + 12U + address_size > output.size()) return 0U;
  std::size_t cursor = end;
  output[cursor++] = std::byte{0xc0};
  output[cursor++] = std::byte{0x0c};
  output[cursor++] = query[end - 4U];
  output[cursor++] = query[end - 3U];
  output[cursor++] = std::byte{0};
  output[cursor++] = std::byte{1};
  output[cursor++] = std::byte{0};
  output[cursor++] = std::byte{0};
  output[cursor++] = std::byte{0};
  output[cursor++] = std::byte{42};
  output[cursor++] = std::byte{0};
  output[cursor++] = static_cast<std::byte>(address_size);
  if (query_type == 28U) {
    output[cursor++] = std::byte{0x20};
    output[cursor++] = std::byte{0x01};
    output[cursor++] = std::byte{0x0d};
    output[cursor++] = std::byte{0xb8};
    while (cursor < end + 12U + address_size - 1U) output[cursor++] = std::byte{0};
    output[cursor++] = std::byte{1};
  } else {
    output[cursor++] = std::byte{192};
    output[cursor++] = std::byte{0};
    output[cursor++] = std::byte{2};
    output[cursor++] = std::byte{7};
  }
  return cursor;
}

struct Fixture final {
  int udp{-1};
  int tcp{-1};
  std::uint16_t port{};
  pid_t child{-1};

  Fixture() noexcept = default;
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
  ~Fixture() {
    if (udp >= 0) ::close(udp);
    if (tcp >= 0) ::close(tcp);
    if (child > 0) {
      ::kill(child, SIGKILL);
      int status{};
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
  }
};

[[nodiscard]] bool bind_fixture(Fixture& fixture, bool tcp) noexcept {
  fixture.udp = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fixture.udp < 0) return false;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fixture.udp, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    return false;
  }
  socklen_t size = sizeof(address);
  if (::getsockname(fixture.udp, reinterpret_cast<sockaddr*>(&address), &size) != 0) return false;
  fixture.port = ntohs(address.sin_port);
  if (!tcp) return true;
  fixture.tcp = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fixture.tcp < 0) return false;
  int enabled = 1;
  if (::setsockopt(fixture.tcp, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0 ||
      ::bind(fixture.tcp, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(fixture.tcp, 2) != 0) {
    return false;
  }
  return true;
}

[[nodiscard]] bool serve_udp_once(int descriptor, ReplyKind kind) noexcept {
  std::array<std::byte, 512> query{};
  sockaddr_storage peer{};
  socklen_t peer_size = sizeof(peer);
  const ssize_t received = ::recvfrom(descriptor, query.data(), query.size(), 0,
      reinterpret_cast<sockaddr*>(&peer), &peer_size);
  if (received <= 0) return false;
  std::array<std::byte, 512> response{};
  const std::size_t response_size = make_response(
      std::span{query}.first(static_cast<std::size_t>(received)), response, kind,
      kind == ReplyKind::truncated_then_tcp);
  return response_size != 0U && ::sendto(descriptor, response.data(), response_size, 0,
      reinterpret_cast<const sockaddr*>(&peer), peer_size) ==
      static_cast<ssize_t>(response_size);
}

[[nodiscard]] bool serve_tcp_once(int listener) noexcept {
  const int connection = ::accept(listener, nullptr, nullptr);
  if (connection < 0) return false;
  std::array<std::byte, 2> prefix{};
  std::array<std::byte, 512> query{};
  bool passed = read_all(connection, prefix.data(), prefix.size());
  const std::size_t query_size = passed
      ? static_cast<std::size_t>((std::to_integer<std::uint8_t>(prefix[0]) << 8U) |
                                 std::to_integer<std::uint8_t>(prefix[1])) : 0U;
  passed = passed && query_size <= query.size() && read_all(connection, query.data(), query_size);
  std::array<std::byte, 512> response{};
  const std::size_t response_size = passed
      ? make_response(std::span{query}.first(query_size), response, ReplyKind::success, false) : 0U;
  prefix[0] = static_cast<std::byte>((response_size >> 8U) & 0xffU);
  prefix[1] = static_cast<std::byte>(response_size & 0xffU);
  passed = response_size != 0U && write_all(connection, prefix.data(), prefix.size()) &&
           write_all(connection, response.data(), response_size);
  ::close(connection);
  return passed;
}

[[nodiscard]] bool start_fixture(Fixture& fixture, ReplyKind kind) noexcept {
  const bool tcp = kind == ReplyKind::truncated_then_tcp;
  if (!bind_fixture(fixture, tcp)) return false;
  fixture.child = ::fork();
  if (fixture.child < 0) return false;
  if (fixture.child == 0) {
    const bool passed = serve_udp_once(fixture.udp, kind) &&
                        (!tcp || serve_tcp_once(fixture.tcp));
    _exit(passed ? 0 : 1);
  }
  return true;
}

[[nodiscard]] DnsNameserver nameserver(std::uint16_t port) noexcept {
  DnsNameserver server{};
  server.family = DnsAddressFamily::ipv4;
  server.bytes[0] = std::byte{127};
  server.bytes[3] = std::byte{1};
  server.port = port;
  return server;
}

[[nodiscard]] laghu::core::Result<DnsResolver> make_resolver(
    Completion& completion, std::span<const DnsNameserver> servers,
    std::size_t maximum_queries = 4U, std::uint32_t timeout = 50U) noexcept {
  return DnsResolver::create(
      DnsResolverConfig{{maximum_queries, 4U, timeout, 1U}, servers, &test_lifecycle},
      {&completion, completed});
}

[[nodiscard]] bool drive(DnsResolver& resolver, Completion& completion) noexcept {
  for (std::size_t attempt = 0; attempt < 100U && completion.calls == 0U; ++attempt) {
    std::array<DnsSocketInterest, DnsResolver::maximum_socket_capacity> interests{};
    const auto count = resolver.socket_interests(interests);
    if (!count.has_value()) return false;
    std::array<pollfd, DnsResolver::maximum_socket_capacity> descriptors{};
    for (std::size_t index = 0; index < *count; ++index) {
      descriptors[index].fd = interests[index].descriptor;
      descriptors[index].events = static_cast<short>(
          (interests[index].readable ? POLLIN : 0) |
          (interests[index].writable ? POLLOUT : 0));
    }
    const DnsTimeout timeout = resolver.next_timeout();
    const int wait = timeout.active && timeout.milliseconds < 10U
        ? static_cast<int>(timeout.milliseconds) : 10;
    const int ready = ::poll(descriptors.data(), static_cast<nfds_t>(*count), wait);
    if (ready < 0 && errno != EINTR) return false;
    std::array<DnsSocketEvent, DnsResolver::maximum_socket_capacity> events{};
    std::size_t event_count{};
    if (ready > 0) {
      for (std::size_t index = 0; index < *count; ++index) {
        if (descriptors[index].revents == 0) continue;
        events[event_count++] = {descriptors[index].fd,
            (descriptors[index].revents & (POLLIN | POLLERR | POLLHUP)) != 0,
            (descriptors[index].revents & POLLOUT) != 0};
      }
      if (!resolver.process_events(std::span{events}.first(event_count)).has_value()) return false;
    } else if (!resolver.process_timeout().has_value()) {
      return false;
    }
  }
  return completion.calls != 0U;
}

[[nodiscard]] bool query_fixture(ReplyKind kind, DnsQueryFamily family,
                                 Completion& completion) noexcept {
  Fixture fixture;
  if (!start_fixture(fixture, kind)) return false;
  const std::array servers{nameserver(fixture.port)};
  auto resolver = make_resolver(completion, servers);
  if (!resolver.has_value() ||
      !resolver->resolve(TextView::from("fixture.test"), family).has_value()) return false;
  return drive(*resolver, completion);
}

bool ipv4_ipv6_and_ttl() noexcept {
  Completion ipv4{};
  Completion ipv6{};
  return query_fixture(ReplyKind::success, DnsQueryFamily::ipv4, ipv4) &&
         ipv4.succeeded && ipv4.count == 1U &&
         ipv4.addresses[0].family == DnsAddressFamily::ipv4 &&
         ipv4.addresses[0].ttl_seconds == 42U &&
         query_fixture(ReplyKind::success, DnsQueryFamily::ipv6, ipv6) &&
         ipv6.succeeded && ipv6.count == 1U &&
         ipv6.addresses[0].family == DnsAddressFamily::ipv6 &&
         ipv6.addresses[0].ttl_seconds == 42U;
}

bool nxdomain() noexcept {
  Completion missing{};
  return query_fixture(ReplyKind::not_found, DnsQueryFamily::ipv4, missing) &&
         !missing.succeeded && missing.error == ErrorCode::unavailable_capability;
}

bool malformed_reply() noexcept {
  Completion malformed{};
  return query_fixture(ReplyKind::malformed, DnsQueryFamily::ipv4, malformed) &&
         !malformed.succeeded &&
         malformed.dependency == laghu::core::DependencyId::c_ares &&
         malformed.operation == laghu::core::DependencyOperation::dns_query;
}

bool tcp_fallback() noexcept {
  Completion completion{};
  return query_fixture(ReplyKind::truncated_then_tcp, DnsQueryFamily::ipv4, completion) &&
         completion.succeeded && completion.count == 1U &&
         completion.addresses[0].ttl_seconds == 42U;
}

bool cancellation_and_query_exhaustion() noexcept {
  Completion completion{};
  const std::array servers{nameserver(9)};
  auto resolver = make_resolver(completion, servers, 1U, 20U);
  if (!resolver.has_value()) return false;
  DnsResolver moved = std::move(*resolver);
  const auto first = moved.resolve(TextView::from("cancel.test"), DnsQueryFamily::ipv4);
  const auto exhausted = moved.resolve(TextView::from("second.test"), DnsQueryFamily::ipv4);
  const auto missing_output = moved.socket_interests({});
  if (!first.has_value() || exhausted.has_value() ||
      exhausted.error().code() != ErrorCode::exhaustion ||
      missing_output.has_value() || missing_output.error().code() != ErrorCode::invalid_range ||
      resolver->resolve(TextView::from("inactive.test")).has_value() ||
      !moved.cancel(*first).has_value() || completion.calls != 1U ||
      completion.error != ErrorCode::cancellation || moved.outstanding_queries() != 0U) {
    return false;
  }
  const auto replacement = moved.resolve(TextView::from("replacement.test"),
                                          DnsQueryFamily::ipv4);
  return replacement.has_value() && moved.outstanding_queries() == 1U &&
         moved.cancel(*replacement).has_value() && completion.calls == 2U &&
         completion.error == ErrorCode::cancellation && moved.outstanding_queries() == 0U &&
         !moved.cancel(*first).has_value();
}

bool completion_is_inactive_during_callback() noexcept {
  Fixture fixture;
  if (!start_fixture(fixture, ReplyKind::success)) return false;
  Completion completion{};
  const std::array servers{nameserver(fixture.port)};
  auto resolver = make_resolver(completion, servers, 1U);
  if (!resolver.has_value()) return false;
  completion.cancel_on_completion = &*resolver;
  const auto query = resolver->resolve(TextView::from("complete.test"), DnsQueryFamily::ipv4);
  return query.has_value() && drive(*resolver, completion) && completion.succeeded &&
         completion.calls == 1U && completion.completed_token_was_inactive &&
         resolver->outstanding_queries() == 0U;
}

bool socket_capacity_covers_query_capacity() noexcept {
  Completion completion{};
  const std::array servers{nameserver(9)};
  auto resolver = make_resolver(completion, servers, DnsResolver::maximum_query_capacity, 20U);
  if (!resolver.has_value()) return false;
  std::array<laghu::adapters::DnsQueryToken, DnsResolver::maximum_query_capacity> tokens{};
  for (auto& token : tokens) {
    const auto query = resolver->resolve(TextView::from("capacity.test"), DnsQueryFamily::ipv4);
    if (!query.has_value()) return false;
    token = *query;
  }
  std::array<DnsSocketInterest, DnsResolver::maximum_socket_capacity> interests{};
  const auto count = resolver->socket_interests(interests);
  if (!count.has_value() || *count <= 32U) return false;
  for (const auto token : tokens) {
    if (!resolver->cancel(token).has_value()) return false;
  }
  return resolver->outstanding_queries() == 0U;
}

bool completed_slot_supports_reentrant_submission() noexcept {
  Fixture fixture;
  if (!start_fixture(fixture, ReplyKind::success)) return false;
  Completion completion{};
  const std::array servers{nameserver(fixture.port)};
  auto resolver = make_resolver(completion, servers, DnsResolver::maximum_query_capacity, 50U);
  if (!resolver.has_value()) return false;
  completion.submit_on_completion = &*resolver;
  for (std::size_t index = 0; index < DnsResolver::maximum_query_capacity; ++index) {
    if (!resolver->resolve(TextView::from("occupied.test"), DnsQueryFamily::ipv4).has_value()) {
      return false;
    }
  }
  return drive(*resolver, completion) && completion.calls == 1U && completion.succeeded &&
         completion.replacement_submitted &&
         resolver->outstanding_queries() == DnsResolver::maximum_query_capacity;
}

bool event_callback_may_destroy_resolver() noexcept {
  Fixture fixture;
  if (!start_fixture(fixture, ReplyKind::success)) return false;
  Completion completion{};
  const std::array servers{nameserver(fixture.port)};
  auto created = make_resolver(completion, servers, 1U);
  if (!created.has_value()) return false;
  auto* resolver = new (std::nothrow) DnsResolver(std::move(*created));
  if (resolver == nullptr) return false;
  completion.destroy_on_completion = &resolver;
  if (!resolver->resolve(TextView::from("destroy.test"), DnsQueryFamily::ipv4).has_value()) {
    delete resolver;
    return false;
  }
  std::array<DnsSocketInterest, DnsResolver::maximum_socket_capacity> interests{};
  const auto count = resolver->socket_interests(interests);
  if (!count.has_value() || *count == 0U) {
    delete resolver;
    return false;
  }
  pollfd descriptor{interests[0].descriptor, POLLIN, 0};
  if (::poll(&descriptor, 1, 1000) <= 0) {
    delete resolver;
    return false;
  }
  const std::array events{
      DnsSocketEvent{descriptor.fd, true, false},
      DnsSocketEvent{descriptor.fd, true, false},
  };
  const auto processed = resolver->process_events(events);
  return processed.has_value() && resolver == nullptr && completion.calls == 1U;
}

bool timeout_callback_may_destroy_resolver() noexcept {
  Fixture silent;
  if (!bind_fixture(silent, false)) return false;
  Completion completion{};
  const std::array servers{nameserver(silent.port)};
  auto created = make_resolver(completion, servers, 2U, 1U);
  if (!created.has_value()) return false;
  auto* resolver = new (std::nothrow) DnsResolver(std::move(*created));
  if (resolver == nullptr) return false;
  completion.destroy_on_completion = &resolver;
  if (!resolver->resolve(TextView::from("first.test"), DnsQueryFamily::ipv4).has_value() ||
      !resolver->resolve(TextView::from("second.test"), DnsQueryFamily::ipv4).has_value()) {
    delete resolver;
    return false;
  }
  const DnsTimeout timeout = resolver->next_timeout();
  if (!timeout.active || timeout.milliseconds > 1000U) {
    delete resolver;
    return false;
  }
  (void)::poll(nullptr, 0, static_cast<int>(timeout.milliseconds + 10U));
  const auto processed = resolver->process_timeout();
  if (resolver != nullptr) delete resolver;
  return processed.has_value() && resolver == nullptr && completion.calls == 1U;
}

bool system_resolver_lookup() noexcept {
  Completion completion{};
  auto resolver = make_resolver(completion, {});
  if (!resolver.has_value() ||
      !resolver->resolve(TextView::from("localhost"), DnsQueryFamily::ipv4).has_value() ||
      !drive(*resolver, completion) || !completion.succeeded || completion.count == 0U) {
    return false;
  }
  for (std::size_t index = 0; index < completion.count; ++index) {
    const auto& address = completion.addresses[index];
    if (address.family == DnsAddressFamily::ipv4 &&
        address.bytes[0] == std::byte{127}) {
      return true;
    }
  }
  return false;
}

bool dependency_lifecycle_contract() noexcept {
  DnsDependencyLifecycle lifecycle;
  const auto hooks = lifecycle.hooks();
  Completion completion{};
  DnsResolverConfig config{{1U, 1U, 10U, 1U}, {}, &lifecycle};
  if (DnsResolver::create(config, {&completion, completed}).has_value() ||
      !hooks.preflight(hooks.context).has_value() ||
      !hooks.worker_initialize(hooks.context).has_value() ||
      hooks.live_state(hooks.context).sessions != 0U) {
    return false;
  }
  auto resolver = DnsResolver::create(config, {&completion, completed});
  if (!resolver.has_value() || hooks.live_state(hooks.context).sessions != 1U ||
      hooks.worker_cleanup(hooks.context).has_value()) {
    return false;
  }
  *resolver = DnsResolver{};
  return hooks.live_state(hooks.context).sessions == 0U &&
         hooks.worker_cleanup(hooks.context).has_value() &&
         hooks.master_cleanup(hooks.context).has_value();
}

bool timeout_and_input_validation() noexcept {
  Fixture silent;
  if (!bind_fixture(silent, false)) return false;
  Completion completion{};
  const std::array servers{nameserver(silent.port)};
  auto resolver = make_resolver(completion, servers, 1U, 10U);
  Completion system_completion{};
  auto system_resolver = make_resolver(system_completion, {});
  auto invalid_server = nameserver(0);
  const std::array invalid_servers{invalid_server};
  auto invalid_resolver = make_resolver(system_completion, invalid_servers);
  if (!resolver.has_value() || !system_resolver.has_value() || invalid_resolver.has_value()) {
    return false;
  }
  const std::array invalid_events{DnsSocketEvent{-1, true, false}};
  if (system_resolver->process_events(invalid_events).has_value()) return false;
  std::array<char, 254> oversized{};
  oversized.fill('a');
  const auto query = resolver->resolve(TextView::from("timeout.test"), DnsQueryFamily::ipv4);
  return query.has_value() && drive(*resolver, completion) && !completion.succeeded &&
         completion.error == ErrorCode::io && completion.native_code == ARES_ETIMEOUT &&
         !resolver->resolve(TextView::from(oversized.data(), oversized.size()).value()).has_value();
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"ipv4-ipv6-ttl", ipv4_ipv6_and_ttl},
      laghu::test::TestCase{"nxdomain", nxdomain},
      laghu::test::TestCase{"malformed-reply", malformed_reply},
      laghu::test::TestCase{"tcp-fallback", tcp_fallback},
      laghu::test::TestCase{"cancel-exhaustion", cancellation_and_query_exhaustion},
      laghu::test::TestCase{"completion-inactive", completion_is_inactive_during_callback},
      laghu::test::TestCase{"socket-capacity", socket_capacity_covers_query_capacity},
      laghu::test::TestCase{"reentrant-submit", completed_slot_supports_reentrant_submission},
      laghu::test::TestCase{"event-callback-destroy", event_callback_may_destroy_resolver},
      laghu::test::TestCase{"timeout-callback-destroy", timeout_callback_may_destroy_resolver},
      laghu::test::TestCase{"system-resolver", system_resolver_lookup},
      laghu::test::TestCase{"dependency-lifecycle", dependency_lifecycle_contract},
      laghu::test::TestCase{"timeout-input", timeout_and_input_validation},
  };
  const auto lifecycle = test_lifecycle.hooks();
  if (!lifecycle.worker_initialize(lifecycle.context).has_value()) return 2;
  const int result = laghu::test::run_tests(tests);
  if (!lifecycle.worker_cleanup(lifecycle.context).has_value()) return 2;
  return result;
}
