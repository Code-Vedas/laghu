// SPDX-License-Identifier: AGPL-3.0-only
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <ares.h>

#include <laghu/adapters/dns.hpp>

namespace laghu::adapters {
namespace {

struct State;

struct QuerySlot final {
  State* owner{};
  DnsQueryToken token;
  bool active{};
  bool canceled{};
};

struct SocketSlot final {
  int descriptor{-1};
  bool readable{};
  bool writable{};
};

struct State final {
  ares_channel_t* channel{};
  DnsResolverLimits limits{};
  DnsQuerySink sink{};
  DependencyLogSink log{};
  std::array<QuerySlot, DnsResolver::maximum_query_capacity> queries{};
  std::array<SocketSlot, DnsResolver::maximum_socket_capacity> sockets{};
  std::uint64_t next_token{1};
  std::size_t outstanding{};
  std::size_t socket_count{};
  bool socket_overflow{};
  bool shutting_down{};
};

[[nodiscard]] constexpr core::Error core_error(core::ErrorCode code,
                                                std::string_view diagnostic) noexcept {
  return {core::ErrorDomain::core, code, 0, diagnostic};
}

[[nodiscard]] constexpr core::DependencyStatus status_for(int status) noexcept {
  switch (status) {
    case ARES_ENOMEM:
      return core::DependencyStatus::exhaustion;
    case ARES_EBADNAME:
    case ARES_EBADQUERY:
    case ARES_EBADFAMILY:
    case ARES_ESERVICE:
      return core::DependencyStatus::invalid_input;
    case ARES_ENOTIMP:
    case ARES_ENOTINITIALIZED:
      return core::DependencyStatus::unavailable;
    case ARES_ETIMEOUT:
    case ARES_ECONNREFUSED:
    case ARES_ESERVFAIL:
      return core::DependencyStatus::io;
    case ARES_EFORMERR:
    case ARES_EBADRESP:
      return core::DependencyStatus::corrupt_data;
    case ARES_ENODATA:
    case ARES_ENOTFOUND:
      return core::DependencyStatus::unavailable;
    case ARES_ECANCELLED:
    case ARES_EDESTRUCTION:
      return core::DependencyStatus::unknown;
    default:
      return core::DependencyStatus::unknown;
  }
}

[[nodiscard]] core::Error native_error(core::DependencyOperation operation, int status,
                                       const DependencyLogSink& sink) noexcept {
  const core::Error error = normalize_dependency_error(
      core::DependencyId::c_ares, operation, status_for(status), status);
  log_dependency_error(sink, error);
  return error;
}

[[nodiscard]] QuerySlot* find_query(State& state, DnsQueryToken token) noexcept {
  if (!token.valid()) return nullptr;
  for (auto& query : state.queries) {
    if (query.active && query.token.value() == token.value()) return &query;
  }
  return nullptr;
}

void finish_query(QuerySlot& query) noexcept {
  if (!query.active) return;
  query.active = false;
  query.canceled = false;
  if (query.owner != nullptr && query.owner->outstanding != 0U) {
    --query.owner->outstanding;
  }
}

void socket_state(void* context, ares_socket_t descriptor, int readable, int writable) {
  auto& state = *static_cast<State*>(context);
  for (std::size_t index = 0; index < state.socket_count; ++index) {
    if (state.sockets[index].descriptor != descriptor) continue;
    if (readable == 0 && writable == 0) {
      state.sockets[index] = state.sockets[state.socket_count - 1U];
      --state.socket_count;
    } else {
      state.sockets[index].readable = readable != 0;
      state.sockets[index].writable = writable != 0;
    }
    return;
  }
  if (readable == 0 && writable == 0) return;
  if (state.socket_count == state.sockets.size()) {
    state.socket_overflow = true;
    return;
  }
  state.sockets[state.socket_count] = {descriptor, readable != 0, writable != 0};
  ++state.socket_count;
}

[[nodiscard]] bool copy_address(const ares_addrinfo_node& node,
                                DnsAddress& output) noexcept {
  if (node.ai_addr == nullptr || node.ai_ttl < 0) return false;
  if (node.ai_family == AF_INET && node.ai_addrlen >= sizeof(sockaddr_in)) {
    sockaddr_in address{};
    std::memcpy(&address, node.ai_addr, sizeof(address));
    output.family = DnsAddressFamily::ipv4;
    std::memcpy(output.bytes.data(), &address.sin_addr, sizeof(address.sin_addr));
  } else if (node.ai_family == AF_INET6 && node.ai_addrlen >= sizeof(sockaddr_in6)) {
    sockaddr_in6 address{};
    std::memcpy(&address, node.ai_addr, sizeof(address));
    output.family = DnsAddressFamily::ipv6;
    std::memcpy(output.bytes.data(), &address.sin6_addr, sizeof(address.sin6_addr));
  } else {
    return false;
  }
  output.ttl_seconds = static_cast<std::uint32_t>(node.ai_ttl);
  return true;
}

void address_complete(void* context, int status, int, ares_addrinfo* native_result) {
  auto& query = *static_cast<QuerySlot*>(context);
  State& state = *query.owner;
  if (query.canceled || state.shutting_down) {
    if (native_result != nullptr) ares_freeaddrinfo(native_result);
    finish_query(query);
    return;
  }

  std::array<DnsAddress, DnsResolver::maximum_address_capacity> addresses{};
  std::size_t count{};
  core::Error error{core::ErrorDomain::core, core::ErrorCode::invalid_state};
  const core::Error* failure{};
  if (status != ARES_SUCCESS) {
    error = native_error(core::DependencyOperation::dns_query, status, state.log);
    failure = &error;
  } else {
    for (ares_addrinfo_node* node = native_result == nullptr ? nullptr : native_result->nodes;
         node != nullptr; node = node->ai_next) {
      DnsAddress address{};
      if (!copy_address(*node, address)) continue;
      if (count == state.limits.maximum_result_addresses) {
        error = core_error(core::ErrorCode::exhaustion,
                           "DNS result exceeds caller address capacity");
        failure = &error;
        count = 0;
        break;
      }
      addresses[count] = address;
      ++count;
    }
    if (failure == nullptr && count == 0U) {
      error = native_error(core::DependencyOperation::dns_query, ARES_ENODATA, state.log);
      failure = &error;
    }
  }
  if (native_result != nullptr) ares_freeaddrinfo(native_result);
  const DnsQueryToken token = query.token;
  const DnsQuerySink sink = state.sink;
  finish_query(query);
  const DnsQueryResult result{token, std::span{addresses}.first(count), failure};
  sink.complete(sink.context, result);
}

[[nodiscard]] core::Result<void> append_nameserver(
    const DnsNameserver& nameserver, std::span<char> output, std::size_t& size) noexcept {
  if (nameserver.port == 0U) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "DNS nameserver port must be nonzero")};
  }
  std::array<char, INET6_ADDRSTRLEN> address{};
  const void* source{};
  int family{};
  if (nameserver.family == DnsAddressFamily::ipv4) {
    family = AF_INET;
    source = nameserver.bytes.data();
  } else if (nameserver.family == DnsAddressFamily::ipv6) {
    family = AF_INET6;
    source = nameserver.bytes.data();
  } else {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "DNS nameserver family is invalid")};
  }
  if (inet_ntop(family, source, address.data(), address.size()) == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "DNS nameserver address is invalid")};
  }
  const std::string_view text{address.data()};
  const std::size_t required = text.size() + 1U + 5U +
      (family == AF_INET6 ? 2U : 0U) + (size == 0U ? 0U : 1U);
  if (required > output.size() - size) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "DNS nameserver list exceeds bounded storage")};
  }
  if (size != 0U) output[size++] = ',';
  if (family == AF_INET6) output[size++] = '[';
  std::copy(text.begin(), text.end(), output.begin() + static_cast<std::ptrdiff_t>(size));
  size += text.size();
  if (family == AF_INET6) output[size++] = ']';
  output[size++] = ':';
  const auto converted = std::to_chars(output.data() + size, output.data() + output.size() - 1,
                                       nameserver.port);
  if (converted.ec != std::errc{}) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "DNS nameserver port does not fit bounded storage")};
  }
  size = static_cast<std::size_t>(converted.ptr - output.data());
  output[size] = '\0';
  return {};
}

}  // namespace

