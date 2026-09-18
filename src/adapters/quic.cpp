// SPDX-License-Identifier: AGPL-3.0-only
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <netinet/in.h>
#include <span>
#include <sys/socket.h>
#include <utility>

#include <ngtcp2/ngtcp2.h>

#include <laghu/adapters/internal/arena_memory.hpp>
#include <laghu/adapters/quic.hpp>

namespace laghu::adapters {
namespace {

struct State final {
  internal::ArenaMemory memory;
  ngtcp2_mem native_memory{};
  ngtcp2_path_storage path{};
  QuicCryptoCallbacks crypto{};
  QuicEventSink events{};
  QuicConnectionIdSink connection_ids{};
  DependencyLogSink log{};
  QuicRole role{QuicRole::client};
  core::WorkerId worker;
  core::GenerationId generation;
  std::array<std::byte, QuicConnectionId::capacity> initial_dcid{};
  std::size_t initial_dcid_size{};
  std::size_t application_iv_size{};
  std::size_t maximum_packet_bytes{};
  bool random_failed{};
  bool terminal{};
  bool packet_pending_transmit{};
};

struct ActionContext final {
  ngtcp2_conn* connection{};
  State* state{};
  std::uint32_t negotiated_version{};
};

[[nodiscard]] core::Error core_error(core::ErrorCode code, const char* text) noexcept {
  return {core::ErrorDomain::core, code, 0, text};
}

[[nodiscard]] core::DependencyStatus status_for(int code) noexcept {
  if (code == NGTCP2_ERR_NOMEM || code == NGTCP2_ERR_STREAM_ID_BLOCKED ||
      code == NGTCP2_ERR_STREAM_DATA_BLOCKED) return core::DependencyStatus::exhaustion;
  if (code == NGTCP2_ERR_INVALID_ARGUMENT) return core::DependencyStatus::invalid_input;
  if (code == NGTCP2_ERR_CRYPTO) return core::DependencyStatus::crypto;
  return core::DependencyStatus::corrupt_data;
}

[[nodiscard]] bool terminal_error(int code) noexcept {
  return code == NGTCP2_ERR_RETRY || code == NGTCP2_ERR_DROP_CONN ||
         code == NGTCP2_ERR_DRAINING || code == NGTCP2_ERR_CLOSING ||
         ngtcp2_err_is_fatal(code) != 0;
}

[[nodiscard]] bool valid_endpoint(const QuicEndpoint& endpoint) noexcept {
  return (endpoint.family == QuicAddressFamily::ipv4 ||
          endpoint.family == QuicAddressFamily::ipv6) && endpoint.port != 0U;
}

[[nodiscard]] bool store_endpoint(const QuicEndpoint& endpoint,
                                  ngtcp2_sockaddr_union& storage,
                                  ngtcp2_addr& output) noexcept {
  if (!valid_endpoint(endpoint)) return false;
  std::memset(&storage, 0, sizeof(storage));
  if (endpoint.family == QuicAddressFamily::ipv4) {
    auto& address = storage.in;
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    std::memcpy(&address.sin_addr, endpoint.address.data(), 4U);
    output = {reinterpret_cast<ngtcp2_sockaddr*>(&address), sizeof(address)};
    return true;
  }
  auto& address = storage.in6;
  address.sin6_family = AF_INET6;
  address.sin6_port = htons(endpoint.port);
  std::memcpy(&address.sin6_addr, endpoint.address.data(), 16U);
  output = {reinterpret_cast<ngtcp2_sockaddr*>(&address), sizeof(address)};
  return true;
}

[[nodiscard]] bool store_path(const QuicPath& path, ngtcp2_path_storage& output) noexcept {
  output.path.user_data = nullptr;
  return store_endpoint(path.local, output.local_addrbuf, output.path.local) &&
         store_endpoint(path.remote, output.remote_addrbuf, output.path.remote);
}

[[nodiscard]] QuicEndpoint endpoint_from(const ngtcp2_addr& input) noexcept {
  QuicEndpoint output{};
  if (input.addr != nullptr && input.addr->sa_family == AF_INET6 &&
      input.addrlen >= sizeof(sockaddr_in6)) {
    const auto& address = *reinterpret_cast<const sockaddr_in6*>(input.addr);
    output.family = QuicAddressFamily::ipv6;
    output.port = ntohs(address.sin6_port);
    std::memcpy(output.address.data(), &address.sin6_addr, 16U);
  } else if (input.addr != nullptr && input.addr->sa_family == AF_INET &&
             input.addrlen >= sizeof(sockaddr_in)) {
    const auto& address = *reinterpret_cast<const sockaddr_in*>(input.addr);
    output.family = QuicAddressFamily::ipv4;
    output.port = ntohs(address.sin_port);
    std::memcpy(output.address.data(), &address.sin_addr, 4U);
  }
  return output;
}

[[nodiscard]] QuicPath path_from(const ngtcp2_path& input) noexcept {
  return {endpoint_from(input.local), endpoint_from(input.remote)};
}

[[nodiscard]] core::Error native_error(core::DependencyOperation operation, int code,
                                       const DependencyLogSink& sink) noexcept {
  const auto error = normalize_dependency_error(
      core::DependencyId::ngtcp2, operation, status_for(code), code);
  log_dependency_error(sink, error);
  return error;
}

[[nodiscard]] core::Result<void> require_random(State& state) noexcept {
  if (!state.random_failed) return {};
  state.random_failed = false;
  const auto error = normalize_dependency_error(core::DependencyId::ngtcp2,
      core::DependencyOperation::quic_session, core::DependencyStatus::crypto,
      NGTCP2_ERR_CALLBACK_FAILURE);
  log_dependency_error(state.log, error);
  return std::unexpected{error};
}

void random_fill(std::uint8_t* destination, std::size_t size,
                 const ngtcp2_rand_ctx* context) {
  auto& state = *static_cast<State*>(context->native_handle);
  const auto output = *core::MutableByteView::from(
      std::span<std::byte>{reinterpret_cast<std::byte*>(destination), size});
  if (state.crypto.random_fill == nullptr ||
      !state.crypto.random_fill(state.crypto.context, output)) {
    state.random_failed = true;
    std::memset(destination, 0, size);
  }
}

[[nodiscard]] QuicEncryptionLevel encryption_level(ngtcp2_encryption_level level) noexcept {
  switch (level) {
    case NGTCP2_ENCRYPTION_LEVEL_INITIAL: return QuicEncryptionLevel::initial;
    case NGTCP2_ENCRYPTION_LEVEL_0RTT: return QuicEncryptionLevel::early_data;
    case NGTCP2_ENCRYPTION_LEVEL_HANDSHAKE: return QuicEncryptionLevel::handshake;
    case NGTCP2_ENCRYPTION_LEVEL_1RTT: return QuicEncryptionLevel::application;
  }
  return QuicEncryptionLevel::initial;
}

[[nodiscard]] ngtcp2_encryption_level native_level(QuicEncryptionLevel level) noexcept {
  switch (level) {
    case QuicEncryptionLevel::initial: return NGTCP2_ENCRYPTION_LEVEL_INITIAL;
    case QuicEncryptionLevel::early_data: return NGTCP2_ENCRYPTION_LEVEL_0RTT;
    case QuicEncryptionLevel::handshake: return NGTCP2_ENCRYPTION_LEVEL_HANDSHAKE;
    case QuicEncryptionLevel::application: return NGTCP2_ENCRYPTION_LEVEL_1RTT;
  }
  return NGTCP2_ENCRYPTION_LEVEL_INITIAL;
}

[[nodiscard]] bool valid_key(const QuicPacketKey& key, bool require_secret) noexcept {
  return key.aead != nullptr && key.aead_context != nullptr && key.header != nullptr &&
         key.header_context != nullptr && key.aead->maximum_overhead != 0U &&
         key.aead->encrypt != nullptr && key.aead->decrypt != nullptr &&
         key.header->protect != nullptr && !key.iv.empty() &&
         (!require_secret || !key.secret.empty());
}

[[nodiscard]] ngtcp2_crypto_ctx crypto_context(const QuicPacketKey& key) noexcept {
  ngtcp2_crypto_ctx result{};
  result.aead = {const_cast<QuicAeadAlgorithm*>(key.aead), key.aead->maximum_overhead};
  result.hp = {const_cast<QuicHeaderAlgorithm*>(key.header)};
  result.max_encryption = std::numeric_limits<std::uint64_t>::max();
  result.max_decryption_failure = std::numeric_limits<std::uint64_t>::max();
  return result;
}

[[nodiscard]] ngtcp2_crypto_aead_ctx aead_context(QuicKeyContext& value) noexcept {
  return {&value};
}
[[nodiscard]] ngtcp2_crypto_cipher_ctx header_context(QuicKeyContext& value) noexcept {
  return {&value};
}

core::Result<void> install_initial(void* context, const QuicPacketKey& receive,
                                   const QuicPacketKey& transmit) noexcept {
  auto& action = *static_cast<ActionContext*>(context);
  if (!valid_key(receive, false) || !valid_key(transmit, false) ||
      receive.iv.size() != transmit.iv.size() || receive.aead != transmit.aead ||
      receive.header != transmit.header) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "QUIC initial keys are incomplete or inconsistent")};
  }
  const ngtcp2_crypto_ctx native_crypto = crypto_context(receive);
  ngtcp2_conn_set_initial_crypto_ctx(action.connection, &native_crypto);
  const auto rx_aead = aead_context(*receive.aead_context);
  const auto rx_hp = header_context(*receive.header_context);
  const auto tx_aead = aead_context(*transmit.aead_context);
  const auto tx_hp = header_context(*transmit.header_context);
  const int result = action.negotiated_version == 0U
      ? ngtcp2_conn_install_initial_key(action.connection, &rx_aead,
          reinterpret_cast<const std::uint8_t*>(receive.iv.data()), &rx_hp, &tx_aead,
          reinterpret_cast<const std::uint8_t*>(transmit.iv.data()), &tx_hp,
          receive.iv.size())
      : ngtcp2_conn_install_vneg_initial_key(action.connection, action.negotiated_version,
          &rx_aead, reinterpret_cast<const std::uint8_t*>(receive.iv.data()), &rx_hp,
          &tx_aead, reinterpret_cast<const std::uint8_t*>(transmit.iv.data()), &tx_hp,
          receive.iv.size());
  if (result != 0) return std::unexpected{native_error(
      core::DependencyOperation::quic_session, result, action.state->log)};
  return {};
}

