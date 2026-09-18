// SPDX-License-Identifier: AGPL-3.0-only
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <utility>
#include <array>

#include <nghttp3/nghttp3.h>

#include <laghu/adapters/http3.hpp>
#include <laghu/adapters/internal/arena_memory.hpp>

namespace laghu::adapters {
namespace {

struct State final {
  internal::ArenaMemory memory;
  nghttp3_mem native_memory{};
  Http3EventSink events{};
  DependencyLogSink log{};
};

[[nodiscard]] core::Error core_error(core::ErrorCode code, const char* text) noexcept {
  return {core::ErrorDomain::core, code, 0, text};
}
[[nodiscard]] core::DependencyStatus status_for(int code) noexcept {
  if (code == NGHTTP3_ERR_NOMEM) return core::DependencyStatus::exhaustion;
  if (code == NGHTTP3_ERR_INVALID_ARGUMENT) return core::DependencyStatus::invalid_input;
  return core::DependencyStatus::corrupt_data;
}
[[nodiscard]] core::Error native_error(core::DependencyOperation operation, int code,
                                       const DependencyLogSink& sink) noexcept {
  const auto error = normalize_dependency_error(core::DependencyId::nghttp3, operation,
                                                 status_for(code), code);
  log_dependency_error(sink, error);
  return error;
}
[[nodiscard]] int emit(State& state, const Http3Event& event) noexcept {
  return state.events.write == nullptr || state.events.write(state.events.context, event)
      ? 0 : NGHTTP3_ERR_CALLBACK_FAILURE;
}
int begin_headers(nghttp3_conn*, std::int64_t stream, void* user, void*) {
  return emit(*static_cast<State*>(user), {Http3EventKind::headers_begin, stream});
}
int recv_header(nghttp3_conn*, std::int64_t stream, std::int32_t,
                nghttp3_rcbuf* name, nghttp3_rcbuf* value, std::uint8_t,
                void* user, void*) {
  const nghttp3_vec native_name = nghttp3_rcbuf_get_buf(name);
  const nghttp3_vec native_value = nghttp3_rcbuf_get_buf(value);
  const auto name_view = *core::ByteView::from(std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(native_name.base), native_name.len});
  const auto value_view = *core::ByteView::from(std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(native_value.base), native_value.len});
  return emit(*static_cast<State*>(user), {Http3EventKind::header, stream,
      name_view, value_view});
}
int end_headers(nghttp3_conn*, std::int64_t stream, int, void* user, void*) {
  return emit(*static_cast<State*>(user), {Http3EventKind::headers_end, stream});
}
int recv_data(nghttp3_conn*, std::int64_t stream, const std::uint8_t* data,
              std::size_t size, void* user, void*) {
  const auto view = *core::ByteView::from(std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(data), size});
  return emit(*static_cast<State*>(user), {Http3EventKind::data, stream, {}, {}, view});
}
int end_stream(nghttp3_conn*, std::int64_t stream, void* user, void*) {
  return emit(*static_cast<State*>(user), {Http3EventKind::stream_end, stream});
}
int stop_sending(nghttp3_conn*, std::int64_t stream, std::uint64_t code, void* user, void*) {
  return emit(*static_cast<State*>(user), {Http3EventKind::stop_sending, stream, {}, {}, {}, code});
}
int reset_stream(nghttp3_conn*, std::int64_t stream, std::uint64_t code, void* user, void*) {
  return emit(*static_cast<State*>(user), {Http3EventKind::reset, stream, {}, {}, {}, code});
}
int shutdown_cb(nghttp3_conn*, std::int64_t id, void* user) {
  return emit(*static_cast<State*>(user), {Http3EventKind::shutdown, id});
}

struct NativeHeaders final {
  static constexpr std::size_t capacity = 64;
  std::array<nghttp3_nv, capacity> values{};
  std::size_t size{};
};