DnsResolver::DnsResolver(DnsResolver&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}

DnsResolver& DnsResolver::operator=(DnsResolver&& other) noexcept {
  if (this != &other) {
    release();
    state_ = std::exchange(other.state_, nullptr);
  }
  return *this;
}

DnsResolver::~DnsResolver() { release(); }

core::Result<DnsResolver> DnsResolver::create(
    DnsResolverConfig config, DnsQuerySink sink, DependencyLogSink log_sink) noexcept {
  if (config.limits.maximum_outstanding_queries == 0U ||
      config.limits.maximum_outstanding_queries > maximum_query_capacity ||
      config.limits.maximum_result_addresses == 0U ||
      config.limits.maximum_result_addresses > maximum_address_capacity ||
      config.limits.timeout_milliseconds == 0U || config.limits.attempts == 0U ||
      config.limits.timeout_milliseconds > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      config.limits.attempts > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      config.nameservers.size() > maximum_nameserver_capacity || sink.complete == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "DNS resolver limits, nameservers, or callback are invalid")};
  }
  auto* state = new (std::nothrow) State{};
  if (state == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::exhaustion,
                                      "DNS resolver state allocation failed")};
  }
  state->limits = config.limits;
  state->sink = sink;
  state->log = log_sink;
  for (auto& query : state->queries) query.owner = state;

  ares_options options{};
  options.flags = ARES_FLAG_NOSEARCH;
  options.timeout = static_cast<int>(config.limits.timeout_milliseconds);
  options.tries = static_cast<int>(config.limits.attempts);
  options.sock_state_cb = socket_state;
  options.sock_state_cb_data = state;
  int option_mask = ARES_OPT_FLAGS | ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES |
                    ARES_OPT_SOCK_STATE_CB;
