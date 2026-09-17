// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <sys/types.h>
#include <utility>

#include <nghttp2/nghttp2.h>

#include <laghu/adapters/http2.hpp>

namespace laghu::adapters {
namespace {

struct alignas(std::max_align_t) AllocationHeader final {
  std::size_t size{};
};

struct NativeState final {
  core::BoundedArena* arena{};
  core::WorkerId worker;
  Http2Limits limits{};
  Http2EventSink event_sink{};
  DependencyLogSink log_sink{};
  std::size_t header_bytes{};
  std::size_t header_fields{};
  bool input_limit_failed{};
};

[[nodiscard]] core::DependencyStatus native_status(int code) noexcept {
  if (code == NGHTTP2_ERR_NOMEM) {
    return core::DependencyStatus::exhaustion;
  }
  if (code == NGHTTP2_ERR_INVALID_ARGUMENT || code == NGHTTP2_ERR_INVALID_HEADER_BLOCK) {
    return core::DependencyStatus::invalid_input;
  }
  if (code == NGHTTP2_ERR_STREAM_ID_NOT_AVAILABLE) {
    return core::DependencyStatus::exhaustion;
  }
  return core::DependencyStatus::corrupt_data;
}

[[nodiscard]] core::Error native_error(core::DependencyOperation operation, int code,
                                       const DependencyLogSink& sink) noexcept {
  const core::Error error = normalize_dependency_error(
      core::DependencyId::nghttp2, operation, native_status(code), code);
  log_dependency_error(sink, error);
  return error;
}

[[nodiscard]] core::Error core_error(core::ErrorCode code,
                                     const char* diagnostic) noexcept {
  return {core::ErrorDomain::core, code, 0, diagnostic};
}

[[nodiscard]] void* arena_allocate(NativeState& state, std::size_t size) noexcept {
  const std::size_t payload_size = size == 0U ? 1U : size;
  if (payload_size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader)) {
    return nullptr;
  }
  const auto allocation = state.arena->try_allocate(
      state.worker, sizeof(AllocationHeader) + payload_size, alignof(std::max_align_t));
  if (!allocation.has_value()) {
    return nullptr;
  }
  const auto bytes = allocation->bytes();
  if (!bytes.has_value()) {
    return nullptr;
  }
  auto* const header = ::new (bytes->data()) AllocationHeader{payload_size};
  return static_cast<void*>(header + 1);
}

void* memory_malloc(std::size_t size, void* context) noexcept {
  return arena_allocate(*static_cast<NativeState*>(context), size);
}

void memory_free(void*, void*) noexcept {}

void* memory_calloc(std::size_t count, std::size_t size, void* context) noexcept {
  auto& state = *static_cast<NativeState*>(context);
  if (count != 0U && size > std::numeric_limits<std::size_t>::max() / count) {
    return nullptr;
  }
  const std::size_t total = count * size;
  void* const output = arena_allocate(state, total);
  if (output != nullptr) {
    std::memset(output, 0, total);
  }
  return output;
}

void* memory_realloc(void* pointer, std::size_t size, void* context) noexcept {
  auto& state = *static_cast<NativeState*>(context);
  if (pointer == nullptr) {
    return arena_allocate(state, size);
  }
  if (size == 0U) {
    return nullptr;
  }
  auto* const old_header = static_cast<AllocationHeader*>(pointer) - 1;
  void* const output = arena_allocate(state, size);
  if (output == nullptr) {
    return nullptr;
  }
  const std::size_t copied = old_header->size < size ? old_header->size : size;
  std::memcpy(output, pointer, copied);
  return output;
}

[[nodiscard]] Http2StreamId stream_id(std::int32_t value) noexcept {
  const auto converted = Http2StreamId::from_wire(value);
  return converted.has_value() ? *converted : Http2StreamId{};
}

[[nodiscard]] int callback_result(NativeState& state, const Http2Event& event,
                                  bool pausable, bool rejectable) noexcept {
  if (!state.event_sink.enabled()) {
    return 0;
  }
  const Http2CallbackAction action = state.event_sink.write(state.event_sink.context, event);
  if (action == Http2CallbackAction::continue_processing) {
    return 0;
  }
  if (action == Http2CallbackAction::pause && pausable) {
    return NGHTTP2_ERR_PAUSE;
  }
  if (action == Http2CallbackAction::reject_stream && rejectable) {
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  }
  return NGHTTP2_ERR_CALLBACK_FAILURE;
}

int on_begin_headers(nghttp2_session*, const nghttp2_frame* frame,
                     void* user_data) noexcept {
  auto& state = *static_cast<NativeState*>(user_data);
  state.header_bytes = 0;
  state.header_fields = 0;
  Http2Event event{};
  event.kind = Http2EventKind::headers_begin;
  event.stream = stream_id(frame->hd.stream_id);
  event.end_stream = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0U;
  return callback_result(state, event, false, true);
}

int on_header(nghttp2_session*, const nghttp2_frame* frame,
              const std::uint8_t* name, std::size_t name_length,
              const std::uint8_t* value, std::size_t value_length,
              std::uint8_t flags, void* user_data) noexcept {
  auto& state = *static_cast<NativeState*>(user_data);
  if (name_length > std::numeric_limits<std::size_t>::max() - value_length ||
      state.header_bytes > std::numeric_limits<std::size_t>::max() - name_length - value_length) {
    state.input_limit_failed = true;
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  }
  state.header_bytes += name_length + value_length;
  ++state.header_fields;
  if (state.header_bytes > state.limits.maximum_header_block_bytes ||
      state.header_fields > state.limits.maximum_header_fields) {
    state.input_limit_failed = true;
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  }
  const auto name_view = core::ByteView::from(
      {reinterpret_cast<const std::byte*>(name), name_length});
  const auto value_view = core::ByteView::from(
      {reinterpret_cast<const std::byte*>(value), value_length});
  if (!name_view.has_value() || !value_view.has_value()) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }
  Http2Event event{};
  event.kind = Http2EventKind::header;
  event.stream = stream_id(frame->hd.stream_id);
  event.name = *name_view;
  event.value = *value_view;
  event.sensitive = (flags & NGHTTP2_NV_FLAG_NO_INDEX) != 0U;
  return callback_result(state, event, true, true);
}

