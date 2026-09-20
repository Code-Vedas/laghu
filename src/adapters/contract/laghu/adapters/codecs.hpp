// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

#include <laghu/adapters/dependency.hpp>
#include <laghu/adapters/native_memory.hpp>
#include <laghu/core/views.hpp>

namespace laghu::adapters {

namespace internal {
struct CodecOperations;
}

enum class CodecDirection : std::uint8_t { encode, decode };

struct CodecLimits final {
  std::size_t maximum_input_bytes{};
  std::size_t maximum_output_bytes{};
  std::uint32_t maximum_work_calls{};
};

struct CodecProgress final {
  std::size_t input_consumed{};
  std::size_t output_produced{};
  bool needs_input{};
  bool needs_output{};
  bool finished{};
};

inline constexpr std::size_t codec_state_storage_words = 64U;
using CodecStateStorage = std::span<std::max_align_t, codec_state_storage_words>;

class CodecStream final {
 public:
  CodecStream(const CodecStream&) = delete;
  CodecStream& operator=(const CodecStream&) = delete;
  CodecStream(CodecStream&& other) noexcept;
  CodecStream& operator=(CodecStream&& other) noexcept;
  ~CodecStream();

  [[nodiscard]] core::Result<CodecProgress> process(
      core::ByteView input, core::MutableByteView output) noexcept;
  [[nodiscard]] core::Result<CodecProgress> finish(
      core::MutableByteView output) noexcept;
  void cancel() noexcept;

 private:
  friend core::Result<CodecStream> create_zlib_ng_codec(
      CodecDirection, CodecStateStorage, NativeMemoryPool&, CodecLimits,
      DependencyLogSink) noexcept;
  friend core::Result<CodecStream> create_brotli_codec(
      CodecDirection, CodecStateStorage, NativeMemoryPool&, CodecLimits,
      DependencyLogSink) noexcept;
  friend core::Result<CodecStream> create_zstd_codec(
      CodecDirection, CodecStateStorage, NativeMemoryPool&, CodecLimits,
      DependencyLogSink) noexcept;

  CodecStream(void* state, const internal::CodecOperations& operations,
              const core::BoundedArena& arena, std::uint64_t generation,
              core::ArenaPin pin) noexcept
      : state_(state), operations_(&operations), arena_(&arena),
        generation_(generation), pin_(std::move(pin)) {}

  [[nodiscard]] core::Result<void> require_valid() const noexcept;
  void release() noexcept;
  void move_from(CodecStream&& other) noexcept;

  void* state_{};
  const internal::CodecOperations* operations_{};
  const core::BoundedArena* arena_{};
  std::uint64_t generation_{};
  core::ArenaPin pin_{};
};

[[nodiscard]] core::Result<CodecStream> create_zlib_ng_codec(
    CodecDirection direction, CodecStateStorage state_storage,
    NativeMemoryPool& memory, CodecLimits limits,
    DependencyLogSink log_sink = {}) noexcept;
[[nodiscard]] core::Result<CodecStream> create_brotli_codec(
    CodecDirection direction, CodecStateStorage state_storage,
    NativeMemoryPool& memory, CodecLimits limits,
    DependencyLogSink log_sink = {}) noexcept;
[[nodiscard]] core::Result<CodecStream> create_zstd_codec(
    CodecDirection direction, CodecStateStorage state_storage,
    NativeMemoryPool& memory, CodecLimits limits,
    DependencyLogSink log_sink = {}) noexcept;

static_assert(std::is_trivially_copyable_v<CodecLimits>);
static_assert(std::is_trivially_copyable_v<CodecProgress>);
static_assert(!std::is_copy_constructible_v<CodecStream>);

}  // namespace laghu::adapters