core::Result<void> install_handshake(void* context, QuicKeyDirection direction,
                                     const QuicPacketKey& key) noexcept {
  auto& action = *static_cast<ActionContext*>(context);
  if (!valid_key(key, false)) return std::unexpected{core_error(
      core::ErrorCode::invalid_input, "QUIC handshake key is incomplete")};
  const ngtcp2_crypto_ctx native_crypto = crypto_context(key);
  ngtcp2_conn_set_crypto_ctx(action.connection, &native_crypto);
  const auto aead = aead_context(*key.aead_context);
  const auto hp = header_context(*key.header_context);
  const int result = direction == QuicKeyDirection::receive
      ? ngtcp2_conn_install_rx_handshake_key(action.connection, &aead,
          reinterpret_cast<const std::uint8_t*>(key.iv.data()), key.iv.size(), &hp)
      : ngtcp2_conn_install_tx_handshake_key(action.connection, &aead,
          reinterpret_cast<const std::uint8_t*>(key.iv.data()), key.iv.size(), &hp);
  if (result != 0) return std::unexpected{native_error(
      core::DependencyOperation::quic_session, result, action.state->log)};
  return {};
}

core::Result<void> install_application(void* context, QuicKeyDirection direction,
                                       const QuicPacketKey& key) noexcept {
  auto& action = *static_cast<ActionContext*>(context);
  if (!valid_key(key, true)) return std::unexpected{core_error(
      core::ErrorCode::invalid_input, "QUIC application key is incomplete")};
  action.state->application_iv_size = key.iv.size();
  const ngtcp2_crypto_ctx native_crypto = crypto_context(key);
  ngtcp2_conn_set_crypto_ctx(action.connection, &native_crypto);
  const auto aead = aead_context(*key.aead_context);
  const auto hp = header_context(*key.header_context);
  const int result = direction == QuicKeyDirection::receive
      ? ngtcp2_conn_install_rx_key(action.connection,
          reinterpret_cast<const std::uint8_t*>(key.secret.data()), key.secret.size(), &aead,
          reinterpret_cast<const std::uint8_t*>(key.iv.data()), key.iv.size(), &hp)
      : ngtcp2_conn_install_tx_key(action.connection,
          reinterpret_cast<const std::uint8_t*>(key.secret.data()), key.secret.size(), &aead,
          reinterpret_cast<const std::uint8_t*>(key.iv.data()), key.iv.size(), &hp);
  if (result != 0) return std::unexpected{native_error(
      core::DependencyOperation::quic_session, result, action.state->log)};
  return {};
}

