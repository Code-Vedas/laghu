// SPDX-License-Identifier: AGPL-3.0-only
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
  std::size_t headers{};
  std::size_t closed{};
  std::size_t reset{};
  std::size_t goaway{};
  std::size_t window_updates{};
  std::int32_t last_stream{};
  bool fail_on_settings{};

  static Http2CallbackAction write(void* context, const Http2Event& event) noexcept {
    auto& self = *static_cast<Events*>(context);
    if (event.stream.valid()) {
      self.last_stream = event.stream.wire_value();
    }
    switch (event.kind) {
      case Http2EventKind::settings:
        ++self.settings;
        return self.fail_on_settings ? Http2CallbackAction::fail_session
                                     : Http2CallbackAction::continue_processing;
      case Http2EventKind::header:
        ++self.headers;
        break;
      case Http2EventKind::stream_closed:
        ++self.closed;
        break;
      case Http2EventKind::reset:
        ++self.reset;
        break;
      case Http2EventKind::goaway:
        ++self.goaway;
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
           server_events.settings > 0U && client_events.closed > 0U &&
           server_events.closed > 0U;
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
        !transfer(*server, *client) ||
        !server->submit_connection_window_update(1024).has_value() ||
        !server->submit_goaway(stream->wire_value(), 0, bytes("done")).has_value()) {
      return false;
    }
    return true;
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
      laghu::test::TestCase{"adapters.http2.control_frames", check_control_frames},
      laghu::test::TestCase{"adapters.http2.failures", check_failures},
      laghu::test::TestCase{"adapters.http2.callback_containment", check_callback_containment},
      laghu::test::TestCase{"adapters.http2.incoming_header_limit",
                            check_incoming_header_limit},
  };
  return laghu::test::run_tests(tests);
}
