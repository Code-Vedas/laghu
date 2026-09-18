// SPDX-License-Identifier: AGPL-3.0-only
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "laghu_test_support.hpp"

#include <laghu/adapters/http2.hpp>
#include <laghu/core/memory_budget.hpp>

namespace {

using laghu::adapters::Http2CallbackAction;
using laghu::adapters::Http2Event;
using laghu::adapters::Http2EventKind;
using laghu::adapters::Http2EventSink;
using laghu::adapters::Http2Header;
using laghu::adapters::Http2Limits;
using laghu::adapters::Http2Role;
using laghu::adapters::Http2Session;
using laghu::adapters::Http2Setting;
using laghu::core::ArenaBlockSource;
using laghu::core::BoundedArena;
using laghu::core::ByteView;
using laghu::core::ErrorCode;
using laghu::core::MemoryBudget;
using laghu::core::MutableByteView;
using laghu::core::Result;
using laghu::core::WorkerId;

struct FixedSource final {
  std::array<std::byte, 131072> storage{};
  bool fail{};

  static Result<MutableByteView> acquire(void* context, std::size_t minimum) noexcept {
    auto& self = *static_cast<FixedSource*>(context);
    if (self.fail || minimum > self.storage.size()) {
      return std::unexpected{laghu::core::Error{
          laghu::core::ErrorDomain::core, ErrorCode::exhaustion, 0,
          "HTTP/2 test block source exhausted"}};
    }
    return MutableByteView::from(self.storage);
  }

  static Result<void> reset(void*) noexcept { return {}; }
};

[[nodiscard]] ArenaBlockSource block_source(FixedSource& source) noexcept {
  return ArenaBlockSource{&source, FixedSource::acquire, FixedSource::reset};
}

[[nodiscard]] ByteView bytes(std::string_view text) noexcept {
  return *ByteView::from({reinterpret_cast<const std::byte*>(text.data()), text.size()});
}

struct Events final {
  std::size_t settings{};
  std::size_t settings_acks{};
  std::size_t headers{};
  std::size_t closed{};
  std::size_t reset{};
  std::size_t goaway{};
  std::size_t window_updates{};
  std::size_t data_events{};
  std::size_t data_bytes{};
  std::size_t ended_frames{};
  std::int32_t last_stream{};
  std::int32_t goaway_last_stream{};
  bool goaway_metadata{};
  bool fail_on_settings{};
  bool pause_on_header{};
  bool paused{};
  bool reject_data{};
  bool data_ended{};

