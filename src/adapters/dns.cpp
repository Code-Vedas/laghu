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
#include <unistd.h>

#include <ares.h>

#include <laghu/adapters/dns.hpp>

namespace laghu::adapters {
namespace {

struct State;

struct QuerySlot final {
  State* owner{};
  ares_channel channel{};
  DnsQueryToken token;
  std::array<DnsAddress, DnsResolver::maximum_address_capacity> completed_addresses{};
  core::Error completed_error{core::ErrorDomain::core, core::ErrorCode::invalid_state};
  std::size_t completed_address_count{};
  bool active{};
  bool native_done{};
  bool canceling{};
  bool socket_capacity_failed{};
  bool completed_with_error{};
};

struct SocketSlot final {
  int descriptor{-1};
  QuerySlot* query{};
  bool readable{};
  bool writable{};
};

struct State final {
  DnsDependencyLifecycle* lifecycle{};
  DnsResolverLimits limits{};
  DnsQuerySink sink{};
  DependencyLogSink log{};
  std::array<QuerySlot, DnsResolver::maximum_query_capacity> queries{};
  std::array<SocketSlot, DnsResolver::maximum_socket_capacity> sockets{};
  std::array<char, 512> nameserver_csv{};
  std::uint64_t next_token{1};
  std::size_t outstanding{};
  std::size_t socket_count{};
  bool has_explicit_nameservers{};
  bool shutting_down{};
};

[[nodiscard]] std::uint64_t current_process() noexcept {
  const pid_t process = ::getpid();
  return process > 0 ? static_cast<std::uint64_t>(process) : 0U;
}

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
    if (query.active && !query.native_done && query.token.value() == token.value()) return &query;
  }
  return nullptr;
}

void finish_query(QuerySlot& query) noexcept {
  if (!query.active) return;
  query.active = false;
  query.native_done = false;
  query.canceling = false;
  query.socket_capacity_failed = false;
  query.completed_address_count = 0;
  query.completed_with_error = false;
  query.channel = nullptr;
}

void socket_state(void* context, ares_socket_t descriptor, int readable, int writable) {
  auto& query = *static_cast<QuerySlot*>(context);
  auto& state = *query.owner;
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
    query.socket_capacity_failed = true;
    return;
  }
  state.sockets[state.socket_count] = {descriptor, &query, readable != 0, writable != 0};
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
  if (state.shutting_down) {
    if (native_result != nullptr) ares_freeaddrinfo(native_result);
    query.native_done = true;
    return;
  }

  if (query.canceling && status == ARES_ECANCELLED) {
    if (native_result != nullptr) ares_freeaddrinfo(native_result);
    query.native_done = true;
    if (state.outstanding != 0U) --state.outstanding;
    return;
  }

  query.completed_address_count = 0;
  query.completed_with_error = false;
  if (status != ARES_SUCCESS) {
    query.completed_error = native_error(core::DependencyOperation::dns_query, status, state.log);
    query.completed_with_error = true;
  } else {
    for (ares_addrinfo_node* node = native_result == nullptr ? nullptr : native_result->nodes;
         node != nullptr; node = node->ai_next) {
      DnsAddress address{};
      if (!copy_address(*node, address)) continue;
      if (query.completed_address_count == state.limits.maximum_result_addresses) {
        query.completed_error = core_error(core::ErrorCode::exhaustion,
                                            "DNS result exceeds caller address capacity");
        query.completed_with_error = true;
        query.completed_address_count = 0;
        break;
      }
      query.completed_addresses[query.completed_address_count] = address;
      ++query.completed_address_count;
    }
    if (!query.completed_with_error && query.completed_address_count == 0U) {
      query.completed_error = native_error(core::DependencyOperation::dns_query,
                                            ARES_ENODATA, state.log);
      query.completed_with_error = true;
    }
  }
  if (native_result != nullptr) ares_freeaddrinfo(native_result);
  query.native_done = true;
  if (state.outstanding != 0U) --state.outstanding;
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

