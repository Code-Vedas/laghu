// SPDX-License-Identifier: AGPL-3.0-only
#include <algorithm>
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

[[nodiscard]] adapters::QuicPath loopback_path(std::uint16_t local,
                                                std::uint16_t remote) noexcept {
  adapters::QuicEndpoint local_endpoint{};
  local_endpoint.port = local;
  local_endpoint.address[0] = std::byte{127};
  local_endpoint.address[3] = std::byte{1};
  adapters::QuicEndpoint remote_endpoint{};
  remote_endpoint.port = remote;
  remote_endpoint.address[0] = std::byte{127};
  remote_endpoint.address[3] = std::byte{1};
  return {local_endpoint, remote_endpoint};
}

[[nodiscard]] bool accept_connection_id(void*,
    const adapters::QuicConnectionId&) noexcept { return true; }

[[nodiscard]] adapters::QuicConnectionIdSink connection_id_sink() noexcept {
  return {nullptr, accept_connection_id};
}

struct DeterministicCrypto final {
  static constexpr std::size_t tag_size = 16;
  std::array<std::byte, 12> iv{};
  adapters::QuicKeyContext aead_context{};
  adapters::QuicKeyContext header_context{};
  adapters::QuicAeadAlgorithm aead{nullptr, tag_size, encrypt, decrypt};
  adapters::QuicHeaderAlgorithm header{nullptr, protect_header};
  std::size_t received_crypto_bytes{};

  [[nodiscard]] adapters::QuicPacketKey key() noexcept {
    return {&aead, &aead_context, &header, &header_context,
            *core::ByteView::from(iv), {}};
  }

  static bool encrypt(void*, void*, core::MutableByteView output,
                      core::ByteView input, core::ByteView,
                      core::ByteView) noexcept {
    if (output.size() != input.size() + tag_size) return false;
    std::copy(input.span().begin(), input.span().end(), output.span().begin());
    std::fill(output.span().begin() + static_cast<std::ptrdiff_t>(input.size()),
              output.span().end(), std::byte{0xa5});
    return true;
  }

  static bool decrypt(void*, void*, core::MutableByteView output,
                      core::ByteView input, core::ByteView,
                      core::ByteView) noexcept {
    if (input.size() != output.size() + tag_size) return false;
    const auto tag = input.span().last(tag_size);
    if (!std::all_of(tag.begin(), tag.end(),
                     [](std::byte value) { return value == std::byte{0xa5}; })) return false;
    std::copy_n(input.span().begin(), output.size(), output.span().begin());
    return true;
  }

  static bool protect_header(void*, void*, core::MutableByteView output,
                             core::ByteView) noexcept {
    std::fill(output.span().begin(), output.span().end(), std::byte{});
    return true;
  }

  static bool start(void* context, adapters::QuicRole role, core::ByteView,
                    const adapters::QuicCryptoActions& actions) noexcept {
    auto& self = *static_cast<DeterministicCrypto*>(context);
    const auto key = self.key();
    if (!actions.install_initial(actions.context, key, key).has_value()) return false;
    return role != adapters::QuicRole::client ||
           actions.submit(actions.context, adapters::QuicEncryptionLevel::initial,
                          bytes("deterministic client hello")).has_value();
  }

  static bool receive(void* context, adapters::QuicEncryptionLevel,
                      std::uint64_t, core::ByteView input,
                      const adapters::QuicCryptoActions&) noexcept {
    static_cast<DeterministicCrypto*>(context)->received_crypto_bytes += input.size();
    return true;
  }

