// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

#include "laghu_test_support.hpp"

#include <laghu/adapters/http3.hpp>
#include <laghu/adapters/quic.hpp>
#include <laghu/core/memory_budget.hpp>

namespace {

using namespace laghu;

struct FixedSource final {
  alignas(std::max_align_t) std::array<std::byte, 512U * 1024U> storage{};
  bool fail{};
  static core::Result<core::MutableByteView> acquire(void* context,
                                                     std::size_t minimum) noexcept {
    auto& self = *static_cast<FixedSource*>(context);
    if (self.fail || minimum > self.storage.size()) {
      return std::unexpected{core::Error{core::ErrorDomain::core,
          core::ErrorCode::exhaustion, 0, "HTTP/3 test storage exhausted"}};
    }
    return core::MutableByteView::from(self.storage);
  }
  static core::Result<void> reset(void*) noexcept { return {}; }
};

[[nodiscard]] core::ByteView bytes(std::string_view value) noexcept {
  return *core::ByteView::from({reinterpret_cast<const std::byte*>(value.data()), value.size()});
}

[[nodiscard]] bool fill_random(void*, core::MutableByteView output) noexcept {
  std::uint8_t value{1};
  for (std::byte& byte : output.span()) byte = static_cast<std::byte>(value++);
  return true;
}

[[nodiscard]] bool crypto_start(void*, adapters::QuicRole, core::ByteView,
                                const adapters::QuicCryptoActions&) noexcept { return true; }
[[nodiscard]] bool crypto_receive(void*, adapters::QuicEncryptionLevel, std::uint64_t,
    core::ByteView, const adapters::QuicCryptoActions&) noexcept { return true; }
[[nodiscard]] bool crypto_retry(void*, const adapters::QuicCryptoActions&) noexcept {
  return true;
}
[[nodiscard]] bool crypto_update(void*, core::MutableByteView, core::MutableByteView,
    core::MutableByteView, core::MutableByteView, core::ByteView, core::ByteView,
    adapters::QuicKeyContext*&, adapters::QuicKeyContext*&) noexcept { return false; }

[[nodiscard]] adapters::QuicCryptoCallbacks crypto_callbacks() noexcept {
  return {nullptr, fill_random, crypto_start, crypto_receive, crypto_retry, crypto_update};
}

[[nodiscard]] bool reset_arena(core::BoundedArena& arena,
                               core::WorkerId worker) noexcept {
  const auto boundary = arena.quiescent_boundary(worker);
  return boundary.has_value() && arena.reset(worker, *boundary).has_value();
}

bool contracts_construct_and_remain_separate() noexcept {
  auto worker = *core::WorkerId::from_uint64(1);
  auto generation = *core::GenerationId::from_uint64(2);
  core::MemoryBudget budget{worker, 1024U * 1024U};
  FixedSource source{};
  core::BoundedArena arena{worker, budget,
      {&source, FixedSource::acquire, FixedSource::reset}, 512U * 1024U, 512U * 1024U};
  const bool passed = [&]() noexcept {
    auto destination = adapters::QuicConnectionId::create(bytes("destination-id"), worker, generation);
    auto source_id = adapters::QuicConnectionId::create(bytes("source-id"), worker, generation);
    if (!destination.has_value() || !source_id.has_value()) return false;
    auto quic = adapters::QuicSession::create(adapters::QuicRole::client, *destination,
        *source_id, arena, {65536, 16384, 8, 8, 1500}, crypto_callbacks());
    if (!quic.has_value()) return false;
    auto h3 = adapters::Http3Session::create(adapters::Http3Role::client, worker, arena,
                                              {16384, 0, 0});
    return h3.has_value() && destination->worker().value() == 1 &&
           destination->generation().value() == 2 &&
           destination->value().size() == bytes("destination-id").size();
  }();
  return reset_arena(arena, worker) && passed;
}

bool invalid_metadata_and_allocation_fail() noexcept {
  auto worker = *core::WorkerId::from_uint64(1);
  auto other = *core::WorkerId::from_uint64(2);
  auto generation = *core::GenerationId::from_uint64(3);
  const auto oversized = adapters::QuicConnectionId::create(
      bytes("123456789012345678901"), worker, generation);
  core::MemoryBudget budget{worker, 1024};
  FixedSource source{};
  source.fail = true;
  core::BoundedArena arena{worker, budget,
      {&source, FixedSource::acquire, FixedSource::reset}, 1024, 1024};
  const auto h3 = adapters::Http3Session::create(adapters::Http3Role::client, worker,
                                                  arena, {1024, 0, 0});
  const auto first = adapters::QuicConnectionId::create(bytes("12345678"), worker, generation);
  const auto same_worker = adapters::QuicConnectionId::create(bytes("abcdefgh"), worker, generation);
  const auto second = adapters::QuicConnectionId::create(bytes("abcdefgh"), other, generation);
  if (!first.has_value() || !same_worker.has_value()) return false;
  const auto excessive_packet = adapters::QuicSession::create(adapters::QuicRole::client,
      *first, *same_worker, arena,
      {65536, 16384, 8, 8, std::numeric_limits<std::uint16_t>::max()}, crypto_callbacks());
  return !oversized.has_value() && !h3.has_value() && second.has_value() &&
         !excessive_packet.has_value() &&
         excessive_packet.error().code() == core::ErrorCode::invalid_input;
}

bool malformed_inputs_are_typed() noexcept {
  auto worker = *core::WorkerId::from_uint64(1);
  auto generation = *core::GenerationId::from_uint64(2);
  core::MemoryBudget budget{worker, 1024U * 1024U};
  FixedSource source{};
  core::BoundedArena arena{worker, budget,
      {&source, FixedSource::acquire, FixedSource::reset}, 512U * 1024U, 512U * 1024U};
  const bool passed = [&]() noexcept {
    auto destination = *adapters::QuicConnectionId::create(bytes("destination-id"), worker, generation);
    auto source_id = *adapters::QuicConnectionId::create(bytes("source-id"), worker, generation);
    auto quic = adapters::QuicSession::create(adapters::QuicRole::server, destination, source_id,
        arena, {65536, 16384, 8, 8, 1500}, crypto_callbacks());
    if (!quic.has_value()) return false;
    const auto packet = quic->receive_packet(bytes("not-a-quic-packet"), 1);
    auto h3 = adapters::Http3Session::create(adapters::Http3Role::server, worker, arena,
                                              {16384, 0, 0});
    if (!h3.has_value()) return false;
    const auto frame = h3->receive(0, bytes("malformed"), true);
    return !packet.has_value() && !frame.has_value() && !h3->next_output().has_value();
  }();
  return reset_arena(arena, worker) && passed;
}

bool arena_reset_requires_session_cleanup() noexcept {
  auto worker = *core::WorkerId::from_uint64(1);
  auto generation = *core::GenerationId::from_uint64(2);
  core::MemoryBudget budget{worker, 1024U * 1024U};
  FixedSource source{};
  core::BoundedArena arena{worker, budget,
      {&source, FixedSource::acquire, FixedSource::reset}, 512U * 1024U, 512U * 1024U};
  auto destination = *adapters::QuicConnectionId::create(
      bytes("destination-id"), worker, generation);
  auto source_id = *adapters::QuicConnectionId::create(bytes("source-id"), worker, generation);
  {
    auto quic = adapters::QuicSession::create(adapters::QuicRole::client, destination,
        source_id, arena, {65536, 16384, 8, 8, 1500}, crypto_callbacks());
    auto h3 = adapters::Http3Session::create(adapters::Http3Role::client, worker, arena,
                                              {16384, 0, 0});
    if (!quic.has_value() || !h3.has_value() || reset_arena(arena, worker)) return false;
    std::array<std::byte, 1200> packet{};
    const auto output = *core::MutableByteView::from(packet);
    if (quic->write_packet(output, -1, {}, false, 1).has_value() ||
        quic->packet_transmitted(1).has_value()) return false;
  }
  return reset_arena(arena, worker);
}

struct HeaderEvents final {
  std::size_t headers{};
  static bool write(void* context, const adapters::Http3Event& event) noexcept {
    auto& self = *static_cast<HeaderEvents*>(context);
    if (event.kind == adapters::Http3EventKind::header) ++self.headers;
    return true;
  }
};

bool h3_stream_smoke_flow() noexcept {
  auto worker = *core::WorkerId::from_uint64(4);
  FixedSource client_source{};
  FixedSource server_source{};
  core::MemoryBudget client_budget{worker, client_source.storage.size()};
  core::MemoryBudget server_budget{worker, server_source.storage.size()};
  core::BoundedArena client_arena{worker, client_budget,
      {&client_source, FixedSource::acquire, FixedSource::reset}, 512U * 1024U, 512U * 1024U};
  core::BoundedArena server_arena{worker, server_budget,
      {&server_source, FixedSource::acquire, FixedSource::reset}, 512U * 1024U, 512U * 1024U};
  const bool passed = [&]() noexcept {
    HeaderEvents events{};
    auto client = adapters::Http3Session::create(adapters::Http3Role::client, worker,
                                                  client_arena, {16384, 0, 0});
    auto server = adapters::Http3Session::create(adapters::Http3Role::server, worker,
        server_arena, {16384, 0, 0}, {&events, HeaderEvents::write});
    if (!client.has_value() || !server.has_value() ||
        !client->bind_streams(2, 6, 10).has_value() ||
        !server->bind_streams(3, 7, 11).has_value()) return false;
    const std::array headers{
        adapters::Http3Header{bytes(":method"), bytes("GET")},
        adapters::Http3Header{bytes(":scheme"), bytes("https")},
        adapters::Http3Header{bytes(":authority"), bytes("example.test")},
        adapters::Http3Header{bytes(":path"), bytes("/")},
    };
    if (!client->submit_request(0, headers).has_value()) return false;
    for (std::size_t attempt = 0; attempt < 16 && events.headers < headers.size(); ++attempt) {
      const auto output = client->next_output();
      if (!output.has_value()) return false;
      if (output->stream_id < 0) continue;
      if (!server->receive(output->stream_id, output->bytes, output->fin).has_value() ||
          !client->mark_output_written(output->stream_id, output->bytes.size()).has_value() ||
          !client->acknowledge_stream_data(
              output->stream_id, output->bytes.size()).has_value()) {
        return false;
      }
    }
    return events.headers == headers.size();
  }();
  return reset_arena(client_arena, worker) && reset_arena(server_arena, worker) && passed;
}

}  // namespace

static_assert(!std::is_aggregate_v<laghu::adapters::QuicConnectionId>);

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"adapters.http3.separate_contracts", contracts_construct_and_remain_separate},
      laghu::test::TestCase{"adapters.http3.cid_and_allocation", invalid_metadata_and_allocation_fail},
      laghu::test::TestCase{"adapters.http3.malformed_input", malformed_inputs_are_typed},
      laghu::test::TestCase{"adapters.http3.arena_reset", arena_reset_requires_session_cleanup},
      laghu::test::TestCase{"adapters.http3.stream_smoke", h3_stream_smoke_flow},
  };
  return laghu::test::run_tests(tests);
}