[[nodiscard]] core::Result<void> initialize_channel(State& state,
                                                    QuerySlot& query) noexcept {
  ares_options options{};
  options.flags = ARES_FLAG_NOSEARCH;
  options.timeout = static_cast<int>(state.limits.timeout_milliseconds);
  options.tries = static_cast<int>(state.limits.attempts);
  options.sock_state_cb = socket_state;
  options.sock_state_cb_data = &query;
  int option_mask = ARES_OPT_FLAGS | ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES |
                    ARES_OPT_SOCK_STATE_CB;
#if defined(ARES_OPT_QUERY_CACHE)
  // c-ares 1.31+ caches answers by default. Laghu owns caching policy, so the
  // adapter disables the native cache. Older accepted c-ares releases have no
  // native query cache and do not expose this option.
  options.qcache_max_ttl = 0;
  option_mask |= ARES_OPT_QUERY_CACHE;
#endif
  const int initialized = ares_init_options(&query.channel, &options, option_mask);
  if (initialized != ARES_SUCCESS) {
    return std::unexpected{native_error(core::DependencyOperation::dns_session,
                                         initialized, state.log)};
  }
  if (state.has_explicit_nameservers) {
    const int configured = ares_set_servers_ports_csv(query.channel,
                                                       state.nameserver_csv.data());
    if (configured != ARES_SUCCESS) {
      const core::Error error = native_error(core::DependencyOperation::dns_session,
                                              configured, state.log);
      ares_destroy(query.channel);
      query.channel = nullptr;
      return std::unexpected{error};
    }
  }
  return {};
}

void retire_channel(QuerySlot& query) noexcept {
  if (query.channel != nullptr) ares_destroy(query.channel);
  finish_query(query);
}

void dispatch_completion(QuerySlot& query) noexcept {
  State& state = *query.owner;
  const DnsQueryToken token = query.token;
  const DnsQuerySink sink = state.sink;
  const auto addresses = query.completed_addresses;
  const std::size_t address_count = query.completed_address_count;
  const core::Error error = query.completed_error;
  const bool failed = query.completed_with_error;
  retire_channel(query);
  const DnsQueryResult result{token, std::span{addresses}.first(address_count),
                              failed ? &error : nullptr};
  sink.complete(sink.context, result);
}

void fail_socket_capacity(QuerySlot& query) noexcept {
  State& state = *query.owner;
  const DnsQueryToken token = query.token;
  const DnsQuerySink sink = state.sink;
  query.canceling = true;
  ares_cancel(query.channel);
  retire_channel(query);
  const core::Error error = core_error(core::ErrorCode::exhaustion,
                                       "DNS socket interest capacity exhausted");
  const DnsQueryResult result{token, {}, &error};
  sink.complete(sink.context, result);
}

static_assert(DnsResolver::maximum_socket_capacity >=
              DnsResolver::maximum_query_capacity * ARES_GETSOCK_MAXNUM);

}  // namespace

DependencyLifecycleHooks DnsDependencyLifecycle::hooks() noexcept {
  return {core::DependencyId::c_ares, this, preflight, worker_initialize,
          worker_cleanup, master_cleanup, live_state};
}

core::Result<void> DnsDependencyLifecycle::preflight(void* context) noexcept {
  auto* lifecycle = static_cast<DnsDependencyLifecycle*>(context);
  if (lifecycle == nullptr || lifecycle->initialized_ || lifecycle->live_resolvers_ != 0U) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "c-ares preflight requires inactive lifecycle")};
  }
  return {};
}

core::Result<void> DnsDependencyLifecycle::worker_initialize(void* context) noexcept {
  auto* lifecycle = static_cast<DnsDependencyLifecycle*>(context);
  const std::uint64_t process = current_process();
  if (lifecycle == nullptr || process == 0U || lifecycle->live_resolvers_ != 0U ||
      (lifecycle->initialized_ && lifecycle->process_ != process)) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "c-ares worker initialization state is invalid")};
  }
  if (lifecycle->initialized_) return {};
  const int status = ares_library_init(ARES_LIB_INIT_ALL);
  if (status != ARES_SUCCESS) {
    return std::unexpected{normalize_dependency_error(
        core::DependencyId::c_ares, core::DependencyOperation::dns_session,
        status_for(status), status)};
  }
  lifecycle->process_ = process;
  lifecycle->initialized_ = true;
  return {};
}

