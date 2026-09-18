// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

#include <laghu/adapters/dependency.hpp>
#include <laghu/core/bounded_arena.hpp>
#include <laghu/core/views.hpp>

namespace laghu::adapters {

enum class Http3Role : std::uint8_t { client, server };
enum class Http3EventKind : std::uint8_t {
  headers_begin, header, headers_end, data, stream_end, reset, stop_sending,
  shutdown, qpack_failure,
};

struct Http3Header final { core::ByteView name{}; core::ByteView value{}; };
struct Http3Event final {
  // All views are valid only for the duration of the sink callback.
  Http3EventKind kind{Http3EventKind::data};
  std::int64_t stream_id{-1};
  core::ByteView name{};
  core::ByteView value{};
  core::ByteView data{};
  std::uint64_t application_error{};
};
using Http3EventWrite = bool (*)(void*, const Http3Event&) noexcept;
struct Http3EventSink final { void* context{}; Http3EventWrite write{}; };

struct Http3Limits final {
  std::uint64_t maximum_field_section_bytes{};
  std::uint64_t qpack_table_capacity{};
  std::uint64_t qpack_blocked_streams{};
};

struct Http3Output final {
  // The byte view remains valid only until the next operation on this session.
  std::int64_t stream_id{-1};
  core::ByteView bytes{};
  bool fin{};
};

class Http3Session final {
 public:
  Http3Session(const Http3Session&) = delete;
  Http3Session& operator=(const Http3Session&) = delete;
  Http3Session(Http3Session&& other) noexcept;
  Http3Session& operator=(Http3Session&& other) noexcept;
  ~Http3Session();

  // The arena object must outlive the session. The arena cannot be reset while
  // this session holds its pin.
  [[nodiscard]] static core::Result<Http3Session> create(
      Http3Role role, core::WorkerId worker, core::BoundedArena& arena,
      Http3Limits limits, Http3EventSink events = {},
      DependencyLogSink log_sink = {}) noexcept;
  [[nodiscard]] core::Result<void> bind_streams(std::int64_t control,
                                                std::int64_t qpack_encoder,
                                                std::int64_t qpack_decoder) noexcept;
  [[nodiscard]] core::Result<std::size_t> receive(std::int64_t stream_id,
                                                  core::ByteView data,
                                                  bool fin) noexcept;
  [[nodiscard]] core::Result<Http3Output> next_output() noexcept;
  [[nodiscard]] core::Result<void> mark_output_written(std::int64_t stream_id,
                                                       std::size_t bytes) noexcept;
  [[nodiscard]] core::Result<void> acknowledge_stream_data(std::int64_t stream_id,
                                                           std::uint64_t bytes) noexcept;
  [[nodiscard]] core::Result<void> submit_request(
      std::int64_t stream_id, std::span<const Http3Header> headers) noexcept;
  [[nodiscard]] core::Result<void> submit_response(
      std::int64_t stream_id, std::span<const Http3Header> headers) noexcept;
  [[nodiscard]] core::Result<void> close_stream(std::int64_t stream_id,
                                                std::uint64_t code) noexcept;
  [[nodiscard]] core::Result<void> shutdown() noexcept;

 private:
  Http3Session(void* connection, void* state, const core::BoundedArena& arena,
               std::uint64_t generation, core::ArenaPin pin) noexcept
      : connection_(connection), state_(state), arena_(&arena), generation_(generation),
        pin_(std::move(pin)) {}
  [[nodiscard]] core::Result<void> require_valid() const noexcept;
  void release() noexcept;
  void move_from(Http3Session&& other) noexcept;
  void* connection_{};
  void* state_{};
  const core::BoundedArena* arena_{};
  std::uint64_t generation_{};
  core::ArenaPin pin_{};
};

static_assert(std::is_trivially_copyable_v<Http3Header>);
static_assert(!std::is_copy_constructible_v<Http3Session>);

}  // namespace laghu::adapters