int on_data(nghttp2_session*, std::uint8_t flags, std::int32_t native_stream_id,
            const std::uint8_t* data, std::size_t length, void* user_data) noexcept {
  auto& state = *static_cast<NativeState*>(user_data);
  const auto data_view = core::ByteView::from(
      {reinterpret_cast<const std::byte*>(data), length});
  if (!data_view.has_value()) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }
  Http2Event event{};
  event.kind = Http2EventKind::data;
  event.stream = stream_id(native_stream_id);
  event.data = *data_view;
  event.end_stream = (flags & NGHTTP2_FLAG_END_STREAM) != 0U;
  return callback_result(state, event, true, false);
}

int on_frame_received(nghttp2_session*, const nghttp2_frame* frame,
                      void* user_data) noexcept {
  auto& state = *static_cast<NativeState*>(user_data);
  Http2Event event{};
  event.kind = Http2EventKind::frame_received;
  event.stream = stream_id(frame->hd.stream_id);
  event.end_stream = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0U;
  switch (frame->hd.type) {
    case NGHTTP2_SETTINGS:
      event.kind = Http2EventKind::settings;
      event.settings_ack = (frame->hd.flags & NGHTTP2_FLAG_ACK) != 0U;
      if (frame->settings.niv == 0U) {
        return callback_result(state, event, false, false);
      }
      for (std::size_t index = 0; index < frame->settings.niv; ++index) {
        event.code = static_cast<std::uint32_t>(frame->settings.iv[index].settings_id);
        event.setting_value = frame->settings.iv[index].value;
        const int result = callback_result(state, event, false, false);
        if (result != 0) {
          return result;
        }
      }
      return 0;
    case NGHTTP2_RST_STREAM:
      event.kind = Http2EventKind::reset;
      event.code = frame->rst_stream.error_code;
      break;
    case NGHTTP2_GOAWAY:
      if (frame->goaway.opaque_data_len > state.limits.maximum_debug_data_bytes) {
        state.input_limit_failed = true;
        return NGHTTP2_ERR_CALLBACK_FAILURE;
      }
      event.kind = Http2EventKind::goaway;
      event.code = frame->goaway.error_code;
      event.last_stream_id = frame->goaway.last_stream_id;
      event.debug_data = *core::ByteView::from(
          {reinterpret_cast<const std::byte*>(frame->goaway.opaque_data),
           frame->goaway.opaque_data_len});
      break;
    case NGHTTP2_WINDOW_UPDATE:
      event.kind = Http2EventKind::window_update;
      event.window_increment = frame->window_update.window_size_increment;
      break;
    default:
      break;
  }
  return callback_result(state, event, false, false);
}

