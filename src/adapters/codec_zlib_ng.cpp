// SPDX-License-Identifier: AGPL-3.0-only
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <zlib-ng.h>

#include <laghu/adapters/codecs.hpp>
#include <laghu/adapters/dependency.hpp>
#include <laghu/adapters/internal/arena_memory.hpp>
#include <laghu/adapters/internal/codecs.hpp>

namespace laghu::adapters {
namespace {

struct ZlibState final {
  internal::CodecAccounting accounting{};
  internal::ArenaMemory memory{};
  zng_stream stream{};
  DependencyLogSink log{};
  bool initialized{};
};

void* zlib_allocate(void* context, std::uint32_t count, std::uint32_t size) noexcept {
  return internal::arena_calloc(count, size, context);
}

void zlib_free(void* context, void* pointer) noexcept {
  internal::arena_free(pointer, context);
}

[[nodiscard]] core::Error zlib_error(core::DependencyOperation operation,
                                     int code,
                                     const DependencyLogSink& log) noexcept {
  const core::DependencyStatus status = code == Z_MEM_ERROR
      ? core::DependencyStatus::exhaustion
      : code == Z_DATA_ERROR ? core::DependencyStatus::corrupt_data
                             : core::DependencyStatus::invalid_input;
  const core::Error error = normalize_dependency_error(
      core::DependencyId::zlib_ng, operation, status, code);
  log_dependency_error(log, error);
  return error;
}

[[nodiscard]] core::Result<CodecProgress> zlib_process(
    void* opaque, core::ByteView input, core::MutableByteView output,
    bool finishing) noexcept {
  auto& state = *static_cast<ZlibState*>(opaque);
  if (const auto valid = internal::preflight(state.accounting, input.size());
      !valid.has_value()) return std::unexpected{valid.error()};
  const std::size_t output_size = internal::bounded_output_size(
      state.accounting, output.size());
  if (output_size == 0U &&
      state.accounting.direction == CodecDirection::encode) {
    return std::unexpected{internal::codec_error(
        core::ErrorCode::exhaustion, "codec output limit is exhausted")};
  }
  const std::size_t offered_input = std::min<std::size_t>(
      input.size(), std::numeric_limits<std::uint32_t>::max());
  const std::size_t offered_output = std::min<std::size_t>(
      output_size, std::numeric_limits<std::uint32_t>::max());
  state.stream.next_in = reinterpret_cast<const unsigned char*>(input.data());
  state.stream.avail_in = static_cast<std::uint32_t>(offered_input);
  state.stream.next_out = reinterpret_cast<unsigned char*>(output.data());
  state.stream.avail_out = static_cast<std::uint32_t>(offered_output);
  const int result = state.accounting.direction == CodecDirection::encode
      ? zng_deflate(&state.stream, finishing ? Z_FINISH : Z_NO_FLUSH)
      : zng_inflate(&state.stream, Z_NO_FLUSH);
  const std::size_t consumed = offered_input - state.stream.avail_in;
  const std::size_t produced = offered_output - state.stream.avail_out;
  const bool finished = result == Z_STREAM_END;
  if (finished && consumed != input.size()) {
    return std::unexpected{internal::codec_error(
        core::ErrorCode::corrupt_data, "compressed stream has trailing data")};
  }
  if (result != Z_OK && result != Z_BUF_ERROR && !finished) {
    return std::unexpected{zlib_error(
        core::DependencyOperation::codec_process, result, state.log)};
  }
  if (finishing && state.accounting.direction == CodecDirection::decode &&
      !finished && consumed == input.size() && state.stream.avail_out != 0U) {
    return std::unexpected{internal::codec_error(
        core::ErrorCode::corrupt_data, "compressed stream is truncated")};
  }
  internal::record(state.accounting, consumed, produced, finished);
  return CodecProgress{consumed, produced,
                       !finished && consumed == input.size(),
                       !finished && state.stream.avail_out == 0U,
                       finished};
}

void zlib_destroy(void* opaque) noexcept {
  auto& state = *static_cast<ZlibState*>(opaque);
  if (state.initialized) {
    if (state.accounting.direction == CodecDirection::encode) {
      (void)zng_deflateEnd(&state.stream);
    } else {
      (void)zng_inflateEnd(&state.stream);
    }
  }
  state.~ZlibState();
}

void zlib_cancel(void* opaque) noexcept {
  static_cast<ZlibState*>(opaque)->accounting.cancelled = true;
}

constexpr internal::CodecOperations zlib_operations{
    zlib_process, zlib_destroy, zlib_cancel};

}  // namespace

core::Result<CodecStream> create_zlib_ng_codec(
    CodecDirection direction, CodecStateStorage state_storage,
    NativeMemoryPool& memory, CodecLimits limits,
    DependencyLogSink log_sink) noexcept {
  if (!internal::valid_limits(limits)) {
    return std::unexpected{internal::codec_error(
        core::ErrorCode::invalid_input, "codec limits must be positive")};
  }
  internal::ArenaMemoryAccess::synchronize(memory);
  auto& arena = internal::ArenaMemoryAccess::arena(memory);
  const auto worker = internal::ArenaMemoryAccess::worker(memory);
  auto pin = arena.pin(worker);
  if (!pin.has_value()) return std::unexpected{pin.error()};
  auto created = internal::construct_state<ZlibState>(state_storage);
  if (!created.has_value()) return std::unexpected{created.error()};
  ZlibState& state = **created;
  state.accounting = {limits, 0U, 0U, 0U, direction, false, false};
  state.memory.pool = &memory;
  state.log = log_sink;
  state.stream.zalloc = zlib_allocate;
  state.stream.zfree = zlib_free;
  state.stream.opaque = &state.memory;
  const int result = direction == CodecDirection::encode
      ? zng_deflateInit2(&state.stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                         MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY)
      : zng_inflateInit2(&state.stream, MAX_WBITS + 32);
  if (result != Z_OK) {
    state.~ZlibState();
    return std::unexpected{zlib_error(
        core::DependencyOperation::codec_initialize, result, log_sink)};
  }
  state.initialized = true;
  return CodecStream{&state, zlib_operations, arena, arena.generation(),
                     std::move(*pin)};
}

}  // namespace laghu::adapters
