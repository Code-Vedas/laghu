// SPDX-License-Identifier: AGPL-3.0-only
#include <cstddef>

#include <brotli/decode.h>
#include <brotli/encode.h>

#include <laghu/adapters/codecs.hpp>
#include <laghu/adapters/dependency.hpp>
#include <laghu/adapters/internal/arena_memory.hpp>
#include <laghu/adapters/internal/codecs.hpp>

namespace laghu::adapters {
namespace {

struct BrotliState final {
  internal::CodecAccounting accounting{};
  internal::ArenaMemory memory{};
  BrotliEncoderState* encoder{};
  BrotliDecoderState* decoder{};
  DependencyLogSink log{};
};

void* brotli_allocate(void* context, std::size_t size) noexcept {
  return internal::arena_malloc(size, context);
}

void brotli_free(void* context, void* pointer) noexcept {
  internal::arena_free(pointer, context);
}

[[nodiscard]] core::Error brotli_error(core::DependencyStatus status,
                                       core::DependencyOperation operation,
                                       int code,
                                       const DependencyLogSink& log) noexcept {
  const core::Error error = normalize_dependency_error(
      core::DependencyId::brotli, operation, status, code);
  log_dependency_error(log, error);
  return error;
}

[[nodiscard]] bool brotli_allocation_error(
    BrotliDecoderErrorCode code) noexcept {
  switch (code) {
    case BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES:
    case BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS:
    case BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP:
    case BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1:
    case BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2:
    case BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] core::Result<CodecProgress> brotli_process(
    void* opaque, core::ByteView input, core::MutableByteView output,
    bool finishing) noexcept {
  auto& state = *static_cast<BrotliState*>(opaque);
  if (const auto valid = internal::preflight(state.accounting, input.size());
      !valid.has_value()) return std::unexpected{valid.error()};
  if (output.empty() && internal::output_capacity_remains(state.accounting)) {
    internal::record(state.accounting, 0U, 0U, false);
    return CodecProgress{0U, 0U, false, true, false};
  }
  const std::size_t output_size = internal::bounded_output_size(
      state.accounting, output.size());
  if (output_size == 0U &&
      state.accounting.direction == CodecDirection::encode) {
    return std::unexpected{internal::codec_error(
        core::ErrorCode::exhaustion, "codec output limit is exhausted")};
  }
  std::size_t available_input = input.size();
  std::size_t available_output = output_size;
  const std::uint8_t* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
  std::uint8_t* next_output = reinterpret_cast<std::uint8_t*>(output.data());
  bool finished{};
  bool needs_input{};
  bool needs_output{};
  if (state.accounting.direction == CodecDirection::encode) {
    const BrotliEncoderOperation operation = finishing
        ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS;
    if (BrotliEncoderCompressStream(state.encoder, operation, &available_input,
                                    &next_input, &available_output,
                                    &next_output, nullptr) == BROTLI_FALSE) {
      internal::record_failure(state.accounting);
      return std::unexpected{brotli_error(
          core::DependencyStatus::exhaustion,
          core::DependencyOperation::codec_process, 0, state.log)};
    }
    finished = BrotliEncoderIsFinished(state.encoder) == BROTLI_TRUE;
    needs_output = !finished && (available_output == 0U ||
        BrotliEncoderHasMoreOutput(state.encoder) == BROTLI_TRUE);
    needs_input = !finished && !finishing && available_input == 0U && !needs_output;
  } else {
    const BrotliDecoderResult result = BrotliDecoderDecompressStream(
        state.decoder, &available_input, &next_input, &available_output,
        &next_output, nullptr);
    if (result == BROTLI_DECODER_RESULT_ERROR) {
      const BrotliDecoderErrorCode code =
          BrotliDecoderGetErrorCode(state.decoder);
      internal::record_failure(state.accounting);
      return std::unexpected{brotli_error(
          brotli_allocation_error(code)
              ? core::DependencyStatus::exhaustion
              : core::DependencyStatus::corrupt_data,
          core::DependencyOperation::codec_process,
          static_cast<int>(code), state.log)};
    }
    finished = result == BROTLI_DECODER_RESULT_SUCCESS;
    needs_input = result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT;
    needs_output = result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT;
    if (output_size == 0U && needs_output) {
      internal::record_failure(state.accounting);
      return std::unexpected{internal::codec_error(
          core::ErrorCode::exhaustion, "codec output limit is exhausted")};
    }
    if (finished && available_input != 0U) {
      internal::record_failure(state.accounting);
      return std::unexpected{internal::codec_error(
          core::ErrorCode::corrupt_data, "compressed stream has trailing data")};
    }
    if (finishing && needs_input) {
      internal::record_failure(state.accounting);
      return std::unexpected{internal::codec_error(
          core::ErrorCode::corrupt_data, "compressed stream is truncated")};
    }
  }
  const std::size_t consumed = input.size() - available_input;
  const std::size_t produced = output_size - available_output;
  internal::record(state.accounting, consumed, produced, finished);
  return CodecProgress{consumed, produced, needs_input, needs_output, finished};
}

void brotli_destroy(void* opaque) noexcept {
  auto& state = *static_cast<BrotliState*>(opaque);
  if (state.encoder != nullptr) BrotliEncoderDestroyInstance(state.encoder);
  if (state.decoder != nullptr) BrotliDecoderDestroyInstance(state.decoder);
  state.~BrotliState();
}

void brotli_cancel(void* opaque) noexcept {
  static_cast<BrotliState*>(opaque)->accounting.cancelled = true;
}

constexpr internal::CodecOperations brotli_operations{
    brotli_process, brotli_destroy, brotli_cancel};

}  // namespace

core::Result<CodecStream> create_brotli_codec(
    CodecDirection direction, CodecStateStorage state_storage,
    NativeMemoryPool& memory, CodecLimits limits,
    DependencyLogSink log_sink) noexcept {
  if (!internal::valid_direction(direction)) {
    return std::unexpected{internal::codec_error(
        core::ErrorCode::invalid_input, "codec direction is invalid")};
  }
  if (!internal::valid_limits(limits)) {
    return std::unexpected{internal::codec_error(
        core::ErrorCode::invalid_input, "codec limits must be positive")};
  }
  internal::ArenaMemoryAccess::synchronize(memory);
  auto& arena = internal::ArenaMemoryAccess::arena(memory);
  const auto worker = internal::ArenaMemoryAccess::worker(memory);
  auto pin = arena.pin(worker);
  if (!pin.has_value()) return std::unexpected{pin.error()};
  auto created = internal::construct_state<BrotliState>(state_storage);
  if (!created.has_value()) return std::unexpected{created.error()};
  BrotliState& state = **created;
  state.accounting = {limits, 0U, 0U, 0U, direction, false, false, false};
  state.memory.pool = &memory;
  state.log = log_sink;
  if (direction == CodecDirection::encode) {
    state.encoder = BrotliEncoderCreateInstance(
        brotli_allocate, brotli_free, &state.memory);
    if (state.encoder != nullptr &&
        BrotliEncoderSetParameter(state.encoder, BROTLI_PARAM_QUALITY, 4U) == BROTLI_FALSE) {
      BrotliEncoderDestroyInstance(state.encoder);
      state.encoder = nullptr;
    }
  } else {
    state.decoder = BrotliDecoderCreateInstance(
        brotli_allocate, brotli_free, &state.memory);
  }
  if (state.encoder == nullptr && state.decoder == nullptr) {
    state.~BrotliState();
    return std::unexpected{brotli_error(
        core::DependencyStatus::exhaustion,
        core::DependencyOperation::codec_initialize, 0, log_sink)};
  }
  return CodecStream{&state, brotli_operations, arena, arena.generation(),
                     std::move(*pin)};
}

}  // namespace laghu::adapters
