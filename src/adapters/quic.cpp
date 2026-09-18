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
  sockaddr_in local{};
  sockaddr_in remote{};
  ngtcp2_path path{};
  QuicCryptoCallbacks crypto{};
  QuicEventSink events{};
  DependencyLogSink log{};
};

[[nodiscard]] core::Error core_error(core::ErrorCode code, const char* text) noexcept {
  return {core::ErrorDomain::core, code, 0, text};
}

[[nodiscard]] core::DependencyStatus status_for(int code) noexcept {
  if (code == NGTCP2_ERR_NOMEM) return core::DependencyStatus::exhaustion;
  if (code == NGTCP2_ERR_INVALID_ARGUMENT) return core::DependencyStatus::invalid_input;
  if (code == NGTCP2_ERR_CRYPTO) return core::DependencyStatus::crypto;
  return core::DependencyStatus::corrupt_data;
}

[[nodiscard]] core::Error native_error(core::DependencyOperation operation, int code,
                                       const DependencyLogSink& sink) noexcept {
  const auto error = normalize_dependency_error(
      core::DependencyId::ngtcp2, operation, status_for(code), code);
  log_dependency_error(sink, error);
  return error;
}

void random_fill(std::uint8_t* destination, std::size_t size,
                 const ngtcp2_rand_ctx* context) {
  auto& state = *static_cast<State*>(context->native_handle);
  const auto output = *core::MutableByteView::from(
      std::span<std::byte>{reinterpret_cast<std::byte*>(destination), size});
  if (state.crypto.random_fill == nullptr ||
      !state.crypto.random_fill(state.crypto.context, output)) {
    std::memset(destination, 0, size);
  }
}

int client_initial(ngtcp2_conn*, void*) { return NGTCP2_ERR_CALLBACK_FAILURE; }
int recv_client_initial(ngtcp2_conn*, const ngtcp2_cid*, void*) {
  return NGTCP2_ERR_CALLBACK_FAILURE;
}
int recv_crypto(ngtcp2_conn*, ngtcp2_encryption_level level, std::uint64_t offset,
                const std::uint8_t* data, std::size_t size, void* user_data) {
  auto& state = *static_cast<State*>(user_data);
  if (state.events.write == nullptr) return 0;
  const auto mapped = static_cast<QuicEncryptionLevel>(level);
  const auto view = *core::ByteView::from(
      std::span<const std::byte>{reinterpret_cast<const std::byte*>(data), size});
  const QuicEvent event{QuicEventKind::handshake_data, mapped, -1, offset, 0, view, false};
  return state.events.write(state.events.context, event) ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}
int unavailable_encrypt(std::uint8_t*, const ngtcp2_crypto_aead*,
                        const ngtcp2_crypto_aead_ctx*, const std::uint8_t*, std::size_t,
                        const std::uint8_t*, std::size_t, const std::uint8_t*, std::size_t) {
  return NGTCP2_ERR_CALLBACK_FAILURE;
}
int unavailable_decrypt(std::uint8_t*, const ngtcp2_crypto_aead*,
                        const ngtcp2_crypto_aead_ctx*, const std::uint8_t*, std::size_t,
                        const std::uint8_t*, std::size_t, const std::uint8_t*, std::size_t) {
  return NGTCP2_ERR_CALLBACK_FAILURE;
}
int unavailable_mask(std::uint8_t*, const ngtcp2_crypto_cipher*,
                     const ngtcp2_crypto_cipher_ctx*, const std::uint8_t*) {
  return NGTCP2_ERR_CALLBACK_FAILURE;
}
int recv_retry(ngtcp2_conn*, const ngtcp2_pkt_hd*, void*) {
  return NGTCP2_ERR_CALLBACK_FAILURE;
}
int update_key(ngtcp2_conn*, std::uint8_t*, std::uint8_t*, ngtcp2_crypto_aead_ctx*,
               std::uint8_t*, ngtcp2_crypto_aead_ctx*, std::uint8_t*,
               const std::uint8_t*, const std::uint8_t*, std::size_t, void*) {
  return NGTCP2_ERR_CALLBACK_FAILURE;
}
int version_negotiation(ngtcp2_conn*, std::uint32_t, const ngtcp2_cid*, void*) {
  return NGTCP2_ERR_CALLBACK_FAILURE;
}
int new_connection_id(ngtcp2_conn*, ngtcp2_cid*, ngtcp2_stateless_reset_token*,
                      std::size_t, void*) {
  return NGTCP2_ERR_CALLBACK_FAILURE;
}
int path_challenge(ngtcp2_conn*, ngtcp2_path_challenge_data*, void*) {
  return NGTCP2_ERR_CALLBACK_FAILURE;
}
void delete_aead(ngtcp2_conn*, ngtcp2_crypto_aead_ctx*, void*) {}
void delete_cipher(ngtcp2_conn*, ngtcp2_crypto_cipher_ctx*, void*) {}