core::Result<void> DnsDependencyLifecycle::worker_cleanup(void* context) noexcept {
  auto* lifecycle = static_cast<DnsDependencyLifecycle*>(context);
  if (lifecycle == nullptr ||
      (lifecycle->initialized_ && lifecycle->process_ != current_process()) ||
      lifecycle->live_resolvers_ != 0U) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "c-ares cleanup requires quiescent worker state")};
  }
  if (lifecycle->initialized_) ares_library_cleanup();
  lifecycle->process_ = 0U;
  lifecycle->initialized_ = false;
  return {};
}

core::Result<void> DnsDependencyLifecycle::master_cleanup(void* context) noexcept {
  auto* lifecycle = static_cast<DnsDependencyLifecycle*>(context);
  if (lifecycle == nullptr || lifecycle->initialized_ || lifecycle->live_resolvers_ != 0U) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "c-ares master cleanup requires inactive lifecycle")};
  }
  return {};
}

DependencyLiveState DnsDependencyLifecycle::live_state(void* context) noexcept {
  const auto* lifecycle = static_cast<const DnsDependencyLifecycle*>(context);
  return lifecycle == nullptr ? DependencyLiveState{}
                              : DependencyLiveState{lifecycle->live_resolvers_, 0U, 0U, 0U};
}

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
  if (config.lifecycle == nullptr || !config.lifecycle->initialized_ ||
      config.lifecycle->process_ != current_process()) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "DNS resolver requires initialized worker lifecycle")};
  }
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
  state->lifecycle = config.lifecycle;
  state->sink = sink;
  state->log = log_sink;
  for (auto& query : state->queries) query.owner = state;

  if (!config.nameservers.empty()) {
    std::size_t size{};
    for (const auto& nameserver : config.nameservers) {
      if (const auto appended = append_nameserver(nameserver, state->nameserver_csv, size);
          !appended.has_value()) {
        delete state;
        return std::unexpected{appended.error()};
      }
    }
    state->has_explicit_nameservers = true;
  }
  ++state->lifecycle->live_resolvers_;
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
  slot->native_done = false;
  slot->canceling = false;
  slot->socket_capacity_failed = false;
  slot->completed_address_count = 0;
  slot->completed_with_error = false;
  if (const auto initialized = initialize_channel(state, *slot); !initialized.has_value()) {
    finish_query(*slot);
    return std::unexpected{initialized.error()};
  }
  ++state.outstanding;
  ares_addrinfo_hints hints{};
  hints.ai_family = family == DnsQueryFamily::ipv4 ? AF_INET
      : family == DnsQueryFamily::ipv6 ? AF_INET6 : AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  ares_getaddrinfo(slot->channel, native_hostname->c_str(), nullptr, &hints,
                   address_complete, slot);
  if (slot->native_done) {
    dispatch_completion(*slot);
  } else if (slot->socket_capacity_failed) {
    fail_socket_capacity(*slot);
  }
  return token;
}

core::Result<void> DnsResolver::cancel(DnsQueryToken token) noexcept {
  if (state_ == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "DNS resolver is inactive")};
  }
  auto& state = *static_cast<State*>(state_);
  QuerySlot* const query = find_query(state, token);
  if (query == nullptr || query->canceling) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "DNS query token is inactive")};
  }
  query->canceling = true;
  ares_cancel(query->channel);
  if (!query->native_done) {
    query->canceling = false;
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "DNS dependency did not complete cancellation")};
  }
  retire_channel(*query);
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
  DnsTimeout result{};
  for (const auto& query : state.queries) {
    if (!query.active || query.channel == nullptr) continue;
    timeval timeout{};
    if (ares_timeout(query.channel, nullptr, &timeout) == nullptr) continue;
    const std::uint64_t seconds = timeout.tv_sec < 0 ? 0U :
        static_cast<std::uint64_t>(timeout.tv_sec);
    const std::uint64_t microseconds = timeout.tv_usec < 0 ? 0U :
        static_cast<std::uint64_t>(timeout.tv_usec);
    const std::uint64_t milliseconds = seconds * 1000U + (microseconds + 999U) / 1000U;
    if (!result.active || milliseconds < result.milliseconds) {
      result = {true, milliseconds};
    }
  }
  return result;
}

