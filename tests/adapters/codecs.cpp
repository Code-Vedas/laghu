// SPDX-License-Identifier: AGPL-3.0-only
#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "laghu_test_support.hpp"

#include <laghu/adapters/codecs.hpp>
#include <laghu/core/bounded_arena.hpp>
#include <laghu/core/memory_budget.hpp>

namespace {

using laghu::adapters::CodecDirection;
using laghu::adapters::CodecLimits;
using laghu::adapters::CodecStateStorage;
using laghu::adapters::NativeMemoryPool;
using laghu::core::ArenaBlockSource;
using laghu::core::BoundedArena;
using laghu::core::ByteView;
using laghu::core::Error;
using laghu::core::ErrorCode;
using laghu::core::MemoryBudget;
using laghu::core::MutableByteView;
using laghu::core::Result;
using laghu::core::WorkerId;

constexpr std::size_t arena_capacity = 32U * 1024U * 1024U;
alignas(std::max_align_t) std::array<std::byte, arena_capacity> arena_storage{};

struct BlockSource final {
  bool fail{};
};

[[nodiscard]] Result<MutableByteView> acquire(void* context,
                                               std::size_t minimum) noexcept {
  auto& source = *static_cast<BlockSource*>(context);
  if (source.fail || minimum > arena_storage.size()) {
    return std::unexpected{Error{laghu::core::ErrorDomain::core,
                                 ErrorCode::exhaustion, 0,
                                 "injected codec allocation failure"}};
  }
  return MutableByteView::from(arena_storage);
}

[[nodiscard]] Result<void> reset(void*) noexcept { return {}; }

struct Fixture final {
  static constexpr WorkerId worker = *WorkerId::from_uint64(1U);
  BlockSource source{};
  MemoryBudget budget{worker, arena_capacity};
  BoundedArena arena{worker, budget, ArenaBlockSource{&source, acquire, reset},
                     4096U, arena_capacity};
  NativeMemoryPool memory{worker, arena};
  std::array<std::max_align_t,
             laghu::adapters::codec_state_storage_words> state{};

  ~Fixture() {
    const auto boundary = arena.quiescent_boundary(worker);
    if (boundary.has_value()) (void)arena.reset(worker, *boundary);
  }

  [[nodiscard]] CodecStateStorage storage() noexcept { return state; }
};

constexpr CodecLimits generous_limits{65536U, 65536U, 65536U};

[[nodiscard]] ByteView bytes(std::span<const std::byte> value) noexcept {
  return *ByteView::from(value);
}

[[nodiscard]] MutableByteView mutable_bytes(std::span<std::byte> value) noexcept {
  return *MutableByteView::from(value);
}

[[nodiscard]] std::span<const std::byte> text_bytes(std::string_view text) noexcept {
  return std::as_bytes(std::span{text.data(), text.size()});
}

[[nodiscard]] bool transform(CodecDirection direction,
                             std::span<const std::byte> input,
                             std::span<std::byte> output,
                             std::size_t input_chunk,
                             std::size_t output_chunk,
                             std::size_t& produced,
                             CodecLimits limits = generous_limits) noexcept {
  Fixture fixture;
  auto stream = LAGHU_CODEC_FACTORY(direction, fixture.storage(), fixture.memory, limits);
  if (!stream.has_value()) return false;
  std::size_t input_offset{};
  std::size_t output_offset{};
  bool finished{};
  while (input_offset < input.size() && !finished) {
    const std::size_t offered_input = std::min(input_chunk, input.size() - input_offset);
    const std::size_t offered_output = std::min(output_chunk, output.size() - output_offset);
    if (offered_output == 0U && direction == CodecDirection::encode) return false;
    const auto progress = stream->process(
        bytes(input.subspan(input_offset, offered_input)),
        mutable_bytes(output.subspan(output_offset, offered_output)));
    if (!progress.has_value()) return false;
    input_offset += progress->input_consumed;
    output_offset += progress->output_produced;
    finished = progress->finished;
    if (!finished && progress->input_consumed == 0U &&
        progress->output_produced == 0U) return false;
  }
  while (!finished) {
    const std::size_t offered_output = std::min(output_chunk, output.size() - output_offset);
    if (offered_output == 0U && direction == CodecDirection::encode) return false;
    const auto progress = stream->finish(
        mutable_bytes(output.subspan(output_offset, offered_output)));
    if (!progress.has_value()) return false;
    output_offset += progress->output_produced;
    finished = progress->finished;
    if (!finished && progress->output_produced == 0U) return false;
  }
  produced = output_offset;
  return input_offset == input.size();
}

[[nodiscard]] bool exact_output_limit() noexcept {
  constexpr std::string_view payload = "exact output limit";
  std::array<std::byte, 4096> compressed{};
  std::size_t compressed_size{};
  if (!transform(CodecDirection::encode, text_bytes(payload), compressed,
                 payload.size(), compressed.size(), compressed_size)) return false;
  std::array<std::byte, payload.size()> decoded{};
  std::size_t decoded_size{};
  return transform(
      CodecDirection::decode,
      std::span<const std::byte>{compressed}.first(compressed_size), decoded,
      compressed_size - 1U, decoded.size(), decoded_size,
      CodecLimits{compressed_size, decoded.size(), 16U}) &&
      decoded_size == payload.size() &&
      std::equal(decoded.begin(), decoded.end(), text_bytes(payload).begin());
}

#if defined(LAGHU_CODEC_zlib_ng)
[[nodiscard]] bool gzip_interoperability() noexcept {
  constexpr std::array gzip_hello{
      std::byte{0x1f}, std::byte{0x8b}, std::byte{0x08}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x03}, std::byte{0xcb}, std::byte{0x48},
      std::byte{0xcd}, std::byte{0xc9}, std::byte{0xc9}, std::byte{0x07},
      std::byte{0x00}, std::byte{0x86}, std::byte{0xa6}, std::byte{0x10},
      std::byte{0x36}, std::byte{0x05}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}};
  constexpr std::string_view expected = "hello";
  std::array<std::byte, expected.size()> decoded{};
  std::size_t decoded_size{};
  if (!transform(CodecDirection::decode, gzip_hello, decoded,
                 gzip_hello.size() - 1U, decoded.size(), decoded_size,
                 CodecLimits{gzip_hello.size(), decoded.size(), 8U}) ||
      decoded_size != expected.size() ||
      !std::equal(decoded.begin(), decoded.end(), text_bytes(expected).begin())) {
    return false;
  }
  std::array<std::byte, 128> encoded{};
  std::size_t encoded_size{};
  return transform(CodecDirection::encode, text_bytes(expected), encoded,
                   expected.size(), encoded.size(), encoded_size) &&
      encoded_size >= 18U && encoded[0] == std::byte{0x1f} &&
      encoded[1] == std::byte{0x8b} && encoded[2] == std::byte{0x08};
}
#endif