int on_stream_closed(nghttp2_session*, std::int32_t native_stream_id,
                     std::uint32_t error_code, void* user_data) noexcept {
  auto& state = *static_cast<NativeState*>(user_data);
  Http2Event event{};
  event.kind = Http2EventKind::stream_closed;
  event.stream = stream_id(native_stream_id);
  event.code = error_code;
  return callback_result(state, event, false, false);
}

[[nodiscard]] core::Result<void> validate_limits(const Http2Limits& limits) noexcept {
  if (limits.maximum_header_block_bytes == 0U || limits.maximum_header_fields == 0U ||
      limits.maximum_header_fields > Http2Limits::maximum_submission_headers) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "HTTP/2 limits must be nonzero and bounded")};
  }
  return {};
}

}  // namespace

Http2Session::Http2Session(Http2Session&& other) noexcept { move_from(std::move(other)); }

Http2Session& Http2Session::operator=(Http2Session&& other) noexcept {
  if (this != &other) {
    release();
    move_from(std::move(other));
  }
  return *this;
}

Http2Session::~Http2Session() { release(); }

core::Result<Http2Session> Http2Session::create(
    core::WorkerId worker, Http2Role role, core::BoundedArena& arena,
    Http2Limits limits, Http2EventSink event_sink,
    DependencyLogSink log_sink) noexcept {
  if (const auto valid = validate_limits(limits); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  const auto state_storage = arena.try_allocate(worker, sizeof(NativeState), alignof(NativeState));
  if (!state_storage.has_value()) {
    return std::unexpected{state_storage.error()};
  }
  const auto state_bytes = state_storage->bytes();
  if (!state_bytes.has_value()) {
    return std::unexpected{state_bytes.error()};
  }
  auto* const state = ::new (state_bytes->data()) NativeState{
      &arena, worker, limits, event_sink, log_sink, 0, 0, false};

  nghttp2_session_callbacks* callbacks{};
  int result = nghttp2_session_callbacks_new(&callbacks);
  if (result != 0) {
    return std::unexpected{native_error(core::DependencyOperation::http2_session,
                                        result, log_sink)};
  }
  nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers);
  nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_received);
  nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_closed);

  nghttp2_mem memory{state, memory_malloc, memory_free, memory_calloc, memory_realloc};
  nghttp2_session* native_session{};
  if (role == Http2Role::client) {
    result = nghttp2_session_client_new3(&native_session, callbacks, state, nullptr, &memory);
  } else {
    result = nghttp2_session_server_new3(&native_session, callbacks, state, nullptr, &memory);
  }
  nghttp2_session_callbacks_del(callbacks);
  if (result != 0) {
    return std::unexpected{native_error(core::DependencyOperation::http2_session,
                                        result, log_sink)};
  }
  return Http2Session{native_session, state, arena, arena.generation()};
}

core::Result<void> Http2Session::require_valid() const noexcept {
  if (native_session_ == nullptr || native_state_ == nullptr || arena_ == nullptr ||
      arena_->generation() != arena_generation_) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "HTTP/2 session is inactive or its arena was reset")};
  }
  return {};
}