#if defined(ARES_OPT_QUERY_CACHE)
  // c-ares 1.31+ caches answers by default. Laghu owns caching policy, so the
  // adapter disables the native cache. Older accepted c-ares releases have no
  // native query cache and do not expose this option.
  options.qcache_max_ttl = 0;
  option_mask |= ARES_OPT_QUERY_CACHE;
#endif
  const int initialized = ares_init_options(&state->channel, &options, option_mask);
  if (initialized != ARES_SUCCESS) {
    const core::Error error = native_error(core::DependencyOperation::dns_session,
                                            initialized, log_sink);
    delete state;
    return std::unexpected{error};
  }

  if (!config.nameservers.empty()) {
    std::array<char, 512> csv{};
    std::size_t size{};
    for (const auto& nameserver : config.nameservers) {
      if (const auto appended = append_nameserver(nameserver, csv, size);
          !appended.has_value()) {
        ares_destroy(state->channel);
        delete state;
        return std::unexpected{appended.error()};
      }
    }
    const int configured = ares_set_servers_ports_csv(state->channel, csv.data());
    if (configured != ARES_SUCCESS) {
      const core::Error error = native_error(core::DependencyOperation::dns_session,
                                              configured, log_sink);
      ares_destroy(state->channel);
      delete state;
      return std::unexpected{error};
    }
  }
  return DnsResolver{state};
}

core::Result<DnsQueryToken> DnsResolver::resolve(core::TextView hostname,
                                                 DnsQueryFamily family) noexcept {
  if (state_ == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "DNS resolver is inactive")};
  }
  auto& state = *static_cast<State*>(state_);
  if (hostname.empty() || hostname.size() > 253U) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "DNS hostname must contain 1 to 253 bytes")};
  }
  if (family != DnsQueryFamily::ipv4 && family != DnsQueryFamily::ipv6 &&
      family != DnsQueryFamily::any) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "DNS query family is invalid")};
  }
  const auto native_hostname = hostname.to_c_string<254>();
  if (!native_hostname.has_value()) return std::unexpected{native_hostname.error()};
  if (state.outstanding == state.limits.maximum_outstanding_queries) {
    return std::unexpected{core_error(core::ErrorCode::exhaustion,
                                      "DNS outstanding query limit reached")};
  }
  QuerySlot* slot{};
  for (auto& candidate : state.queries) {
    if (!candidate.active) {
      slot = &candidate;
      break;
    }
  }
  if (slot == nullptr || state.next_token == 0U) {
    return std::unexpected{core_error(core::ErrorCode::exhaustion,
                                      "DNS query token capacity exhausted")};
  }
  slot->token = DnsQueryToken{state.next_token++};
  const DnsQueryToken token = slot->token;
  slot->active = true;
  slot->canceled = false;
  ++state.outstanding;
  ares_addrinfo_hints hints{};
  hints.ai_family = family == DnsQueryFamily::ipv4 ? AF_INET
      : family == DnsQueryFamily::ipv6 ? AF_INET6 : AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  ares_getaddrinfo(state.channel, native_hostname->c_str(), nullptr, &hints,
                   address_complete, slot);
  return token;
}