[[nodiscard]] ngtcp2_cid native_cid(const QuicConnectionId& value) noexcept {
  ngtcp2_cid result{};
  ngtcp2_cid_init(&result, reinterpret_cast<const std::uint8_t*>(value.bytes.data()), value.size);
  return result;
}

}  // namespace

core::Result<QuicConnectionId> QuicConnectionId::create(
    core::ByteView value, core::WorkerId worker, core::GenerationId generation) noexcept {
  if (value.empty() || value.size() > capacity) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "QUIC connection ID must contain 1 to 20 bytes")};
  }
  QuicConnectionId result{{}, static_cast<std::uint8_t>(value.size()), worker, generation};
  std::copy(value.span().begin(), value.span().end(), result.bytes.begin());
  return result;
}

QuicSession::QuicSession(QuicSession&& other) noexcept { move_from(std::move(other)); }
QuicSession& QuicSession::operator=(QuicSession&& other) noexcept {
  if (this != &other) { release(); move_from(std::move(other)); }
  return *this;
}
QuicSession::~QuicSession() { release(); }

core::Result<QuicSession> QuicSession::create(
    QuicRole role, const QuicConnectionId& destination, const QuicConnectionId& source,
    core::BoundedArena& arena, QuicLimits limits, QuicCryptoCallbacks crypto,
    QuicEventSink events, DependencyLogSink log_sink) noexcept {
  if ((role != QuicRole::client && role != QuicRole::server) ||
      destination.worker.value() != source.worker.value() ||
      destination.generation.value() != source.generation.value() ||
      limits.maximum_packet_bytes < NGTCP2_MAX_UDP_PAYLOAD_SIZE ||
      crypto.random_fill == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "QUIC role, CID ownership, limits, or crypto callbacks are invalid")};
  }
  const auto storage = arena.try_allocate(source.worker, sizeof(State), alignof(State));
  if (!storage.has_value()) return std::unexpected{storage.error()};
  const auto bytes = storage->bytes();
  if (!bytes.has_value()) return std::unexpected{bytes.error()};
  auto* const state = ::new (bytes->data()) State{{&arena, source.worker, nullptr}, {}, {}, {}, {},
                                                  crypto, events, log_sink};
  state->local.sin_family = AF_INET;
  state->remote.sin_family = AF_INET;
  state->path = {
      {reinterpret_cast<sockaddr*>(&state->local), sizeof(state->local)},
      {reinterpret_cast<sockaddr*>(&state->remote), sizeof(state->remote)}, nullptr};

  ngtcp2_callbacks callbacks{};
  callbacks.client_initial = client_initial;
  callbacks.recv_client_initial = recv_client_initial;
  callbacks.recv_crypto_data = recv_crypto;
  callbacks.encrypt = unavailable_encrypt;
  callbacks.decrypt = unavailable_decrypt;
  callbacks.hp_mask = unavailable_mask;
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
      ? ngtcp2_conn_client_new(&connection, &dcid, &scid, &state->path, NGTCP2_PROTO_VER_V1,
                              &callbacks, &settings, &params, &state->native_memory, state)
      : ngtcp2_conn_server_new(&connection, &dcid, &scid, &state->path, NGTCP2_PROTO_VER_V1,
                              &callbacks, &settings, &params, &state->native_memory, state);
  if (result != 0) {
    return std::unexpected{native_error(core::DependencyOperation::quic_session,
                                        result, log_sink)};
  }
  return QuicSession{connection, state, arena, arena.generation()};
}