core::Result<void> submit_crypto(void* context, QuicEncryptionLevel level,
                                 core::ByteView data) noexcept {
  auto& action = *static_cast<ActionContext*>(context);
  const int result = ngtcp2_conn_submit_crypto_data(action.connection, native_level(level),
      reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
  if (result != 0) return std::unexpected{native_error(
      core::DependencyOperation::quic_send, result, action.state->log)};
  return {};
}

[[nodiscard]] QuicCryptoActions actions(ActionContext& context) noexcept {
  return {&context, install_initial, install_handshake, install_application, submit_crypto};
}

int client_initial(ngtcp2_conn* connection, void* user_data) {
  auto& state = *static_cast<State*>(user_data);
  ActionContext context{connection, &state, 0};
  const auto id = *core::ByteView::from(
      std::span{state.initial_dcid}.first(state.initial_dcid_size));
  return state.crypto.start != nullptr &&
      state.crypto.start(state.crypto.context, QuicRole::client, id, actions(context))
      ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}
int recv_client_initial(ngtcp2_conn* connection, const ngtcp2_cid* dcid,
                        void* user_data) {
  auto& state = *static_cast<State*>(user_data);
  ActionContext context{connection, &state, 0};
  const auto id = *core::ByteView::from(std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(dcid->data), dcid->datalen});
  return state.crypto.start != nullptr &&
      state.crypto.start(state.crypto.context, QuicRole::server, id, actions(context))
      ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}