core::Result<std::size_t> Http2Session::receive(core::ByteView input) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  const auto* const bytes = reinterpret_cast<const std::uint8_t*>(input.data());
  const ssize_t result = nghttp2_session_mem_recv(
      static_cast<nghttp2_session*>(native_session_), bytes, input.size());
  auto& state = *static_cast<NativeState*>(native_state_);
  if (state.input_limit_failed) {
    state.input_limit_failed = false;
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "HTTP/2 peer input exceeds its configured bound")};
  }
  if (result < 0) {
    return std::unexpected{native_error(core::DependencyOperation::http2_receive,
                                        static_cast<int>(result), state.log_sink)};
  }
  return static_cast<std::size_t>(result);
}

core::Result<core::ByteView> Http2Session::next_output() noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  const std::uint8_t* data{};
  const ssize_t result = nghttp2_session_mem_send(
      static_cast<nghttp2_session*>(native_session_), &data);
  if (result < 0) {
    auto& state = *static_cast<NativeState*>(native_state_);
    return std::unexpected{native_error(core::DependencyOperation::http2_send,
                                        static_cast<int>(result), state.log_sink)};
  }
  return core::ByteView::from({reinterpret_cast<const std::byte*>(data),
                               static_cast<std::size_t>(result)});
}

core::Result<Http2IoInterest> Http2Session::interest() const noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  auto* const session = static_cast<nghttp2_session*>(native_session_);
  return Http2IoInterest{nghttp2_session_want_read(session) != 0,
                         nghttp2_session_want_write(session) != 0, false};
}

core::Result<void> Http2Session::submit_headers_impl(
    std::int32_t native_stream_id, std::span<const Http2Header> headers,
    bool end_stream, Http2StreamId* assigned) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  auto& state = *static_cast<NativeState*>(native_state_);
  if (headers.empty() || headers.size() > state.limits.maximum_header_fields ||
      headers.size() > Http2Limits::maximum_submission_headers) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "HTTP/2 header submission exceeds its bound")};
  }
  std::array<nghttp2_nv, Http2Limits::maximum_submission_headers> native_headers{};
  std::size_t total{};
  for (std::size_t index = 0; index < headers.size(); ++index) {
    const Http2Header& header = headers[index];
    if (header.name.empty() || header.name.size() > state.limits.maximum_header_block_bytes - total ||
        header.value.size() > state.limits.maximum_header_block_bytes - total - header.name.size()) {
      return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                        "HTTP/2 header block exceeds its bound")};
    }
    total += header.name.size() + header.value.size();
    native_headers[index] = nghttp2_nv{
        const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(header.name.data())),
        const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(header.value.data())),
        header.name.size(), header.value.size(),
        static_cast<std::uint8_t>(header.sensitive ? NGHTTP2_NV_FLAG_NO_INDEX
                                                   : NGHTTP2_NV_FLAG_NONE)};
  }
  const std::uint8_t flags = static_cast<std::uint8_t>(
      NGHTTP2_FLAG_END_HEADERS | (end_stream ? NGHTTP2_FLAG_END_STREAM : 0));
  const std::int32_t result = nghttp2_submit_headers(
      static_cast<nghttp2_session*>(native_session_), flags, native_stream_id,
      nullptr, native_headers.data(), headers.size(), nullptr);
  if (result < 0) {
    return std::unexpected{native_error(core::DependencyOperation::http2_submit,
                                        result, state.log_sink)};
  }
  if (assigned != nullptr) {
    const auto converted = Http2StreamId::from_wire(result);
    if (!converted.has_value()) {
      return std::unexpected{converted.error()};
    }
    *assigned = *converted;
  }
  return {};
}

core::Result<Http2StreamId> Http2Session::submit_headers(
    std::span<const Http2Header> headers, bool end_stream) noexcept {
  Http2StreamId assigned{};
  if (const auto result = submit_headers_impl(-1, headers, end_stream, &assigned);
      !result.has_value()) {
    return std::unexpected{result.error()};
  }
  return assigned;
}

core::Result<void> Http2Session::submit_headers(
    Http2StreamId stream, std::span<const Http2Header> headers,
    bool end_stream) noexcept {
  if (!stream.valid()) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "HTTP/2 stream ID is invalid")};
  }
  return submit_headers_impl(stream.wire_value(), headers, end_stream, nullptr);
}

