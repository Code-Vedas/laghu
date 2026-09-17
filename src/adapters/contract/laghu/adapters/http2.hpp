// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include <laghu/adapters/dependency.hpp>
#include <laghu/core/bounded_arena.hpp>
#include <laghu/core/views.hpp>

namespace laghu::adapters {

enum class Http2Role : std::uint8_t { client, server };

class Http2StreamId final {
 public:
  constexpr Http2StreamId() noexcept = default;

  [[nodiscard]] static constexpr core::Result<Http2StreamId> from_wire(
      std::int32_t value) noexcept {
    if (value <= 0) {
      return std::unexpected{core::Error{core::ErrorDomain::core,
                                         core::ErrorCode::invalid_input, 0,
                                         "HTTP/2 stream ID must be positive"}};
    }
    return Http2StreamId{value};
  }

  [[nodiscard]] constexpr bool valid() const noexcept { return value_ > 0; }
  [[nodiscard]] constexpr std::int32_t wire_value() const noexcept { return value_; }

 private:
  friend class Http2Session;
  explicit constexpr Http2StreamId(std::int32_t value) noexcept : value_(value) {}
  std::int32_t value_{};
};

struct Http2Limits final {
  static constexpr std::size_t maximum_submission_headers = 64;
  static constexpr std::size_t maximum_settings = 16;

  std::size_t maximum_header_block_bytes{};
  std::size_t maximum_header_fields{};
  std::size_t maximum_debug_data_bytes{};
};

struct Http2Header final {
  core::ByteView name{};
  core::ByteView value{};
  bool sensitive{};
};

struct Http2Setting final {
  std::uint16_t identifier{};
  std::uint32_t value{};
};

enum class Http2EventKind : std::uint8_t {
  headers_begin,
  header,
  data,
  frame_received,
  settings,
  reset,
  goaway,
  window_update,
  stream_closed,
};

struct Http2Event final {
  Http2EventKind kind{Http2EventKind::frame_received};
  Http2StreamId stream{};
  core::ByteView name{};
  core::ByteView value{};
  core::ByteView data{};
  std::uint32_t code{};
  std::uint32_t setting_value{};
  std::int32_t last_stream_id{};
  std::int32_t window_increment{};
  core::ByteView debug_data{};
  bool end_stream{};
  bool sensitive{};
  bool settings_ack{};
};

enum class Http2CallbackAction : std::uint8_t {
  continue_processing,
  pause,
  reject_stream,
  fail_session,
};

using Http2EventWrite = Http2CallbackAction (*)(void*, const Http2Event&) noexcept;

struct Http2EventSink final {
  // The caller owns both fields and must keep them valid for the session.
  void* context{};
  Http2EventWrite write{};

  [[nodiscard]] constexpr bool enabled() const noexcept { return write != nullptr; }
};

struct Http2IoInterest final {
  bool read{};
  bool write{};
  // nghttp2 owns no clock or timer. The caller may combine this false value
  // with its own stream/session deadlines.
  bool timer{};
};

class Http2Session final {
 public:
  Http2Session(const Http2Session&) = delete;
  Http2Session& operator=(const Http2Session&) = delete;
  Http2Session(Http2Session&& other) noexcept;
  Http2Session& operator=(Http2Session&& other) noexcept;
  ~Http2Session();

  [[nodiscard]] static core::Result<Http2Session> create(
      core::WorkerId worker, Http2Role role, core::BoundedArena& arena,
      Http2Limits limits, Http2EventSink event_sink = {},
      DependencyLogSink log_sink = {}) noexcept;

  // A callback may pause parsing. When fewer than input.size() bytes are
  // consumed, retain input and pass its unconsumed suffix to receive() again.
  [[nodiscard]] core::Result<std::size_t> receive(core::ByteView input) noexcept;
  // Returned bytes remain valid until the next operation on this session.
  [[nodiscard]] core::Result<core::ByteView> next_output() noexcept;
  [[nodiscard]] core::Result<Http2IoInterest> interest() const noexcept;

  [[nodiscard]] core::Result<Http2StreamId> submit_headers(
      std::span<const Http2Header> headers, bool end_stream) noexcept;
  [[nodiscard]] core::Result<void> submit_headers(
      Http2StreamId stream, std::span<const Http2Header> headers,
      bool end_stream) noexcept;
  [[nodiscard]] core::Result<void> submit_settings(
      std::span<const Http2Setting> settings) noexcept;
  [[nodiscard]] core::Result<void> submit_reset(Http2StreamId stream,
                                                std::uint32_t error_code) noexcept;
  [[nodiscard]] core::Result<void> submit_goaway(std::int32_t last_stream_id,
                                                 std::uint32_t error_code,
                                                 core::ByteView debug_data) noexcept;
  [[nodiscard]] core::Result<void> submit_connection_window_update(
      std::int32_t increment) noexcept;
  [[nodiscard]] core::Result<void> submit_stream_window_update(
      Http2StreamId stream, std::int32_t increment) noexcept;

 private:
  constexpr Http2Session(void* native_session, void* native_state,
                         const core::BoundedArena& arena,
                         std::uint64_t arena_generation) noexcept
      : native_session_(native_session), native_state_(native_state), arena_(&arena),
        arena_generation_(arena_generation) {}

  [[nodiscard]] core::Result<void> require_valid() const noexcept;
  [[nodiscard]] core::Result<void> submit_headers_impl(
      std::int32_t stream_id, std::span<const Http2Header> headers,
      bool end_stream, Http2StreamId* assigned) noexcept;
  void release() noexcept;
  void move_from(Http2Session&& other) noexcept;

  void* native_session_{};
  void* native_state_{};
  const core::BoundedArena* arena_{};
  std::uint64_t arena_generation_{};
};

static_assert(std::is_trivially_copyable_v<Http2StreamId>);
static_assert(std::is_trivially_copyable_v<Http2Limits>);
static_assert(std::is_trivially_copyable_v<Http2Header>);
static_assert(std::is_trivially_copyable_v<Http2Event>);
static_assert(std::is_trivially_copyable_v<Http2EventSink>);
static_assert(!std::is_copy_constructible_v<Http2Session>);

}  // namespace laghu::adapters