int recv_crypto(ngtcp2_conn* connection, ngtcp2_encryption_level level, std::uint64_t offset,
                const std::uint8_t* data, std::size_t size, void* user_data) {
  auto& state = *static_cast<State*>(user_data);
  const auto mapped = encryption_level(level);
  const auto view = *core::ByteView::from(
      std::span<const std::byte>{reinterpret_cast<const std::byte*>(data), size});
  ActionContext context{connection, &state, 0};
  if (state.crypto.receive == nullptr ||
      !state.crypto.receive(state.crypto.context, mapped, offset, view, actions(context))) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  if (state.events.write == nullptr) return 0;
  const QuicEvent event{QuicEventKind::handshake_data, mapped, -1, offset,
                        static_cast<std::uint64_t>(size), 0, view, false};
  return state.events.write(state.events.context, event) ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}
int encrypt(std::uint8_t* destination, const ngtcp2_crypto_aead* aead,
            const ngtcp2_crypto_aead_ctx* key, const std::uint8_t* input,
            std::size_t input_size, const std::uint8_t* nonce, std::size_t nonce_size,
            const std::uint8_t* aad, std::size_t aad_size) {
  const auto& algorithm = *static_cast<const QuicAeadAlgorithm*>(aead->native_handle);
  auto& context = *static_cast<QuicKeyContext*>(key->native_handle);
  const auto output = *core::MutableByteView::from(std::span<std::byte>{
      reinterpret_cast<std::byte*>(destination), input_size + aead->max_overhead});
  const auto source = *core::ByteView::from(std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(input), input_size});
  const auto nonce_view = *core::ByteView::from(std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(nonce), nonce_size});
  const auto aad_view = *core::ByteView::from(std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(aad), aad_size});
  return algorithm.encrypt(algorithm.callback_context, context.handle, output, source,
                           nonce_view, aad_view) ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}
int decrypt(std::uint8_t* destination, const ngtcp2_crypto_aead* aead,
            const ngtcp2_crypto_aead_ctx* key, const std::uint8_t* input,
            std::size_t input_size, const std::uint8_t* nonce, std::size_t nonce_size,
            const std::uint8_t* aad, std::size_t aad_size) {
  const auto& algorithm = *static_cast<const QuicAeadAlgorithm*>(aead->native_handle);
  auto& context = *static_cast<QuicKeyContext*>(key->native_handle);
  const std::size_t output_size = input_size >= aead->max_overhead
      ? input_size - aead->max_overhead : 0U;
  const auto output = *core::MutableByteView::from(std::span<std::byte>{
      reinterpret_cast<std::byte*>(destination), output_size});
  const auto source = *core::ByteView::from(std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(input), input_size});
  const auto nonce_view = *core::ByteView::from(std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(nonce), nonce_size});
  const auto aad_view = *core::ByteView::from(std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(aad), aad_size});
  return algorithm.decrypt(algorithm.callback_context, context.handle, output, source,
                           nonce_view, aad_view) ? 0 : NGTCP2_ERR_DECRYPT;
}
int header_mask(std::uint8_t* destination, const ngtcp2_crypto_cipher* cipher,
                const ngtcp2_crypto_cipher_ctx* key, const std::uint8_t* sample) {
  const auto& algorithm = *static_cast<const QuicHeaderAlgorithm*>(cipher->native_handle);
  auto& context = *static_cast<QuicKeyContext*>(key->native_handle);
  const auto output = *core::MutableByteView::from(std::span<std::byte>{
      reinterpret_cast<std::byte*>(destination), NGTCP2_HP_SAMPLELEN});
  const auto sample_view = *core::ByteView::from(std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(sample), NGTCP2_HP_SAMPLELEN});
  return algorithm.protect(algorithm.callback_context, context.handle, output, sample_view)
      ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}
int recv_retry(ngtcp2_conn* connection, const ngtcp2_pkt_hd*, void* user_data) {
  auto& state = *static_cast<State*>(user_data);
  ActionContext context{connection, &state, 0};
  return state.crypto.retry != nullptr &&
      state.crypto.retry(state.crypto.context, actions(context))
      ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}