  [[nodiscard]] adapters::QuicCryptoCallbacks callbacks() noexcept {
    return {this, fill_random, start, receive, crypto_retry, crypto_update};
  }
};

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
  adapters::NativeMemoryPool memory{worker, arena};
  const bool passed = [&]() noexcept {
    auto destination = adapters::QuicConnectionId::create(bytes("destination-id"), worker, generation);
    auto source_id = adapters::QuicConnectionId::create(bytes("source-id"), worker, generation);
    if (!destination.has_value() || !source_id.has_value()) return false;
    auto quic = adapters::QuicSession::create(adapters::QuicRole::client, *destination,
        *source_id, memory, loopback_path(4433, 4434),
        {65536, 16384, 8, 8, 1500}, crypto_callbacks(), {}, connection_id_sink());
    if (!quic.has_value()) return false;
    auto h3 = adapters::Http3Session::create(adapters::Http3Role::client, memory,
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
  adapters::NativeMemoryPool memory{worker, arena};
  const auto h3 = adapters::Http3Session::create(adapters::Http3Role::client, memory,
                                                  {1024, 0, 0});
  const auto first = adapters::QuicConnectionId::create(bytes("12345678"), worker, generation);
  const auto same_worker = adapters::QuicConnectionId::create(bytes("abcdefgh"), worker, generation);
  const auto second = adapters::QuicConnectionId::create(bytes("abcdefgh"), other, generation);
  if (!first.has_value() || !same_worker.has_value()) return false;
  const auto excessive_packet = adapters::QuicSession::create(adapters::QuicRole::client,
      *first, *same_worker, memory, loopback_path(4433, 4434),
      {65536, 16384, 8, 8, std::numeric_limits<std::uint16_t>::max()},
      crypto_callbacks(), {}, connection_id_sink());
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
  adapters::NativeMemoryPool memory{worker, arena};
  const bool passed = [&]() noexcept {
    auto destination = *adapters::QuicConnectionId::create(bytes("destination-id"), worker, generation);
    auto source_id = *adapters::QuicConnectionId::create(bytes("source-id"), worker, generation);
    auto quic = adapters::QuicSession::create(adapters::QuicRole::server, destination, source_id,
        memory, loopback_path(4434, 4433), {65536, 16384, 8, 8, 1500},
        crypto_callbacks(), {}, connection_id_sink());
    if (!quic.has_value()) return false;
    const auto packet = quic->receive_packet(loopback_path(4434, 4433),
                                             bytes("not-a-quic-packet"), 1);
    auto h3 = adapters::Http3Session::create(adapters::Http3Role::server, memory,
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
  adapters::NativeMemoryPool memory{worker, arena};
  auto destination = *adapters::QuicConnectionId::create(
      bytes("destination-id"), worker, generation);
  auto source_id = *adapters::QuicConnectionId::create(bytes("source-id"), worker, generation);
  {
    auto quic = adapters::QuicSession::create(adapters::QuicRole::client, destination,
        source_id, memory, loopback_path(4433, 4434),
        {65536, 16384, 8, 8, 1500}, crypto_callbacks(), {}, connection_id_sink());
    auto h3 = adapters::Http3Session::create(adapters::Http3Role::client, memory,
                                              {16384, 0, 0});
    if (!quic.has_value() || !h3.has_value() || reset_arena(arena, worker)) return false;
    std::array<std::byte, 1200> packet{};
    const auto output = *core::MutableByteView::from(packet);
    if (quic->write_packet(output, -1, {}, false, 1).has_value() ||
        quic->packet_transmitted(1).has_value()) return false;
  }
  return reset_arena(arena, worker);
}

bool native_memory_reuses_freed_blocks() noexcept {
  auto worker = *core::WorkerId::from_uint64(7);
  core::MemoryBudget budget{worker, 64U * 1024U};
  FixedSource source{};
  core::BoundedArena arena{worker, budget,
      {&source, FixedSource::acquire, FixedSource::reset}, 64U * 1024U, 64U * 1024U};
  adapters::NativeMemoryPool memory{worker, arena};
  std::size_t settled_usage{};
  for (std::size_t iteration = 0; iteration < 32U; ++iteration) {
    {
      auto session = adapters::Http3Session::create(
          adapters::Http3Role::client, memory, {16384, 0, 0});
      if (!session.has_value()) return false;
    }
    if (iteration == 0U) {
      settled_usage = arena.used();
    } else if (arena.used() != settled_usage) {
      return false;
    }
  }
  return reset_arena(arena, worker);
}

bool encrypted_quic_packet_loopback() noexcept {
  auto worker = *core::WorkerId::from_uint64(5);
  auto generation = *core::GenerationId::from_uint64(6);
  FixedSource client_source{};
  FixedSource server_source{};
  core::MemoryBudget client_budget{worker, client_source.storage.size()};
  core::MemoryBudget server_budget{worker, server_source.storage.size()};
  core::BoundedArena client_arena{worker, client_budget,
      {&client_source, FixedSource::acquire, FixedSource::reset},
      client_source.storage.size(), client_source.storage.size()};
  core::BoundedArena server_arena{worker, server_budget,
      {&server_source, FixedSource::acquire, FixedSource::reset},
      server_source.storage.size(), server_source.storage.size()};
  adapters::NativeMemoryPool client_memory{worker, client_arena};
  adapters::NativeMemoryPool server_memory{worker, server_arena};
  DeterministicCrypto client_crypto{};
  DeterministicCrypto server_crypto{};
  const bool passed = [&]() noexcept {
    const auto server_id = *adapters::QuicConnectionId::create(
        bytes("server01"), worker, generation);
    const auto client_id = *adapters::QuicConnectionId::create(
        bytes("client01"), worker, generation);
    auto client = adapters::QuicSession::create(adapters::QuicRole::client,
        server_id, client_id, client_memory, loopback_path(4433, 4434),
        {65536, 16384, 8, 8, 1500}, client_crypto.callbacks(), {},
        connection_id_sink());
    auto server = adapters::QuicSession::create(adapters::QuicRole::server,
        server_id, server_id, server_memory, loopback_path(4434, 4433),
        {65536, 16384, 8, 8, 1500}, server_crypto.callbacks(), {},
        connection_id_sink());
    if (!client.has_value() || !server.has_value()) return false;
    std::array<std::byte, 1500> packet{};
    const auto output = *core::MutableByteView::from(packet);
    const auto written = client->write_packet(output, -1, {}, false, 1);
    if (!written.has_value() || written->packet_bytes == 0U ||
        written->path.local.port != 4433 || written->path.remote.port != 4434 ||
        !client->packet_transmitted(1).has_value()) return false;
    const auto wire = *core::ByteView::from(
        std::span<const std::byte>{packet}.first(written->packet_bytes));
    if (!server->receive_packet(loopback_path(4434, 4433), wire, 2).has_value() ||
        server_crypto.received_crypto_bytes == 0U) return false;
    std::fill(packet.begin(), packet.end(), std::byte{});
    const auto acknowledgement = server->write_packet(output, -1, {}, false, 3);
    if (!acknowledgement.has_value() || acknowledgement->packet_bytes == 0U ||
        !server->packet_transmitted(3).has_value()) return false;
    const auto acknowledgement_wire = *core::ByteView::from(
        std::span<const std::byte>{packet}.first(acknowledgement->packet_bytes));
    return client->receive_packet(loopback_path(4433, 4434),
                                  acknowledgement_wire, 4).has_value();
  }();
  return reset_arena(client_arena, worker) && reset_arena(server_arena, worker) && passed;
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
  adapters::NativeMemoryPool client_memory{worker, client_arena};
  adapters::NativeMemoryPool server_memory{worker, server_arena};
  const bool passed = [&]() noexcept {
    HeaderEvents events{};
    auto client = adapters::Http3Session::create(adapters::Http3Role::client, client_memory,
                                                  {16384, 0, 0});
    auto server = adapters::Http3Session::create(adapters::Http3Role::server, server_memory,
        {16384, 0, 0}, {&events, HeaderEvents::write});
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
      laghu::test::TestCase{"adapters.http3.native_memory_reuse", native_memory_reuses_freed_blocks},
      laghu::test::TestCase{"adapters.http3.encrypted_quic_loopback", encrypted_quic_packet_loopback},
      laghu::test::TestCase{"adapters.http3.stream_smoke", h3_stream_smoke_flow},
  };
  return laghu::test::run_tests(tests);
}
