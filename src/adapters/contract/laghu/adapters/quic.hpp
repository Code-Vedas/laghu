// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <laghu/adapters/dependency.hpp>
#include <laghu/core/bounded_arena.hpp>
#include <laghu/core/identifiers.hpp>
#include <laghu/core/views.hpp>

namespace laghu::adapters {

enum class QuicRole : std::uint8_t { client, server };
enum class QuicEncryptionLevel : std::uint8_t { initial, early_data, handshake, application };
enum class QuicKeyDirection : std::uint8_t { receive, transmit };

class QuicConnectionId final {
 public:
  static constexpr std::size_t capacity = 20;

  [[nodiscard]] static core::Result<QuicConnectionId> create(
      core::ByteView value, core::WorkerId worker,
      core::GenerationId generation) noexcept;
  [[nodiscard]] core::ByteView value() const noexcept {
    return *core::ByteView::from(std::span{bytes_}.first(size_));
  }
  [[nodiscard]] constexpr core::WorkerId worker() const noexcept { return worker_; }
  [[nodiscard]] constexpr core::GenerationId generation() const noexcept { return generation_; }

 private:
  friend class QuicSession;
  constexpr QuicConnectionId(std::array<std::byte, capacity> bytes, std::uint8_t size,
                             core::WorkerId worker, core::GenerationId generation) noexcept
      : bytes_(bytes), size_(size), worker_(worker), generation_(generation) {}
  std::array<std::byte, capacity> bytes_{};
  std::uint8_t size_{};
  core::WorkerId worker_;
  core::GenerationId generation_;
};

enum class QuicEventKind : std::uint8_t {
  handshake_data,
  handshake_completed,
  stream_opened,
  stream_data,
  stream_reset,
  stop_sending,
  stream_closed,
};

struct QuicEvent final {
  QuicEventKind kind{QuicEventKind::stream_data};
  QuicEncryptionLevel encryption_level{QuicEncryptionLevel::initial};
  std::int64_t stream_id{-1};
  std::uint64_t offset{};
  std::uint64_t application_error{};
  core::ByteView data{};
  bool fin{};
};

using QuicEventWrite = bool (*)(void*, const QuicEvent&) noexcept;
struct QuicEventSink final {
  void* context{};
  QuicEventWrite write{};
};

using QuicRandomFill = bool (*)(void*, core::MutableByteView) noexcept;

using QuicPacketProtect = bool (*)(void* callback_context, void* key_context,
    core::MutableByteView output, core::ByteView input, core::ByteView nonce,
    core::ByteView associated_data) noexcept;
using QuicHeaderProtect = bool (*)(void* callback_context, void* key_context,
    core::MutableByteView output, core::ByteView sample) noexcept;
using QuicKeyDestroy = void (*)(void* callback_context, void* key_context) noexcept;

struct QuicAeadAlgorithm final {
  void* callback_context{};
  std::size_t maximum_overhead{};
  QuicPacketProtect encrypt{};
  QuicPacketProtect decrypt{};
};

struct QuicHeaderAlgorithm final {
  void* callback_context{};
  QuicHeaderProtect protect{};
};

struct QuicKeyContext final {
  void* handle{};
  void* destroy_context{};
  QuicKeyDestroy destroy{};
};

struct QuicPacketKey final {
  const QuicAeadAlgorithm* aead{};
  QuicKeyContext* aead_context{};
  const QuicHeaderAlgorithm* header{};
  QuicKeyContext* header_context{};
  core::ByteView iv{};
  core::ByteView secret{};
};

struct QuicCryptoActions final {
  void* context{};
  core::Result<void> (*install_initial)(void*, const QuicPacketKey&,
                                        const QuicPacketKey&) noexcept{};
  core::Result<void> (*install_handshake)(void*, QuicKeyDirection,
                                          const QuicPacketKey&) noexcept{};
  core::Result<void> (*install_application)(void*, QuicKeyDirection,
                                            const QuicPacketKey&) noexcept{};
  core::Result<void> (*submit)(void*, QuicEncryptionLevel,
                               core::ByteView) noexcept{};
};

using QuicCryptoStart = bool (*)(void*, QuicRole, core::ByteView,
                                 const QuicCryptoActions&) noexcept;
using QuicCryptoReceive = bool (*)(void*, QuicEncryptionLevel, std::uint64_t,
    core::ByteView, const QuicCryptoActions&) noexcept;
using QuicCryptoRetry = bool (*)(void*, const QuicCryptoActions&) noexcept;
using QuicCryptoUpdate = bool (*)(void*, core::MutableByteView, core::MutableByteView,
    core::MutableByteView, core::MutableByteView, core::ByteView, core::ByteView,
    QuicKeyContext*&, QuicKeyContext*&) noexcept;

struct QuicCryptoCallbacks final {
  void* context{};
  QuicRandomFill random_fill{};
  QuicCryptoStart start{};
  QuicCryptoReceive receive{};
  QuicCryptoRetry retry{};
  QuicCryptoUpdate update{};
};

struct QuicLimits final {
  std::uint64_t initial_max_data{};
  std::uint64_t initial_max_stream_data{};
  std::uint64_t initial_max_streams_bidi{};
  std::uint64_t initial_max_streams_uni{};
  std::size_t maximum_packet_bytes{};
};

struct QuicPacketWrite final {
  std::size_t packet_bytes{};
  std::size_t stream_bytes{};
};

class QuicSession final {
 public:
  QuicSession(const QuicSession&) = delete;
  QuicSession& operator=(const QuicSession&) = delete;
  QuicSession(QuicSession&& other) noexcept;
  QuicSession& operator=(QuicSession&& other) noexcept;
  ~QuicSession();

  [[nodiscard]] static core::Result<QuicSession> create(
      QuicRole role, const QuicConnectionId& destination,
      const QuicConnectionId& source, core::BoundedArena& arena,
      QuicLimits limits, QuicCryptoCallbacks crypto,
      QuicEventSink events = {}, DependencyLogSink log_sink = {}) noexcept;

  [[nodiscard]] core::Result<void> receive_packet(core::ByteView packet,
                                                  std::uint64_t now_ns) noexcept;
  [[nodiscard]] core::Result<QuicPacketWrite> write_packet(
      core::MutableByteView output, std::int64_t stream_id,
      core::ByteView stream_data, bool fin, std::uint64_t now_ns) noexcept;
  [[nodiscard]] core::Result<std::int64_t> open_bidirectional_stream() noexcept;
  [[nodiscard]] core::Result<void> reset_stream(std::int64_t stream_id,
                                                std::uint64_t code) noexcept;
  [[nodiscard]] core::Result<void> stop_sending(std::int64_t stream_id,
                                                std::uint64_t code) noexcept;
  [[nodiscard]] std::uint64_t expiry_ns() const noexcept;
  [[nodiscard]] core::Result<void> handle_expiry(std::uint64_t now_ns) noexcept;

 private:
  constexpr QuicSession(void* connection, void* state, const core::BoundedArena& arena,
                        std::uint64_t generation) noexcept
      : connection_(connection), state_(state), arena_(&arena), generation_(generation) {}
  [[nodiscard]] core::Result<void> require_valid() const noexcept;
  void release() noexcept;
  void move_from(QuicSession&& other) noexcept;

  void* connection_{};
  void* state_{};
  const core::BoundedArena* arena_{};
  std::uint64_t generation_{};
};

static_assert(std::is_trivially_copyable_v<QuicConnectionId>);
static_assert(!std::is_copy_constructible_v<QuicSession>);

}  // namespace laghu::adapters