int update_key(ngtcp2_conn*, std::uint8_t* rx_secret, std::uint8_t* tx_secret,
               ngtcp2_crypto_aead_ctx* rx_aead, std::uint8_t* rx_iv,
               ngtcp2_crypto_aead_ctx* tx_aead, std::uint8_t* tx_iv,
               const std::uint8_t* current_rx, const std::uint8_t* current_tx,
               std::size_t secret_size, void* user_data) {
  auto& state = *static_cast<State*>(user_data);
  if (state.crypto.update == nullptr || state.application_iv_size == 0U) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  const auto mutable_view = [](std::uint8_t* data, std::size_t size) noexcept {
    return *core::MutableByteView::from(std::span<std::byte>{
        reinterpret_cast<std::byte*>(data), size});
  };
  const auto view = [](const std::uint8_t* data, std::size_t size) noexcept {
    return *core::ByteView::from(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(data), size});
  };
  QuicKeyContext* receive{};
  QuicKeyContext* transmit{};
  if (!state.crypto.update(state.crypto.context, mutable_view(rx_secret, secret_size),
          mutable_view(tx_secret, secret_size), mutable_view(rx_iv, state.application_iv_size),
          mutable_view(tx_iv, state.application_iv_size), view(current_rx, secret_size),
          view(current_tx, secret_size), receive, transmit) || receive == nullptr ||
      transmit == nullptr) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  rx_aead->native_handle = receive;
  tx_aead->native_handle = transmit;
  return 0;
}
int version_negotiation(ngtcp2_conn* connection, std::uint32_t version,
                        const ngtcp2_cid* dcid, void* user_data) {
  auto& state = *static_cast<State*>(user_data);
  ActionContext context{connection, &state, version};
  const auto id = *core::ByteView::from(std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(dcid->data), dcid->datalen});
  return state.crypto.start != nullptr &&
      state.crypto.start(state.crypto.context, state.role, id, actions(context))
      ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}
int new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token,
                      std::size_t size, void* user_data) {
  auto& state = *static_cast<State*>(user_data);
  std::array<std::uint8_t, NGTCP2_MAX_CIDLEN> bytes{};
  const auto cid_output = *core::MutableByteView::from(std::span<std::byte>{
      reinterpret_cast<std::byte*>(bytes.data()), size});
  const auto token_output = *core::MutableByteView::from(std::span<std::byte>{
      reinterpret_cast<std::byte*>(token->data), sizeof(token->data)});
  if (!state.crypto.random_fill(state.crypto.context, cid_output) ||
      !state.crypto.random_fill(state.crypto.context, token_output)) {
    state.random_failed = true;
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  const auto value = *core::ByteView::from(std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(bytes.data()), size});
  const auto owned = QuicConnectionId::create(value, state.worker, state.generation);
  if (!owned.has_value() || state.connection_ids.write == nullptr ||
      !state.connection_ids.write(state.connection_ids.context, *owned)) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  ngtcp2_cid_init(cid, bytes.data(), size);
  return 0;
}
int path_challenge(ngtcp2_conn*, ngtcp2_path_challenge_data* data, void* user_data) {
  auto& state = *static_cast<State*>(user_data);
  const auto output = *core::MutableByteView::from(std::span<std::byte>{
      reinterpret_cast<std::byte*>(data->data), sizeof(data->data)});
  if (!state.crypto.random_fill(state.crypto.context, output)) {
    state.random_failed = true;
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}

[[nodiscard]] int emit(State& state, const QuicEvent& event) noexcept {
  return state.events.write == nullptr || state.events.write(state.events.context, event)
      ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}
int handshake_completed(ngtcp2_conn*, void* user_data) {
  return emit(*static_cast<State*>(user_data),
              {QuicEventKind::handshake_completed});
}
int stream_opened(ngtcp2_conn*, std::int64_t stream, void* user_data) {
  return emit(*static_cast<State*>(user_data),
              {QuicEventKind::stream_opened, QuicEncryptionLevel::application, stream});
}
int stream_data(ngtcp2_conn*, std::uint32_t flags, std::int64_t stream,
                std::uint64_t offset, const std::uint8_t* data, std::size_t size,
                void* user_data, void*) {
  const auto view = *core::ByteView::from(std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(data), size});
  return emit(*static_cast<State*>(user_data),
      {QuicEventKind::stream_data, QuicEncryptionLevel::application, stream, offset,
       static_cast<std::uint64_t>(size), 0, view,
       (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0U});
}
int stream_data_acked(ngtcp2_conn*, std::int64_t stream, std::uint64_t offset,
                      std::uint64_t size, void* user_data, void*) {
  return emit(*static_cast<State*>(user_data),
      {QuicEventKind::stream_data_acked, QuicEncryptionLevel::application, stream,
       offset, size});
}
int stream_reset(ngtcp2_conn*, std::int64_t stream, std::uint64_t,
                 std::uint64_t code, void* user_data, void*) {
  return emit(*static_cast<State*>(user_data),
      {QuicEventKind::stream_reset, QuicEncryptionLevel::application, stream, 0, 0, code});
}
int stop_sending_received(ngtcp2_conn*, std::int64_t stream, std::uint64_t code,
                          void* user_data, void*) {
  return emit(*static_cast<State*>(user_data),
      {QuicEventKind::stop_sending, QuicEncryptionLevel::application, stream, 0, 0, code});
}
int stream_closed(ngtcp2_conn*, std::uint32_t, std::int64_t stream,
                  std::uint64_t receive_code, std::uint64_t transmit_code,
                  void* user_data, void*) {
  const std::uint64_t code = receive_code != 0U ? receive_code : transmit_code;
  return emit(*static_cast<State*>(user_data),
      {QuicEventKind::stream_closed, QuicEncryptionLevel::application, stream, 0, 0, code});
}
void delete_aead(ngtcp2_conn*, ngtcp2_crypto_aead_ctx* native, void*) {
  auto& context = *static_cast<QuicKeyContext*>(native->native_handle);
  if (context.destroy != nullptr) context.destroy(context.destroy_context, context.handle);
}
void delete_cipher(ngtcp2_conn*, ngtcp2_crypto_cipher_ctx* native, void*) {
  auto& context = *static_cast<QuicKeyContext*>(native->native_handle);
  if (context.destroy != nullptr) context.destroy(context.destroy_context, context.handle);
}

[[nodiscard]] ngtcp2_cid native_cid(const QuicConnectionId& value) noexcept {
  ngtcp2_cid result{};
  ngtcp2_cid_init(&result, reinterpret_cast<const std::uint8_t*>(value.value().data()),
                  value.value().size());
  return result;
}

}  // namespace