core::Result<void> QuicSession::require_valid() const noexcept {
  if (connection_ == nullptr || state_ == nullptr || arena_ == nullptr ||
      arena_->generation() != generation_) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "QUIC session is inactive or its arena was reset")};
  }
  return {};
}

core::Result<void> QuicSession::receive_packet(core::ByteView packet,
                                                std::uint64_t now_ns) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return valid;
  auto& state = *static_cast<State*>(state_);
  const int result = ngtcp2_conn_read_pkt(static_cast<ngtcp2_conn*>(connection_), &state.path, nullptr,
      reinterpret_cast<const std::uint8_t*>(packet.data()), packet.size(), now_ns);
  if (result != 0) return std::unexpected{native_error(core::DependencyOperation::quic_receive,
      result, static_cast<State*>(state_)->log)};
  return {};
}

core::Result<QuicPacketWrite> QuicSession::write_packet(
    core::MutableByteView output, std::int64_t stream_id, core::ByteView stream_data,
    bool fin, std::uint64_t now_ns) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return std::unexpected{valid.error()};
  if (output.size() < NGTCP2_MAX_UDP_PAYLOAD_SIZE) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "QUIC output is smaller than the minimum packet buffer")};
  }
  ngtcp2_ssize consumed{-1};
  const ngtcp2_vec vector{reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(stream_data.data())),
                           stream_data.size()};
  const std::uint32_t flags = fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : NGTCP2_WRITE_STREAM_FLAG_NONE;
  const auto result = ngtcp2_conn_writev_stream(static_cast<ngtcp2_conn*>(connection_), nullptr,
      nullptr, reinterpret_cast<std::uint8_t*>(output.data()), output.size(), &consumed,
      flags, stream_id, stream_data.empty() ? nullptr : &vector,
      stream_data.empty() ? 0U : 1U, now_ns);
  if (result < 0) return std::unexpected{native_error(core::DependencyOperation::quic_send,
      static_cast<int>(result), static_cast<State*>(state_)->log)};
  return QuicPacketWrite{static_cast<std::size_t>(result),
      consumed < 0 ? 0U : static_cast<std::size_t>(consumed)};
}

core::Result<std::int64_t> QuicSession::open_bidirectional_stream() noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return std::unexpected{valid.error()};
  std::int64_t stream{};
  const int result = ngtcp2_conn_open_bidi_stream(static_cast<ngtcp2_conn*>(connection_), &stream,
                                                   nullptr);
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
  return connection_ == nullptr ? 0 : ngtcp2_conn_get_expiry2(static_cast<const ngtcp2_conn*>(connection_));
}
core::Result<void> QuicSession::handle_expiry(std::uint64_t now_ns) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return valid;
  const int result = ngtcp2_conn_handle_expiry(static_cast<ngtcp2_conn*>(connection_), now_ns);
  if (result != 0) return std::unexpected{native_error(core::DependencyOperation::quic_expiry,
      result, static_cast<State*>(state_)->log)};
  return {};
}
void QuicSession::release() noexcept {
  if (connection_ != nullptr) ngtcp2_conn_del(static_cast<ngtcp2_conn*>(connection_));
  connection_ = nullptr; state_ = nullptr; arena_ = nullptr; generation_ = 0;
}
void QuicSession::move_from(QuicSession&& other) noexcept {
  connection_ = std::exchange(other.connection_, nullptr);
  state_ = std::exchange(other.state_, nullptr);
  arena_ = std::exchange(other.arena_, nullptr);
  generation_ = std::exchange(other.generation_, 0);
}

}  // namespace laghu::adapters