core::Result<void> DnsResolver::process_events(
    std::span<const DnsSocketEvent> events) noexcept {
  if (state_ == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "DNS resolver is inactive")};
  }
  auto& state = *static_cast<State*>(state_);
  if (events.size() > maximum_socket_capacity) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "DNS socket event input exceeds capacity")};
  }
  std::array<QuerySlot*, maximum_socket_capacity> event_queries{};
  std::array<std::uint64_t, maximum_socket_capacity> event_tokens{};
  for (std::size_t event_index = 0; event_index < events.size(); ++event_index) {
    const auto& event = events[event_index];
    if (event.descriptor < 0 || (!event.readable && !event.writable)) {
      return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                        "DNS socket event is invalid")};
    }
    QuerySlot* query{};
    for (std::size_t index = 0; index < state.socket_count; ++index) {
      if (state.sockets[index].descriptor == event.descriptor) {
        query = state.sockets[index].query;
        break;
      }
    }
    if (query == nullptr || !query->active || query->channel == nullptr) {
      return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                        "DNS socket event is not registered")};
    }
    event_queries[event_index] = query;
    event_tokens[event_index] = query->token.value();
  }
  for (std::size_t event_index = 0; event_index < events.size(); ++event_index) {
    const auto& event = events[event_index];
    QuerySlot* const query = event_queries[event_index];
    if (!query->active || query->channel == nullptr ||
        query->token.value() != event_tokens[event_index]) {
      continue;
    }
    ares_process_fd(query->channel, event.readable ? event.descriptor : ARES_SOCKET_BAD,
                    event.writable ? event.descriptor : ARES_SOCKET_BAD);
    if (query->native_done) {
      dispatch_completion(*query);
      return {};
    } else if (query->socket_capacity_failed) {
      fail_socket_capacity(*query);
      return {};
    }
  }
  return {};
}

core::Result<void> DnsResolver::process_timeout() noexcept {
  if (state_ == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "DNS resolver is inactive")};
  }
  auto& state = *static_cast<State*>(state_);
  std::array<QuerySlot*, maximum_query_capacity> pending{};
  std::array<std::uint64_t, maximum_query_capacity> pending_tokens{};
  std::size_t count{};
  for (auto& query : state.queries) {
    if (query.active && query.channel != nullptr) {
      pending[count] = &query;
      pending_tokens[count] = query.token.value();
      ++count;
    }
  }
  for (std::size_t index = 0; index < count; ++index) {
    QuerySlot& query = *pending[index];
    if (!query.active || query.channel == nullptr ||
        query.token.value() != pending_tokens[index]) {
      continue;
    }
    ares_process_fd(query.channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
    if (query.native_done) {
      dispatch_completion(query);
      return {};
    } else if (query.socket_capacity_failed) {
      fail_socket_capacity(query);
      return {};
    }
  }
  return {};
}

std::size_t DnsResolver::outstanding_queries() const noexcept {
  return state_ == nullptr ? 0U : static_cast<const State*>(state_)->outstanding;
}

void DnsResolver::release() noexcept {
  if (state_ == nullptr) return;
  auto* state = static_cast<State*>(state_);
  state->shutting_down = true;
  for (auto& query : state->queries) {
    if (query.channel != nullptr) ares_destroy(query.channel);
  }
  DnsDependencyLifecycle* const lifecycle = state->lifecycle;
  delete state;
  if (lifecycle != nullptr && lifecycle->live_resolvers_ != 0U) {
    --lifecycle->live_resolvers_;
  }
  state_ = nullptr;
}

}  // namespace laghu::adapters
