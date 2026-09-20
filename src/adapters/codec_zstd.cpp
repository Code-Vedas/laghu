// SPDX-License-Identifier: AGPL-3.0-only
#include <cstddef>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zstd_errors.h>

#include <laghu/adapters/codecs.hpp>
#include <laghu/adapters/dependency.hpp>
#include <laghu/adapters/internal/arena_memory.hpp>
#include <laghu/adapters/internal/codecs.hpp>

namespace laghu::adapters {
namespace {

struct ZstdState final {
  internal::CodecAccounting accounting{};
  internal::ArenaMemory memory{};
  ZSTD_CCtx* encoder{};
  ZSTD_DCtx* decoder{};
  DependencyLogSink log{};
};

void* zstd_allocate(void* context, std::size_t size) noexcept {
  return internal::arena_malloc(size, context);
}

void zstd_free(void* context, void* pointer) noexcept {
  internal::arena_free(pointer, context);
}

[[nodiscard]] core::Error zstd_error(core::DependencyOperation operation,
                                     std::size_t result,
                                     const DependencyLogSink& log) noexcept {
  const auto code = ZSTD_getErrorCode(result);
  const core::DependencyStatus status = code == ZSTD_error_memory_allocation
      ? core::DependencyStatus::exhaustion
      : core::DependencyStatus::corrupt_data;
  const core::Error error = normalize_dependency_error(
      core::DependencyId::zstd, operation, status, static_cast<int>(code));
  log_dependency_error(log, error);
  return error;
}

[[nodiscard]] core::Error zstd_allocation_error(
    core::DependencyOperation operation,
    const DependencyLogSink& log) noexcept {
  const core::Error error = normalize_dependency_error(
      core::DependencyId::zstd, operation,
      core::DependencyStatus::exhaustion,
      static_cast<int>(ZSTD_error_memory_allocation));
  log_dependency_error(log, error);
  return error;
}

[[nodiscard]] core::Result<CodecProgress> zstd_process(
    void* opaque, core::ByteView input, core::MutableByteView output,
    bool finishing) noexcept {
  auto& state = *static_cast<ZstdState*>(opaque);
  if (const auto valid = internal::preflight(state.accounting, input.size());
      !valid.has_value()) return std::unexpected{valid.error()};
  const std::size_t output_size = internal::bounded_output_size(
      state.accounting, output.size());
  if (output_size == 0U) {
    return std::unexpected{internal::codec_error(
        core::ErrorCode::exhaustion, "codec output limit is exhausted")};
  }
  ZSTD_inBuffer source{input.data(), input.size(), 0U};
  ZSTD_outBuffer destination{output.data(), output_size, 0U};
  std::size_t result{};
  bool finished{};
  bool needs_input{};
  bool needs_output{};
  if (state.accounting.direction == CodecDirection::encode) {
    result = ZSTD_compressStream2(state.encoder, &destination, &source,
        finishing ? ZSTD_e_end : ZSTD_e_continue);
    if (ZSTD_isError(result) != 0U) {
      return std::unexpected{zstd_error(
          core::DependencyOperation::codec_process, result, state.log)};
    }
    finished = finishing && result == 0U;
    needs_output = !finished && destination.pos == destination.size;
    needs_input = !finished && !finishing && source.pos == source.size && !needs_output;
  } else {
    result = ZSTD_decompressStream(state.decoder, &destination, &source);
    if (ZSTD_isError(result) != 0U) {
      return std::unexpected{zstd_error(
          core::DependencyOperation::codec_process, result, state.log)};
    }
    finished = result == 0U;
    needs_output = !finished && destination.pos == destination.size;
    needs_input = !finished && source.pos == source.size && !needs_output;
    if (finished && source.pos != source.size) {
      return std::unexpected{internal::codec_error(
          core::ErrorCode::corrupt_data, "compressed stream has trailing data")};
    }
    if (finishing && !finished && needs_input) {
      return std::unexpected{internal::codec_error(
          core::ErrorCode::corrupt_data, "compressed stream is truncated")};
    }
  }
  internal::record(state.accounting, source.pos, destination.pos, finished);
  return CodecProgress{source.pos, destination.pos, needs_input, needs_output, finished};
}

void zstd_destroy(void* opaque) noexcept {
  auto& state = *static_cast<ZstdState*>(opaque);
  if (state.encoder != nullptr) (void)ZSTD_freeCCtx(state.encoder);
  if (state.decoder != nullptr) (void)ZSTD_freeDCtx(state.decoder);
  state.~ZstdState();
}

void zstd_cancel(void* opaque) noexcept {
  static_cast<ZstdState*>(opaque)->accounting.cancelled = true;
}

constexpr internal::CodecOperations zstd_operations{
    zstd_process, zstd_destroy, zstd_cancel};

}  // namespace

core::Result<CodecStream> create_zstd_codec(
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
  auto created = internal::construct_state<ZstdState>(state_storage);
  if (!created.has_value()) return std::unexpected{created.error()};
  ZstdState& state = **created;
  state.accounting = {limits, 0U, 0U, 0U, direction, false, false};
  state.memory.pool = &memory;
  state.log = log_sink;
  const ZSTD_customMem custom_memory{zstd_allocate, zstd_free, &state.memory};
  std::size_t result{};
  if (direction == CodecDirection::encode) {
    state.encoder = ZSTD_createCCtx_advanced(custom_memory);
    if (state.encoder != nullptr) result = ZSTD_CCtx_setParameter(
        state.encoder, ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT);
  } else {
    state.decoder = ZSTD_createDCtx_advanced(custom_memory);
    if (state.decoder != nullptr) result = ZSTD_DCtx_reset(
        state.decoder, ZSTD_reset_session_only);
  }
  if ((state.encoder == nullptr && state.decoder == nullptr) ||
      ZSTD_isError(result) != 0U) {
    if (state.encoder != nullptr) (void)ZSTD_freeCCtx(state.encoder);
    if (state.decoder != nullptr) (void)ZSTD_freeDCtx(state.decoder);
    state.~ZstdState();
    return std::unexpected{ZSTD_isError(result) != 0U
        ? zstd_error(core::DependencyOperation::codec_initialize,
                     result, log_sink)
        : zstd_allocation_error(core::DependencyOperation::codec_initialize,
                                log_sink)};
  }
  return CodecStream{&state, zstd_operations, arena, arena.generation(),
                     std::move(*pin)};
}

}  // namespace laghu::adapters