core::Result<void> DnsResolver::cancel(DnsQueryToken token) noexcept {
  if (state_ == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "DNS resolver is inactive")};
  }
  auto& state = *static_cast<State*>(state_);
  QuerySlot* const query = find_query(state, token);
  if (query == nullptr || query->canceled) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "DNS query token is inactive")};
  }
  query->canceled = true;
  const core::Error error{core::ErrorDomain::core, core::ErrorCode::cancellation, 0,
                          "DNS query was canceled"};
  const DnsQueryResult result{token, {}, &error};
  state.sink.complete(state.sink.context, result);
  return {};
}

core::Result<std::size_t> DnsResolver::socket_interests(
    std::span<DnsSocketInterest> output) const noexcept {
  if (state_ == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "DNS resolver is inactive")};
  }
  const auto& state = *static_cast<const State*>(state_);
  if (state.socket_overflow) {
    return std::unexpected{core_error(core::ErrorCode::exhaustion,
                                      "DNS socket interest capacity exhausted")};
  }
  if (output.size() < state.socket_count) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "DNS socket interest output is too small")};
  }
  for (std::size_t index = 0; index < state.socket_count; ++index) {
    output[index] = {state.sockets[index].descriptor, state.sockets[index].readable,
                     state.sockets[index].writable};
  }
  return state.socket_count;
}

DnsTimeout DnsResolver::next_timeout() const noexcept {
  if (state_ == nullptr) return {};
  const auto& state = *static_cast<const State*>(state_);
  timeval timeout{};
  if (ares_timeout(state.channel, nullptr, &timeout) == nullptr) return {};
  const std::uint64_t seconds = timeout.tv_sec < 0 ? 0U : static_cast<std::uint64_t>(timeout.tv_sec);
  const std::uint64_t microseconds = timeout.tv_usec < 0 ? 0U :
      static_cast<std::uint64_t>(timeout.tv_usec);
  return {true, seconds * 1000U + (microseconds + 999U) / 1000U};
}

core::Result<void> DnsResolver::process_events(
    std::span<const DnsSocketEvent> events) noexcept {
  if (state_ == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "DNS resolver is inactive")};
  }
  auto& state = *static_cast<State*>(state_);
  for (const auto& event : events) {
    if (event.descriptor < 0 || (!event.readable && !event.writable)) {
      return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                        "DNS socket event is invalid")};
    }
    ares_process_fd(state.channel, event.readable ? event.descriptor : ARES_SOCKET_BAD,
                    event.writable ? event.descriptor : ARES_SOCKET_BAD);
  }
  if (state.socket_overflow) {
    return std::unexpected{core_error(core::ErrorCode::exhaustion,
                                      "DNS socket interest capacity exhausted")};
  }
  return {};
}

core::Result<void> DnsResolver::process_timeout() noexcept {
  if (state_ == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "DNS resolver is inactive")};
  }
  auto& state = *static_cast<State*>(state_);
  ares_process_fd(state.channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
  return {};
}

std::size_t DnsResolver::outstanding_queries() const noexcept {
  return state_ == nullptr ? 0U : static_cast<const State*>(state_)->outstanding;
}

void DnsResolver::release() noexcept {
  if (state_ == nullptr) return;
  auto* state = static_cast<State*>(state_);
  state->shutting_down = true;
  ares_destroy(state->channel);
  delete state;
  state_ = nullptr;
}

}  // namespace laghu::adapters