core::Result<QuicConnectionId> QuicConnectionId::create(
    core::ByteView value, core::WorkerId worker, core::GenerationId generation) noexcept {
  if (value.empty() || value.size() > capacity) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "QUIC connection ID must contain 1 to 20 bytes")};
  }
  std::array<std::byte, capacity> bytes{};
  std::copy(value.span().begin(), value.span().end(), bytes.begin());
  return QuicConnectionId{bytes, static_cast<std::uint8_t>(value.size()), worker, generation};
}

QuicSession::QuicSession(QuicSession&& other) noexcept { move_from(std::move(other)); }
QuicSession& QuicSession::operator=(QuicSession&& other) noexcept {
  if (this != &other) { release(); move_from(std::move(other)); }
  return *this;
}
QuicSession::~QuicSession() { release(); }

core::Result<QuicSession> QuicSession::create(
    QuicRole role, const QuicConnectionId& destination, const QuicConnectionId& source,
    NativeMemoryPool& memory, QuicPath initial_path, QuicLimits limits,
    QuicCryptoCallbacks crypto, QuicEventSink events,
    QuicConnectionIdSink connection_ids, DependencyLogSink log_sink) noexcept {
  if ((role != QuicRole::client && role != QuicRole::server) ||
      destination.worker().value() != source.worker().value() ||
      destination.generation().value() != source.generation().value() ||
      limits.maximum_packet_bytes < NGTCP2_MAX_UDP_PAYLOAD_SIZE ||
      limits.maximum_packet_bytes > NGTCP2_MAX_TX_UDP_PAYLOAD_SIZE ||
      crypto.random_fill == nullptr || crypto.start == nullptr ||
      crypto.receive == nullptr || crypto.retry == nullptr || crypto.update == nullptr ||
      connection_ids.write == nullptr || !valid_endpoint(initial_path.local) ||
      !valid_endpoint(initial_path.remote)) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "QUIC role, CID ownership, limits, or crypto callbacks are invalid")};
  }
  internal::ArenaMemoryAccess::synchronize(memory);
  auto& arena = internal::ArenaMemoryAccess::arena(memory);
  if (internal::ArenaMemoryAccess::worker(memory).value() != source.worker().value()) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "QUIC memory pool belongs to a different worker")};
  }
  auto pin = arena.pin(source.worker());
  if (!pin.has_value()) return std::unexpected{pin.error()};
  internal::ArenaMemory allocator{&memory};
  void* const storage = internal::arena_malloc(sizeof(State), &allocator);
  if (storage == nullptr) return std::unexpected{core_error(
      core::ErrorCode::exhaustion, "QUIC session storage is exhausted")};
  auto* const state = ::new (storage) State{{&memory}, {}, {}, crypto, events,
      connection_ids, log_sink, role, source.worker(), source.generation(), {},
      destination.value().size(), 0,
      limits.maximum_packet_bytes, false, false, false};
  std::copy(destination.value().span().begin(), destination.value().span().end(),
            state->initial_dcid.begin());
  if (!store_path(initial_path, state->path)) {
    state->~State();
    internal::arena_free(storage, &allocator);
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "QUIC initial path is invalid")};
  }

  ngtcp2_callbacks callbacks{};
  callbacks.client_initial = client_initial;
  callbacks.recv_client_initial = recv_client_initial;
  callbacks.recv_crypto_data = recv_crypto;
  callbacks.handshake_completed = handshake_completed;
  callbacks.encrypt = encrypt;
  callbacks.decrypt = decrypt;
  callbacks.hp_mask = header_mask;
  callbacks.recv_stream_data = stream_data;
  callbacks.acked_stream_data_offset = stream_data_acked;
  callbacks.stream_open = stream_opened;
  callbacks.stream_reset = stream_reset;
  callbacks.recv_stop_sending = stop_sending_received;
  callbacks.stream_close2 = stream_closed;
  callbacks.rand = random_fill;
  callbacks.recv_retry = recv_retry;
  callbacks.update_key = update_key;
  callbacks.version_negotiation = version_negotiation;
  callbacks.get_new_connection_id2 = new_connection_id;
  callbacks.get_path_challenge_data2 = path_challenge;
  callbacks.delete_crypto_aead_ctx = delete_aead;
  callbacks.delete_crypto_cipher_ctx = delete_cipher;

  ngtcp2_settings settings{};
  ngtcp2_settings_default(&settings);
  settings.initial_ts = 0;
  settings.rand_ctx.native_handle = state;
  settings.max_tx_udp_payload_size = limits.maximum_packet_bytes;
  ngtcp2_transport_params params{};
  ngtcp2_transport_params_default(&params);
  params.initial_max_data = limits.initial_max_data;
  params.initial_max_stream_data_bidi_local = limits.initial_max_stream_data;
  params.initial_max_stream_data_bidi_remote = limits.initial_max_stream_data;
  params.initial_max_stream_data_uni = limits.initial_max_stream_data;
  params.initial_max_streams_bidi = limits.initial_max_streams_bidi;
  params.initial_max_streams_uni = limits.initial_max_streams_uni;

  const ngtcp2_cid dcid = native_cid(destination);
  const ngtcp2_cid scid = native_cid(source);
  if (role == QuicRole::server) {
    params.original_dcid = dcid;
    params.original_dcid_present = 1;
  }
  state->native_memory = {&state->memory, internal::arena_malloc, internal::arena_free,
                          internal::arena_calloc, internal::arena_realloc};
  ngtcp2_conn* connection{};
  const int result = role == QuicRole::client
      ? ngtcp2_conn_client_new(&connection, &dcid, &scid, &state->path.path, NGTCP2_PROTO_VER_V1,
                              &callbacks, &settings, &params, &state->native_memory, state)
      : ngtcp2_conn_server_new(&connection, &dcid, &scid, &state->path.path, NGTCP2_PROTO_VER_V1,
                              &callbacks, &settings, &params, &state->native_memory, state);
  if (result != 0) {
    state->~State();
    internal::arena_free(storage, &allocator);
    return std::unexpected{native_error(core::DependencyOperation::quic_session,
                                        result, log_sink)};
  }
  return QuicSession{connection, state, arena, arena.generation(), std::move(*pin)};
}