[[nodiscard]] core::Result<NativeHeaders> native_headers(
    std::span<const Http3Header> headers) noexcept {
  if (headers.empty() || headers.size() > NativeHeaders::capacity) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "HTTP/3 headers must contain 1 to 64 fields")};
  }
  NativeHeaders result{};
  for (const auto& header : headers) {
    if (header.name.empty()) {
      return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                        "HTTP/3 header name must not be empty")};
    }
    result.values[result.size] = {
                      const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(header.name.data())),
                      const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(header.value.data())),
                      header.name.size(), header.value.size(), NGHTTP3_NV_FLAG_NONE};
    ++result.size;
  }
  return result;
}

}  // namespace

Http3Session::Http3Session(Http3Session&& other) noexcept { move_from(std::move(other)); }
Http3Session& Http3Session::operator=(Http3Session&& other) noexcept {
  if (this != &other) { release(); move_from(std::move(other)); }
  return *this;
}
Http3Session::~Http3Session() { release(); }

core::Result<Http3Session> Http3Session::create(
    Http3Role role, core::WorkerId worker, core::BoundedArena& arena, Http3Limits limits,
    Http3EventSink events, DependencyLogSink log_sink) noexcept {
  if ((role != Http3Role::client && role != Http3Role::server) ||
      limits.maximum_field_section_bytes == 0) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "HTTP/3 role or field-section limit is invalid")};
  }
  const auto storage = arena.try_allocate(worker, sizeof(State), alignof(State));
  if (!storage.has_value()) return std::unexpected{storage.error()};
  const auto bytes = storage->bytes();
  if (!bytes.has_value()) return std::unexpected{bytes.error()};
  auto* const state = ::new (bytes->data()) State{{&arena, worker, nullptr}, {}, events, log_sink};
  nghttp3_callbacks callbacks{};
  callbacks.begin_headers = begin_headers;
  callbacks.recv_header = recv_header;
  callbacks.end_headers = end_headers;
  callbacks.recv_data = recv_data;
  callbacks.end_stream = end_stream;
  callbacks.stop_sending = stop_sending;
  callbacks.reset_stream = reset_stream;
  callbacks.shutdown = shutdown_cb;
  nghttp3_settings settings{};
  nghttp3_settings_default(&settings);
  settings.max_field_section_size = limits.maximum_field_section_bytes;
  settings.qpack_max_dtable_capacity = limits.qpack_table_capacity;
  settings.qpack_encoder_max_dtable_capacity = limits.qpack_table_capacity;
  settings.qpack_blocked_streams = limits.qpack_blocked_streams;
  state->native_memory = {&state->memory, internal::arena_malloc, internal::arena_free,
                          internal::arena_calloc, internal::arena_realloc};
  nghttp3_conn* connection{};
  const int result = role == Http3Role::client
      ? nghttp3_conn_client_new(&connection, &callbacks, &settings, &state->native_memory, state)
      : nghttp3_conn_server_new(&connection, &callbacks, &settings, &state->native_memory, state);
  if (result != 0) return std::unexpected{native_error(core::DependencyOperation::http3_session,
                                                       result, log_sink)};
  return Http3Session{connection, state, arena, arena.generation()};
}