  static Http2CallbackAction write(void* context, const Http2Event& event) noexcept {
    auto& self = *static_cast<Events*>(context);
    if (event.stream.valid()) {
      self.last_stream = event.stream.wire_value();
    }
    switch (event.kind) {
      case Http2EventKind::settings:
        ++self.settings;
        self.settings_acks += event.settings_ack ? 1U : 0U;
        return self.fail_on_settings ? Http2CallbackAction::fail_session
                                     : Http2CallbackAction::continue_processing;
      case Http2EventKind::header:
        ++self.headers;
        if (self.pause_on_header && !self.paused) {
          self.paused = true;
          return Http2CallbackAction::pause;
        }
        break;
      case Http2EventKind::data:
        ++self.data_events;
        self.data_bytes += event.data.size();
        self.data_ended = self.data_ended || event.end_stream;
        return self.reject_data ? Http2CallbackAction::reject_stream
                                : Http2CallbackAction::continue_processing;
      case Http2EventKind::frame_received:
        self.ended_frames += event.end_stream ? 1U : 0U;
        break;
      case Http2EventKind::stream_closed:
        ++self.closed;
        break;
      case Http2EventKind::reset:
        ++self.reset;
        break;
      case Http2EventKind::goaway:
        ++self.goaway;
        self.goaway_last_stream = event.last_stream_id;
        self.goaway_metadata = event.code == 0 && event.debug_data.size() == 4U;
        break;
      case Http2EventKind::window_update:
        ++self.window_updates;
        break;
      default:
        break;
    }
    return Http2CallbackAction::continue_processing;
  }
};

[[nodiscard]] constexpr Http2Limits limits() noexcept {
  return Http2Limits{4096, 32, 128};
}

[[nodiscard]] bool reset_arena(BoundedArena& arena, WorkerId worker) noexcept {
  const auto boundary = arena.quiescent_boundary(worker);
  return boundary.has_value() && arena.reset(worker, *boundary).has_value();
}

[[nodiscard]] bool transfer(Http2Session& source, Http2Session& destination) noexcept {
  for (std::size_t iteration = 0; iteration < 64; ++iteration) {
    const auto output = source.next_output();
    if (!output.has_value()) {
      return false;
    }
    if (output->empty()) {
      return true;
    }
    const auto consumed = destination.receive(*output);
    if (!consumed.has_value() || *consumed != output->size()) {
      return false;
    }
  }
  return false;
}

[[nodiscard]] bool append_data_frame(std::span<std::byte> output, std::size_t& used,
                                     std::int32_t stream_id,
                                     std::span<const std::byte> payload,
                                     bool end_stream) noexcept {
  if (stream_id <= 0 || payload.size() > 0x00ff'ffffU || used > output.size() ||
      payload.size() > output.size() - used || 9U > output.size() - used - payload.size()) {
    return false;
  }
  const auto length = static_cast<std::uint32_t>(payload.size());
  const auto stream = static_cast<std::uint32_t>(stream_id);
  output[used] = static_cast<std::byte>((length >> 16U) & 0xffU);
  output[used + 1U] = static_cast<std::byte>((length >> 8U) & 0xffU);
  output[used + 2U] = static_cast<std::byte>(length & 0xffU);
  output[used + 3U] = std::byte{0};
  output[used + 4U] = end_stream ? std::byte{1} : std::byte{0};
  output[used + 5U] = static_cast<std::byte>((stream >> 24U) & 0x7fU);
  output[used + 6U] = static_cast<std::byte>((stream >> 16U) & 0xffU);
  output[used + 7U] = static_cast<std::byte>((stream >> 8U) & 0xffU);
  output[used + 8U] = static_cast<std::byte>(stream & 0xffU);
  std::copy(payload.begin(), payload.end(),
            output.begin() + static_cast<std::ptrdiff_t>(used + 9U));
  used += 9U + payload.size();
  return true;
}

[[nodiscard]] bool receive_data_frame(Http2Session& session, std::int32_t stream_id,
                                      std::span<const std::byte> payload,
                                      bool end_stream) noexcept {
  std::array<std::byte, 128> frame{};
  std::size_t used{};
  if (!append_data_frame(frame, used, stream_id, payload, end_stream)) {
    return false;
  }
  const auto input = ByteView::from({frame.data(), used});
  if (!input.has_value()) {
    return false;
  }
  const auto consumed = session.receive(*input);
  return consumed.has_value() && *consumed == input->size();
}

[[nodiscard]] bool check_exchange() noexcept {
  const auto worker = WorkerId::from_uint64(60);
  if (!worker.has_value()) {
    return false;
  }
  FixedSource client_source{};
  FixedSource server_source{};
  MemoryBudget client_budget{*worker, client_source.storage.size()};
  MemoryBudget server_budget{*worker, server_source.storage.size()};
  BoundedArena client_arena{*worker, client_budget, block_source(client_source), 4096,
                            client_source.storage.size()};
  BoundedArena server_arena{*worker, server_budget, block_source(server_source), 4096,
                            server_source.storage.size()};
  const bool passed = [&]() noexcept {
    Events client_events{};
    Events server_events{};
    auto client = Http2Session::create(*worker, Http2Role::client, client_arena, limits(),
                                       Http2EventSink{&client_events, Events::write});
    auto server = Http2Session::create(*worker, Http2Role::server, server_arena, limits(),
                                       Http2EventSink{&server_events, Events::write});
    if (!client.has_value() || !server.has_value()) {
      return false;
    }
    constexpr std::array settings{Http2Setting{3, 16}, Http2Setting{4, 65535}};
    if (!client->submit_settings(settings).has_value() ||
        !server->submit_settings(settings).has_value() ||
        !transfer(*client, *server) || !transfer(*server, *client)) {
      return false;
    }
    const std::array request_headers{
        Http2Header{bytes(":method"), bytes("GET"), false},
        Http2Header{bytes(":scheme"), bytes("https"), false},
        Http2Header{bytes(":authority"), bytes("example.test"), false},
        Http2Header{bytes(":path"), bytes("/"), false},
    };
    const auto stream = client->submit_headers(request_headers, true);
    if (!stream.has_value() || !transfer(*client, *server) ||
        server_events.headers != request_headers.size() ||
        server_events.last_stream != stream->wire_value()) {
      return false;
    }
    const std::array response_headers{
        Http2Header{bytes(":status"), bytes("200"), false},
        Http2Header{bytes("content-length"), bytes("0"), false},
    };
    if (!server->submit_headers(*stream, response_headers, true).has_value() ||
        !transfer(*server, *client) || client_events.headers != response_headers.size()) {
      return false;
    }
    const auto interest = client->interest();
    return interest.has_value() && !interest->timer && client_events.settings > 0U &&
           server_events.settings > 0U && client_events.settings_acks > 0U &&
           server_events.settings_acks > 0U && client_events.closed > 0U &&
           server_events.closed > 0U;
  }();
  return reset_arena(client_arena, *worker) && reset_arena(server_arena, *worker) && passed;
}

[[nodiscard]] bool check_data_and_flow_control() noexcept {
  const auto worker = WorkerId::from_uint64(67);
  if (!worker.has_value()) {
    return false;
  }
  FixedSource client_source{};
  FixedSource server_source{};
  MemoryBudget client_budget{*worker, client_source.storage.size()};
  MemoryBudget server_budget{*worker, server_source.storage.size()};
  BoundedArena client_arena{*worker, client_budget, block_source(client_source), 4096,
                            client_source.storage.size()};
  BoundedArena server_arena{*worker, server_budget, block_source(server_source), 4096,
                            server_source.storage.size()};
  const bool passed = [&]() noexcept {
    Events client_events{};
    Events server_events{};
    auto client = Http2Session::create(*worker, Http2Role::client, client_arena, limits(),
                                       Http2EventSink{&client_events, Events::write});
    auto server = Http2Session::create(*worker, Http2Role::server, server_arena, limits(),
                                       Http2EventSink{&server_events, Events::write});
    constexpr std::array settings{Http2Setting{3, 8}};
    if (!client.has_value() || !server.has_value() ||
        !client->submit_settings(settings).has_value() ||
        !server->submit_settings(settings).has_value() || !transfer(*client, *server) ||
        !transfer(*server, *client)) {
      return false;
    }
    const std::array headers{
        Http2Header{bytes(":method"), bytes("POST"), false},
        Http2Header{bytes(":scheme"), bytes("https"), false},
        Http2Header{bytes(":authority"), bytes("example.test"), false},
        Http2Header{bytes(":path"), bytes("/data"), false},
    };
    const auto data_stream = client->submit_headers(headers, false);
    if (!data_stream.has_value() || !transfer(*client, *server) ||
        !receive_data_frame(*server, data_stream->wire_value(), bytes("abc").span(), true) ||
        server_events.data_events != 1U || server_events.data_bytes != 3U ||
        !server_events.data_ended) {
      return false;
    }
    const auto automatic_output = server->next_output();
    if (!automatic_output.has_value() || !automatic_output->empty()) {
      return false;
    }
    if (!server->submit_stream_window_update(*data_stream, 3).has_value() ||
        !transfer(*server, *client) || client_events.window_updates == 0U) {
      return false;
    }

    const auto empty_stream = client->submit_headers(headers, false);
    if (!empty_stream.has_value() || !transfer(*client, *server) ||
        !receive_data_frame(*server, empty_stream->wire_value(), {}, true) ||
        server_events.ended_frames == 0U) {
      return false;
    }

    const auto first_rejected_stream = client->submit_headers(headers, false);
    const auto second_rejected_stream = client->submit_headers(headers, false);
    if (!first_rejected_stream.has_value() || !second_rejected_stream.has_value() ||
        !transfer(*client, *server)) {
      return false;
    }
    server_events.reject_data = true;
    std::array<std::byte, 256> rejected_frames{};
    std::size_t rejected_size{};
    if (!append_data_frame(rejected_frames, rejected_size,
                           first_rejected_stream->wire_value(), bytes("first").span(), false) ||
        !append_data_frame(rejected_frames, rejected_size,
                           second_rejected_stream->wire_value(), bytes("second").span(), false)) {
      return false;
    }
    const auto rejected_input = ByteView::from({rejected_frames.data(), rejected_size});
    if (!rejected_input.has_value()) {
      return false;
    }
    const auto rejected = server->receive(*rejected_input);
    if (!rejected.has_value() || *rejected != rejected_input->size() ||
        !transfer(*server, *client)) {
      return false;
    }
    return client_events.reset >= 2U;
  }();
  return reset_arena(client_arena, *worker) && reset_arena(server_arena, *worker) && passed;
}

[[nodiscard]] bool check_allocator_reuse() noexcept {
  const auto worker = WorkerId::from_uint64(68);
  if (!worker.has_value()) {
    return false;
  }
  constexpr std::size_t session_capacity = 32768;
  FixedSource client_source{};
  FixedSource server_source{};
  MemoryBudget client_budget{*worker, session_capacity};
  MemoryBudget server_budget{*worker, session_capacity};
  BoundedArena client_arena{*worker, client_budget, block_source(client_source), 4096,
                            session_capacity};
  BoundedArena server_arena{*worker, server_budget, block_source(server_source), 4096,
                            session_capacity};
  const bool passed = [&]() noexcept {
    auto client = Http2Session::create(*worker, Http2Role::client, client_arena, limits());
    auto server = Http2Session::create(*worker, Http2Role::server, server_arena, limits());
    constexpr std::array settings{Http2Setting{3, 8}};
    if (!client.has_value() || !server.has_value() ||
        !client->submit_settings(settings).has_value() ||
        !server->submit_settings(settings).has_value() || !transfer(*client, *server) ||
        !transfer(*server, *client)) {
      return false;
    }
    const std::array request{
        Http2Header{bytes(":method"), bytes("GET"), false},
        Http2Header{bytes(":scheme"), bytes("https"), false},
        Http2Header{bytes(":authority"), bytes("example.test"), false},
        Http2Header{bytes(":path"), bytes("/reuse"), false},
    };
    const std::array response{
        Http2Header{bytes(":status"), bytes("204"), false},
    };
    for (std::size_t iteration = 0; iteration < 128; ++iteration) {
      const auto stream = client->submit_headers(request, true);
      if (!stream.has_value() || !transfer(*client, *server) ||
          !server->submit_headers(*stream, response, true).has_value() ||
          !transfer(*server, *client)) {
        return false;
      }
    }
    return true;
  }();
  return reset_arena(client_arena, *worker) && reset_arena(server_arena, *worker) && passed;
}

[[nodiscard]] bool check_control_frames() noexcept {
  const auto worker = WorkerId::from_uint64(61);
  if (!worker.has_value()) {
    return false;
  }
  FixedSource client_source{};
  FixedSource server_source{};
  MemoryBudget client_budget{*worker, client_source.storage.size()};
  MemoryBudget server_budget{*worker, server_source.storage.size()};
  BoundedArena client_arena{*worker, client_budget, block_source(client_source), 4096,
                            client_source.storage.size()};
  BoundedArena server_arena{*worker, server_budget, block_source(server_source), 4096,
                            server_source.storage.size()};
  const bool passed = [&]() noexcept {
    Events client_events{};
    Events server_events{};
    auto client = Http2Session::create(*worker, Http2Role::client, client_arena, limits(),
                                       Http2EventSink{&client_events, Events::write});
    auto server = Http2Session::create(*worker, Http2Role::server, server_arena, limits(),
                                       Http2EventSink{&server_events, Events::write});
    if (!client.has_value() || !server.has_value() || !transfer(*client, *server) ||
        !transfer(*server, *client)) {
      return false;
    }
    const std::array headers{
        Http2Header{bytes(":method"), bytes("GET"), false},
        Http2Header{bytes(":scheme"), bytes("https"), false},
        Http2Header{bytes(":authority"), bytes("example.test"), false},
        Http2Header{bytes(":path"), bytes("/control"), false},
    };
    const auto stream = client->submit_headers(headers, false);
    if (!stream.has_value() || !transfer(*client, *server) ||
        !server->submit_stream_window_update(*stream, 1024).has_value() ||
        !server->submit_reset(*stream, 8).has_value()) {
      return false;
    }
    const auto control_output = server->next_output();
    if (!control_output.has_value() || control_output->empty() ||
        !client->receive(*control_output).has_value() ||
        !transfer(*server, *client)) {
      return false;
    }
    return server->submit_connection_window_update(1024).has_value();
  }();
  return reset_arena(client_arena, *worker) && reset_arena(server_arena, *worker) && passed;
}

[[nodiscard]] bool check_goaway() noexcept {
  const auto worker = WorkerId::from_uint64(66);
  if (!worker.has_value()) {
    return false;
  }
  FixedSource client_source{};
  FixedSource server_source{};
  MemoryBudget client_budget{*worker, client_source.storage.size()};
  MemoryBudget server_budget{*worker, server_source.storage.size()};
  BoundedArena client_arena{*worker, client_budget, block_source(client_source), 4096,
                            client_source.storage.size()};
  BoundedArena server_arena{*worker, server_budget, block_source(server_source), 4096,
                            server_source.storage.size()};
  const bool passed = [&]() noexcept {
    Events events{};
    auto client = Http2Session::create(*worker, Http2Role::client, client_arena, limits(),
                                       Http2EventSink{&events, Events::write});
    auto server = Http2Session::create(*worker, Http2Role::server, server_arena, limits());
    constexpr std::array settings{Http2Setting{3, 8}};
    if (!client.has_value() || !server.has_value() ||
        !client->submit_settings(settings).has_value() ||
        !server->submit_settings(settings).has_value() || !transfer(*client, *server) ||
        !transfer(*server, *client) ||
        !server->submit_goaway(0, 0, bytes("done")).has_value() ||
        !transfer(*server, *client)) {
      return false;
    }
    return events.goaway == 1U && events.goaway_last_stream == 0 && events.goaway_metadata;
  }();
  return reset_arena(client_arena, *worker) && reset_arena(server_arena, *worker) && passed;
}

[[nodiscard]] bool check_pause_resumption() noexcept {
  const auto worker = WorkerId::from_uint64(65);
  if (!worker.has_value()) {
    return false;
  }
  FixedSource client_source{};
  FixedSource server_source{};
  MemoryBudget client_budget{*worker, client_source.storage.size()};
  MemoryBudget server_budget{*worker, server_source.storage.size()};
  BoundedArena client_arena{*worker, client_budget, block_source(client_source), 4096,
                            client_source.storage.size()};
  BoundedArena server_arena{*worker, server_budget, block_source(server_source), 4096,
                            server_source.storage.size()};
  const bool passed = [&]() noexcept {
    Events events{};
    auto client = Http2Session::create(*worker, Http2Role::client, client_arena, limits());
    auto server = Http2Session::create(*worker, Http2Role::server, server_arena, limits(),
                                       Http2EventSink{&events, Events::write});
    constexpr std::array settings{Http2Setting{3, 8}};
    if (!client.has_value() || !server.has_value() ||
        !client->submit_settings(settings).has_value() ||
        !server->submit_settings(settings).has_value() || !transfer(*client, *server) ||
        !transfer(*server, *client)) {
      return false;
    }
    const std::array headers{
        Http2Header{bytes(":method"), bytes("GET"), false},
        Http2Header{bytes(":scheme"), bytes("https"), false},
        Http2Header{bytes(":authority"), bytes("example.test"), false},
        Http2Header{bytes(":path"), bytes("/paused"), false},
    };
    if (!client->submit_headers(headers, true).has_value() ||
        !client->submit_headers(headers, true).has_value()) {
      return false;
    }
    std::array<std::byte, 4096> wire{};
    std::size_t wire_size{};
    for (std::size_t iteration = 0; iteration < 8; ++iteration) {
      const auto output = client->next_output();
      if (!output.has_value()) {
        return false;
      }
      if (output->empty()) {
        break;
      }
      if (output->size() > wire.size() - wire_size) {
        return false;
      }
      std::copy(output->span().begin(), output->span().end(),
                wire.begin() + static_cast<std::ptrdiff_t>(wire_size));
      wire_size += output->size();
    }
    events.pause_on_header = true;
    const auto input = *ByteView::from({wire.data(), wire_size});
    const auto consumed = server->receive(input);
    if (!consumed.has_value() || *consumed == 0U || *consumed >= wire_size || !events.paused) {
      return false;
    }
    const auto remaining = input.slice(*consumed, wire_size - *consumed);
    if (!remaining.has_value()) {
      return false;
    }
    const auto resumed = server->receive(*remaining);
    return resumed.has_value() && *resumed == remaining->size() && events.headers == 8U;
  }();
  return reset_arena(client_arena, *worker) && reset_arena(server_arena, *worker) && passed;
}

[[nodiscard]] bool check_failures() noexcept {
  const auto worker = WorkerId::from_uint64(62);
  if (!worker.has_value()) {
    return false;
  }
  FixedSource source{};
  MemoryBudget budget{*worker, 512};
  BoundedArena tiny_arena{*worker, budget, block_source(source), 128, 512};
  const auto exhausted = Http2Session::create(*worker, Http2Role::client, tiny_arena, limits());
  const bool bounded_failure =
      !exhausted.has_value() && exhausted.error().code() == ErrorCode::exhaustion &&
      exhausted.error().domain() == laghu::core::ErrorDomain::dependency &&
      exhausted.error().dependency_id() == laghu::core::DependencyId::nghttp2;
  if (!reset_arena(tiny_arena, *worker) || !bounded_failure) {
    return false;
  }

  FixedSource malformed_source{};
  MemoryBudget malformed_budget{*worker, malformed_source.storage.size()};
  BoundedArena malformed_arena{*worker, malformed_budget, block_source(malformed_source), 4096,
                               malformed_source.storage.size()};
  const bool passed = [&]() noexcept {
    auto malformed = Http2Session::create(*worker, Http2Role::server, malformed_arena, limits());
    if (!malformed.has_value()) {
      return false;
    }
    const auto bad = malformed->receive(bytes("not-http2"));
    return !bad.has_value() && bad.error().domain() == laghu::core::ErrorDomain::dependency &&
           bad.error().dependency_id() == laghu::core::DependencyId::nghttp2;
  }();
  return reset_arena(malformed_arena, *worker) && passed;
}

[[nodiscard]] bool check_callback_containment() noexcept {
  const auto worker = WorkerId::from_uint64(63);
  if (!worker.has_value()) {
    return false;
  }
  FixedSource client_source{};
  FixedSource server_source{};
  MemoryBudget client_budget{*worker, client_source.storage.size()};
  MemoryBudget server_budget{*worker, server_source.storage.size()};
  BoundedArena client_arena{*worker, client_budget, block_source(client_source), 4096,
                            client_source.storage.size()};
  BoundedArena server_arena{*worker, server_budget, block_source(server_source), 4096,
                            server_source.storage.size()};
  const bool passed = [&]() noexcept {
    Events events{};
    events.fail_on_settings = true;
    auto client = Http2Session::create(*worker, Http2Role::client, client_arena, limits());
    auto server = Http2Session::create(*worker, Http2Role::server, server_arena, limits(),
                                       Http2EventSink{&events, Events::write});
    if (!client.has_value() || !server.has_value()) {
      return false;
    }
    constexpr std::array settings{Http2Setting{3, 8}};
    if (!client->submit_settings(settings).has_value()) {
      return false;
    }
    for (std::size_t iteration = 0; iteration < 4; ++iteration) {
      const auto output = client->next_output();
      if (!output.has_value() || output->empty()) {
        return false;
      }
      const auto received = server->receive(*output);
      if (!received.has_value()) {
        return received.error().domain() == laghu::core::ErrorDomain::dependency;
      }
    }
    return false;
  }();
  return reset_arena(client_arena, *worker) && reset_arena(server_arena, *worker) && passed;
}

[[nodiscard]] bool check_incoming_header_limit() noexcept {
  const auto worker = WorkerId::from_uint64(64);
  if (!worker.has_value()) {
    return false;
  }
  FixedSource client_source{};
  FixedSource server_source{};
  MemoryBudget client_budget{*worker, client_source.storage.size()};
  MemoryBudget server_budget{*worker, server_source.storage.size()};
  BoundedArena client_arena{*worker, client_budget, block_source(client_source), 4096,
                            client_source.storage.size()};
  BoundedArena server_arena{*worker, server_budget, block_source(server_source), 4096,
                            server_source.storage.size()};
  const bool passed = [&]() noexcept {
    auto client = Http2Session::create(*worker, Http2Role::client, client_arena, limits());
    constexpr Http2Limits narrow_limits{64, 1, 16};
    auto server = Http2Session::create(*worker, Http2Role::server, server_arena, narrow_limits);
    if (!client.has_value() || !server.has_value()) {
      return false;
    }
    constexpr std::array settings{Http2Setting{3, 8}};
    const std::array headers{
        Http2Header{bytes(":method"), bytes("GET"), false},
        Http2Header{bytes(":scheme"), bytes("https"), false},
        Http2Header{bytes(":authority"), bytes("example.test"), false},
        Http2Header{bytes(":path"), bytes("/limited"), false},
    };
    if (!client->submit_settings(settings).has_value() ||
        !client->submit_headers(headers, true).has_value()) {
      return false;
    }
    for (std::size_t iteration = 0; iteration < 8; ++iteration) {
      const auto output = client->next_output();
      if (!output.has_value() || output->empty()) {
        return false;
      }
      const auto received = server->receive(*output);
      if (!received.has_value()) {
        return received.error().domain() == laghu::core::ErrorDomain::core &&
               received.error().code() == ErrorCode::invalid_range;
      }
    }
    return false;
  }();
  return reset_arena(client_arena, *worker) && reset_arena(server_arena, *worker) && passed;
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"adapters.http2.exchange", check_exchange},
      laghu::test::TestCase{"adapters.http2.data_and_flow_control",
                            check_data_and_flow_control},
      laghu::test::TestCase{"adapters.http2.allocator_reuse", check_allocator_reuse},
      laghu::test::TestCase{"adapters.http2.control_frames", check_control_frames},
      laghu::test::TestCase{"adapters.http2.goaway", check_goaway},
      laghu::test::TestCase{"adapters.http2.failures", check_failures},
      laghu::test::TestCase{"adapters.http2.callback_containment", check_callback_containment},
      laghu::test::TestCase{"adapters.http2.incoming_header_limit",
                            check_incoming_header_limit},
      laghu::test::TestCase{"adapters.http2.pause_resumption", check_pause_resumption},
  };
  return laghu::test::run_tests(tests);
}