core::Result<void> QuicSession::require_valid() const noexcept {
  if (connection_ == nullptr || state_ == nullptr || arena_ == nullptr ||
      arena_->generation() != generation_ || static_cast<State*>(state_)->terminal) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "QUIC session is inactive or its arena was reset")};
  }
  return {};
}

core::Result<void> QuicSession::receive_packet(QuicPath path, core::ByteView packet,
                                                std::uint64_t now_ns) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return valid;
  auto& state = *static_cast<State*>(state_);
  ngtcp2_path_storage input{};
  if (!store_path(path, input)) return std::unexpected{core_error(
      core::ErrorCode::invalid_input, "QUIC packet path is invalid")};
  const int result = ngtcp2_conn_read_pkt(static_cast<ngtcp2_conn*>(connection_), &input.path, nullptr,
      reinterpret_cast<const std::uint8_t*>(packet.data()), packet.size(), now_ns);
  if (result < 0 && terminal_error(result)) state.terminal = true;
  if (const auto random = require_random(state); !random.has_value()) return random;
  if (result != 0) return std::unexpected{native_error(core::DependencyOperation::quic_receive,
      result, static_cast<State*>(state_)->log)};
  return {};
}

core::Result<QuicPacketWrite> QuicSession::write_packet(
    core::MutableByteView output, std::int64_t stream_id, core::ByteView stream_data,
    bool fin, std::uint64_t now_ns) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return std::unexpected{valid.error()};
  const auto required = static_cast<State*>(state_)->maximum_packet_bytes;
  if (output.size() < required) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "QUIC output is smaller than the configured packet buffer")};
  }
  ngtcp2_ssize consumed{-1};
  const ngtcp2_vec vector{reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(stream_data.data())),
                           stream_data.size()};
  const std::uint32_t flags = fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : NGTCP2_WRITE_STREAM_FLAG_NONE;
  ngtcp2_path_storage output_path{};
  ngtcp2_path_storage_zero(&output_path);
  const auto result = ngtcp2_conn_writev_stream(static_cast<ngtcp2_conn*>(connection_),
      &output_path.path,
      nullptr, reinterpret_cast<std::uint8_t*>(output.data()), output.size(), &consumed,
      flags, stream_id, stream_data.empty() ? nullptr : &vector,
      stream_data.empty() ? 0U : 1U, now_ns);
  auto& state = *static_cast<State*>(state_);
  if (result < 0 && terminal_error(static_cast<int>(result))) state.terminal = true;
  if (const auto random = require_random(state); !random.has_value()) {
    return std::unexpected{random.error()};
  }
  if (result < 0) return std::unexpected{native_error(core::DependencyOperation::quic_send,
      static_cast<int>(result), static_cast<State*>(state_)->log)};
  if (result > 0) state.packet_pending_transmit = true;
  return QuicPacketWrite{static_cast<std::size_t>(result),
      consumed < 0 ? 0U : static_cast<std::size_t>(consumed),
      result > 0 ? path_from(output_path.path) : QuicPath{}};
}