core::Result<void> Http3Session::require_valid() const noexcept {
  if (connection_ == nullptr || state_ == nullptr || arena_ == nullptr ||
      arena_->generation() != generation_) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "HTTP/3 session is inactive or its arena was reset")};
  }
  return {};
}
core::Result<void> Http3Session::bind_streams(std::int64_t control,
                                              std::int64_t encoder,
                                              std::int64_t decoder) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return valid;
  auto* const connection = static_cast<nghttp3_conn*>(connection_);
  int result = nghttp3_conn_bind_control_stream(connection, control);
  if (result == 0) result = nghttp3_conn_bind_qpack_streams(connection, encoder, decoder);
  if (result != 0) return std::unexpected{native_error(core::DependencyOperation::http3_session,
      result, static_cast<State*>(state_)->log)};
  return {};
}
core::Result<std::size_t> Http3Session::receive(std::int64_t stream, core::ByteView data,
                                                bool fin) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return std::unexpected{valid.error()};
  const auto result = nghttp3_conn_read_stream2(static_cast<nghttp3_conn*>(connection_), stream,
      reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), fin ? 1 : 0, 0);
  if (result < 0) return std::unexpected{native_error(core::DependencyOperation::http3_receive,
      static_cast<int>(result), static_cast<State*>(state_)->log)};
  return static_cast<std::size_t>(result);
}
core::Result<Http3Output> Http3Session::next_output() noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return std::unexpected{valid.error()};
  std::int64_t stream{-1}; int fin{}; nghttp3_vec vector{};
  const auto count = nghttp3_conn_writev_stream(static_cast<nghttp3_conn*>(connection_),
                                                 &stream, &fin, &vector, 1);
  if (count < 0) return std::unexpected{native_error(core::DependencyOperation::http3_send,
      static_cast<int>(count), static_cast<State*>(state_)->log)};
  if (count == 0) return Http3Output{};
  const auto view = *core::ByteView::from(std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(vector.base), vector.len});
  return Http3Output{stream, view, fin != 0};
}
core::Result<void> Http3Session::acknowledge_output(std::int64_t stream,
                                                    std::size_t bytes) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return valid;
  const int result = nghttp3_conn_add_write_offset(static_cast<nghttp3_conn*>(connection_), stream, bytes);
  if (result != 0) return std::unexpected{native_error(core::DependencyOperation::http3_send,
      result, static_cast<State*>(state_)->log)};
  return {};
}
core::Result<void> Http3Session::submit_request(std::int64_t stream,
                                                std::span<const Http3Header> headers) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return valid;
  const auto native = native_headers(headers);
  if (!native.has_value()) return std::unexpected{native.error()};
  const int result = nghttp3_conn_submit_request(static_cast<nghttp3_conn*>(connection_), stream,
                                                  native->values.data(), native->size, nullptr, nullptr);
  if (result != 0) return std::unexpected{native_error(core::DependencyOperation::http3_submit,
      result, static_cast<State*>(state_)->log)};
  return {};
}
core::Result<void> Http3Session::submit_response(std::int64_t stream,
                                                 std::span<const Http3Header> headers) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return valid;
  const auto native = native_headers(headers);
  if (!native.has_value()) return std::unexpected{native.error()};
  const int result = nghttp3_conn_submit_response(static_cast<nghttp3_conn*>(connection_), stream,
                                                   native->values.data(), native->size, nullptr);
  if (result != 0) return std::unexpected{native_error(core::DependencyOperation::http3_submit,
      result, static_cast<State*>(state_)->log)};
  return {};
}
core::Result<void> Http3Session::close_stream(std::int64_t stream, std::uint64_t code) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return valid;
  const int result = nghttp3_conn_close_stream2(static_cast<nghttp3_conn*>(connection_),
      NGHTTP3_STREAM_CLOSE_FLAG_RX_APP_ERROR_CODE_SET |
          NGHTTP3_STREAM_CLOSE_FLAG_TX_APP_ERROR_CODE_SET,
      stream, code, code);
  if (result != 0) return std::unexpected{native_error(core::DependencyOperation::http3_submit,
      result, static_cast<State*>(state_)->log)};
  return {};
}
core::Result<void> Http3Session::shutdown() noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) return valid;
  const int result = nghttp3_conn_shutdown(static_cast<nghttp3_conn*>(connection_));
  if (result != 0) return std::unexpected{native_error(core::DependencyOperation::http3_submit,
      result, static_cast<State*>(state_)->log)};
  return {};
}
void Http3Session::release() noexcept {
  if (connection_ != nullptr) nghttp3_conn_del(static_cast<nghttp3_conn*>(connection_));
  connection_ = nullptr; state_ = nullptr; arena_ = nullptr; generation_ = 0;
}
void Http3Session::move_from(Http3Session&& other) noexcept {
  connection_ = std::exchange(other.connection_, nullptr);
  state_ = std::exchange(other.state_, nullptr);
  arena_ = std::exchange(other.arena_, nullptr);
  generation_ = std::exchange(other.generation_, 0);
}

}  // namespace laghu::adapters