core::Result<void> Http2Session::submit_settings(
    std::span<const Http2Setting> settings) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  if (settings.size() > Http2Limits::maximum_settings) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "HTTP/2 settings exceed their bound")};
  }
  std::array<nghttp2_settings_entry, Http2Limits::maximum_settings> entries{};
  for (std::size_t index = 0; index < settings.size(); ++index) {
    entries[index] = {static_cast<nghttp2_settings_id>(settings[index].identifier),
                      settings[index].value};
  }
  auto& state = *static_cast<NativeState*>(native_state_);
  const int result = nghttp2_submit_settings(static_cast<nghttp2_session*>(native_session_),
                                              NGHTTP2_FLAG_NONE, entries.data(), settings.size());
  if (result != 0) {
    return std::unexpected{native_error(core::DependencyOperation::http2_submit,
                                        result, state.log_sink)};
  }
  return {};
}

core::Result<void> Http2Session::submit_reset(Http2StreamId stream,
                                               std::uint32_t error_code) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  if (!stream.valid()) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "HTTP/2 stream ID is invalid")};
  }
  auto& state = *static_cast<NativeState*>(native_state_);
  const int result = nghttp2_submit_rst_stream(static_cast<nghttp2_session*>(native_session_),
                                               NGHTTP2_FLAG_NONE, stream.wire_value(), error_code);
  if (result != 0) {
    return std::unexpected{native_error(core::DependencyOperation::http2_submit,
                                        result, state.log_sink)};
  }
  return {};
}

core::Result<void> Http2Session::submit_goaway(std::int32_t last_stream_id,
                                                std::uint32_t error_code,
                                                core::ByteView debug_data) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  auto& state = *static_cast<NativeState*>(native_state_);
  if (last_stream_id < 0 || debug_data.size() > state.limits.maximum_debug_data_bytes) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "HTTP/2 GOAWAY arguments exceed their bounds")};
  }
  const int result = nghttp2_submit_goaway(
      static_cast<nghttp2_session*>(native_session_), NGHTTP2_FLAG_NONE,
      last_stream_id, error_code,
      reinterpret_cast<const std::uint8_t*>(debug_data.data()), debug_data.size());
  if (result != 0) {
    return std::unexpected{native_error(core::DependencyOperation::http2_submit,
                                        result, state.log_sink)};
  }
  return {};
}

core::Result<void> Http2Session::submit_connection_window_update(
    std::int32_t increment) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  auto& state = *static_cast<NativeState*>(native_state_);
  const int result = nghttp2_submit_window_update(
      static_cast<nghttp2_session*>(native_session_), NGHTTP2_FLAG_NONE, 0, increment);
  if (result != 0) {
    return std::unexpected{native_error(core::DependencyOperation::http2_submit,
                                        result, state.log_sink)};
  }
  return {};
}

core::Result<void> Http2Session::submit_stream_window_update(
    Http2StreamId stream, std::int32_t increment) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  if (!stream.valid()) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "HTTP/2 stream ID is invalid")};
  }
  auto& state = *static_cast<NativeState*>(native_state_);
  const int result = nghttp2_submit_window_update(
      static_cast<nghttp2_session*>(native_session_), NGHTTP2_FLAG_NONE,
      stream.wire_value(), increment);
  if (result != 0) {
    return std::unexpected{native_error(core::DependencyOperation::http2_submit,
                                        result, state.log_sink)};
  }
  return {};
}

void Http2Session::release() noexcept {
  if (native_session_ != nullptr && arena_ != nullptr &&
      arena_->generation() == arena_generation_) {
    nghttp2_session_del(static_cast<nghttp2_session*>(native_session_));
  }
  native_session_ = nullptr;
  native_state_ = nullptr;
  arena_ = nullptr;
  arena_generation_ = 0;
}

void Http2Session::move_from(Http2Session&& other) noexcept {
  native_session_ = std::exchange(other.native_session_, nullptr);
  native_state_ = std::exchange(other.native_state_, nullptr);
  arena_ = std::exchange(other.arena_, nullptr);
  arena_generation_ = std::exchange(other.arena_generation_, 0);
}

}  // namespace laghu::adapters