core::Result<void> QuicSession::packet_transmitted(std::uint64_t now_ns) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return valid;
  auto& state = *static_cast<State*>(state_);
  if (!state.packet_pending_transmit) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "QUIC has no generated packet awaiting transmission")};
  }
  ngtcp2_conn_update_pkt_tx_time(static_cast<ngtcp2_conn*>(connection_), now_ns);
  state.packet_pending_transmit = false;
  return {};
}

core::Result<std::int64_t> QuicSession::open_bidirectional_stream() noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return std::unexpected{valid.error()};
  std::int64_t stream{};
  const int result = ngtcp2_conn_open_bidi_stream(static_cast<ngtcp2_conn*>(connection_), &stream,
                                                   nullptr);
  if (const auto random = require_random(*static_cast<State*>(state_)); !random.has_value()) {
    return std::unexpected{random.error()};
  }
  if (result != 0) return std::unexpected{native_error(core::DependencyOperation::quic_stream,
      result, static_cast<State*>(state_)->log)};
  return stream;
}

core::Result<std::int64_t> QuicSession::open_unidirectional_stream() noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return std::unexpected{valid.error()};
  std::int64_t stream{};
  const int result = ngtcp2_conn_open_uni_stream(static_cast<ngtcp2_conn*>(connection_), &stream,
                                                  nullptr);
  if (const auto random = require_random(*static_cast<State*>(state_)); !random.has_value()) {
    return std::unexpected{random.error()};
  }
  if (result != 0) return std::unexpected{native_error(core::DependencyOperation::quic_stream,
      result, static_cast<State*>(state_)->log)};
  return stream;
}

core::Result<void> QuicSession::reset_stream(std::int64_t stream, std::uint64_t code) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return valid;
  const int result = ngtcp2_conn_shutdown_stream_write(static_cast<ngtcp2_conn*>(connection_),
                                                        0, stream, code);
  if (result != 0) return std::unexpected{native_error(core::DependencyOperation::quic_stream,
      result, static_cast<State*>(state_)->log)};
  return {};
}

core::Result<void> QuicSession::stop_sending(std::int64_t stream, std::uint64_t code) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return valid;
  const int result = ngtcp2_conn_shutdown_stream_read(static_cast<ngtcp2_conn*>(connection_),
                                                       0, stream, code);
  if (result != 0) return std::unexpected{native_error(core::DependencyOperation::quic_stream,
      result, static_cast<State*>(state_)->log)};
  return {};
}

std::uint64_t QuicSession::expiry_ns() const noexcept {
  return connection_ == nullptr || arena_ == nullptr || arena_->generation() != generation_
      ? 0 : ngtcp2_conn_get_expiry2(static_cast<const ngtcp2_conn*>(connection_));
}
core::Result<void> QuicSession::handle_expiry(std::uint64_t now_ns) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return valid;
  const int result = ngtcp2_conn_handle_expiry(static_cast<ngtcp2_conn*>(connection_), now_ns);
  auto& state = *static_cast<State*>(state_);
  if (result == NGTCP2_ERR_IDLE_CLOSE || (result < 0 && terminal_error(result))) {
    state.terminal = true;
  }
  if (const auto random = require_random(state); !random.has_value()) {
    return random;
  }
  if (result != 0) return std::unexpected{native_error(core::DependencyOperation::quic_expiry,
      result, static_cast<State*>(state_)->log)};
  return {};
}
void QuicSession::release() noexcept {
  if (connection_ != nullptr && arena_ != nullptr && arena_->generation() == generation_) {
    ngtcp2_conn_del(static_cast<ngtcp2_conn*>(connection_));
    auto* const state = static_cast<State*>(state_);
    internal::ArenaMemory allocator{state->memory.pool};
    state->~State();
    internal::arena_free(state, &allocator);
  }
  connection_ = nullptr; state_ = nullptr; arena_ = nullptr; generation_ = 0;
  pin_ = {};
}
void QuicSession::move_from(QuicSession&& other) noexcept {
  connection_ = std::exchange(other.connection_, nullptr);
  state_ = std::exchange(other.state_, nullptr);
  arena_ = std::exchange(other.arena_, nullptr);
  generation_ = std::exchange(other.generation_, 0);
  pin_ = std::move(other.pin_);
}

}  // namespace laghu::adapters