[[nodiscard]] bool round_trip(std::size_t input_chunk,
                              std::size_t output_chunk) noexcept {
  constexpr std::string_view payload =
      "Laghu streaming codec payload: abcabcabcabcabcabcabcabcabcabc.";
  std::array<std::byte, 4096> compressed{};
  std::size_t compressed_size{};
  if (!transform(CodecDirection::encode, text_bytes(payload), compressed,
                 input_chunk, output_chunk, compressed_size)) return false;
  std::array<std::byte, 4096> decoded{};
  std::size_t decoded_size{};
  return transform(CodecDirection::decode,
                   std::span<const std::byte>{compressed}.first(compressed_size),
                   decoded, input_chunk, output_chunk, decoded_size) &&
         decoded_size == payload.size() &&
         std::equal(std::span<const std::byte>{decoded}.first(decoded_size).begin(),
                    std::span<const std::byte>{decoded}.first(decoded_size).end(),
                    text_bytes(payload).begin());
}

[[nodiscard]] bool empty_round_trip() noexcept {
  std::array<std::byte, 4096> compressed{};
  std::size_t compressed_size{};
  if (!transform(CodecDirection::encode, {}, compressed, 1U, 1U,
                 compressed_size)) return false;
  std::array<std::byte, 16> decoded{};
  std::size_t decoded_size{};
  return transform(CodecDirection::decode,
                   std::span<const std::byte>{compressed}.first(compressed_size),
                   decoded, 1U, 1U, decoded_size) && decoded_size == 0U;
}

[[nodiscard]] bool failures_and_cancel() noexcept {
  constexpr std::string_view payload = "bounded codec failure payload";
  std::array<std::byte, 4096> compressed{};
  std::size_t compressed_size{};
  if (!transform(CodecDirection::encode, text_bytes(payload), compressed,
                 payload.size(), compressed.size(), compressed_size) ||
      compressed_size < 2U) return false;

  std::array<std::byte, 4096> output{};
  std::size_t ignored{};
  if (transform(CodecDirection::decode,
                std::span<const std::byte>{compressed}.first(compressed_size - 1U),
                output, compressed_size, output.size(), ignored)) return false;

  std::array<std::byte, 32> corrupt{};
  corrupt.fill(std::byte{0xff});
  if (transform(CodecDirection::decode, corrupt, output,
                corrupt.size(), output.size(), ignored)) return false;

  compressed[compressed_size] = std::byte{0x5a};
  if (transform(CodecDirection::decode,
                std::span<const std::byte>{compressed}.first(compressed_size + 1U),
                output, compressed_size + 1U, output.size(), ignored)) return false;

  const CodecLimits output_limit{65536U, 1U, 65536U};
  if (transform(CodecDirection::encode, text_bytes(payload), output,
                payload.size(), output.size(), ignored, output_limit)) return false;

  {
    Fixture fixture;
    auto stream = LAGHU_CODEC_FACTORY(
        CodecDirection::encode, fixture.storage(), fixture.memory,
        CodecLimits{1U, 65536U, 16U});
    if (!stream.has_value()) return false;
    const auto limited = stream->process(
        bytes(text_bytes(payload)), mutable_bytes(output));
    if (limited.has_value() || limited.error().code() != ErrorCode::invalid_range) {
      return false;
    }
  }

  {
    Fixture fixture;
    auto stream = LAGHU_CODEC_FACTORY(
        CodecDirection::encode, fixture.storage(), fixture.memory,
        CodecLimits{65536U, 65536U, 1U});
    if (!stream.has_value()) return false;
    const auto first = stream->process(bytes(text_bytes(payload)), mutable_bytes(output));
    if (!first.has_value()) return false;
    const auto exhausted = stream->finish(mutable_bytes(output));
    if (exhausted.has_value() || exhausted.error().code() != ErrorCode::exhaustion) {
      return false;
    }
  }

  Fixture fixture;
  auto stream = LAGHU_CODEC_FACTORY(CodecDirection::encode, fixture.storage(),
                                    fixture.memory, generous_limits);
  if (!stream.has_value()) return false;
  stream->cancel();
  const auto cancelled = stream->process(
      bytes(text_bytes(payload)), mutable_bytes(output));
  return !cancelled.has_value() && cancelled.error().code() == ErrorCode::invalid_state;
}

[[nodiscard]] bool allocation_failure() noexcept {
  Fixture fixture;
  fixture.source.fail = true;
  const auto stream = LAGHU_CODEC_FACTORY(
      CodecDirection::encode, fixture.storage(), fixture.memory, generous_limits);
  return !stream.has_value() &&
         (stream.error().code() == ErrorCode::exhaustion ||
          (stream.error().code() == ErrorCode::dependency &&
           stream.error().dependency_status() == laghu::core::DependencyStatus::exhaustion));
}

[[nodiscard]] bool finalization_is_latched() noexcept {
  constexpr std::string_view payload = "finalization state payload";
  Fixture fixture;
  auto stream = LAGHU_CODEC_FACTORY(
      CodecDirection::encode, fixture.storage(), fixture.memory,
      generous_limits);
  if (!stream.has_value()) return false;
  std::array<std::byte, 4096> output{};
  const auto progress = stream->process(
      bytes(text_bytes(payload)), mutable_bytes(output));
  if (!progress.has_value()) return false;
  std::array<std::byte, 1> first_finish_output{};
  const auto first_finish = stream->finish(mutable_bytes(first_finish_output));
  if (!first_finish.has_value() || first_finish->finished) return false;
  const auto invalid_process = stream->process(
      bytes(text_bytes("x")), mutable_bytes(output));
  if (invalid_process.has_value() ||
      invalid_process.error().code() != ErrorCode::invalid_state) return false;
  std::size_t finish_calls = 1U;
  bool finished = first_finish->finished;
  while (!finished && finish_calls < 32U) {
    const auto completion = stream->finish(mutable_bytes(output));
    if (!completion.has_value()) return false;
    finished = completion->finished;
    ++finish_calls;
  }
  return finished;
}

[[nodiscard]] bool invalid_direction_is_rejected() noexcept {
  Fixture fixture;
  const auto stream = LAGHU_CODEC_FACTORY(
      static_cast<CodecDirection>(0xffU), fixture.storage(), fixture.memory,
      generous_limits);
  return !stream.has_value() &&
      stream.error().code() == ErrorCode::invalid_input;
}

[[nodiscard]] bool terminal_failure_is_latched() noexcept {
  Fixture fixture;
  auto stream = LAGHU_CODEC_FACTORY(
      CodecDirection::decode, fixture.storage(), fixture.memory,
      generous_limits);
  if (!stream.has_value()) return false;
  std::array<std::byte, 32> corrupt{};
  corrupt.fill(std::byte{0xff});
  std::array<std::byte, 256> output{};
  const auto failure = stream->process(bytes(corrupt), mutable_bytes(output));
  if (failure.has_value()) return false;
  const auto retried = stream->process(bytes(corrupt), mutable_bytes(output));
  return !retried.has_value() &&
      retried.error().code() == ErrorCode::invalid_state;
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"codec.round-trip", []() noexcept { return round_trip(4096U, 4096U); }},
      laghu::test::TestCase{"codec.byte-at-a-time", []() noexcept { return round_trip(1U, 1U); }},
      laghu::test::TestCase{"codec.empty", empty_round_trip},
      laghu::test::TestCase{"codec.exact-output-limit", exact_output_limit},
#if defined(LAGHU_CODEC_zlib_ng)
      laghu::test::TestCase{"codec.gzip-interoperability", gzip_interoperability},
#endif
      laghu::test::TestCase{"codec.failures-cancel", failures_and_cancel},
      laghu::test::TestCase{"codec.allocation-failure", allocation_failure},
      laghu::test::TestCase{"codec.finalization-latched", finalization_is_latched},
      laghu::test::TestCase{"codec.invalid-direction", invalid_direction_is_rejected},
      laghu::test::TestCase{"codec.terminal-failure-latched", terminal_failure_is_latched},
  };
  return laghu::test::run_tests(tests);
}
