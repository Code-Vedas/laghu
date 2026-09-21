// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <laghu/adapters/codecs.hpp>

namespace laghu::adapters::internal {

struct CodecOperations final {
  core::Result<CodecProgress> (*process)(void*, core::ByteView,
                                         core::MutableByteView, bool) noexcept;
  void (*destroy)(void*) noexcept;
  void (*cancel)(void*) noexcept;
};

struct CodecAccounting final {
  CodecLimits limits{};
  std::size_t input_bytes{};
  std::size_t output_bytes{};
  std::uint32_t work_calls{};
  CodecDirection direction{};
  bool finished{};
  bool cancelled{};
  bool failed{};
};

[[nodiscard]] inline core::Error codec_error(core::ErrorCode code,
                                              const char* diagnostic) noexcept {
  return {core::ErrorDomain::core, code, 0, diagnostic};
}

[[nodiscard]] inline bool valid_limits(const CodecLimits& limits) noexcept {
  return limits.maximum_input_bytes != 0U && limits.maximum_output_bytes != 0U &&
         limits.maximum_work_calls != 0U;
}

[[nodiscard]] inline bool valid_direction(CodecDirection direction) noexcept {
  return direction == CodecDirection::encode ||
         direction == CodecDirection::decode;
}

[[nodiscard]] inline core::Result<void> preflight(
    const CodecAccounting& accounting, std::size_t input_size) noexcept {
  if (accounting.cancelled || accounting.finished || accounting.failed) {
    return std::unexpected{codec_error(core::ErrorCode::invalid_state,
                                      "codec stream is no longer active")};
  }
  if (accounting.work_calls >= accounting.limits.maximum_work_calls) {
    return std::unexpected{codec_error(core::ErrorCode::exhaustion,
                                      "codec work limit is exhausted")};
  }
  if (input_size > accounting.limits.maximum_input_bytes - accounting.input_bytes) {
    return std::unexpected{codec_error(core::ErrorCode::invalid_range,
                                      "codec input exceeds the caller limit")};
  }
  return {};
}

inline void record_failure(CodecAccounting& accounting) noexcept {
  ++accounting.work_calls;
  accounting.failed = true;
}

[[nodiscard]] inline std::size_t bounded_output_size(
    const CodecAccounting& accounting, std::size_t offered) noexcept {
  return std::min(offered,
                  accounting.limits.maximum_output_bytes - accounting.output_bytes);
}

inline void record(CodecAccounting& accounting, std::size_t consumed,
                   std::size_t produced, bool finished) noexcept {
  accounting.input_bytes += consumed;
  accounting.output_bytes += produced;
  ++accounting.work_calls;
  accounting.finished = finished;
}

template <class State>
[[nodiscard]] inline core::Result<State*> construct_state(
    CodecStateStorage storage) noexcept {
  static_assert(sizeof(State) <= codec_state_storage_words * sizeof(std::max_align_t));
  static_assert(alignof(State) <= alignof(std::max_align_t));
  if (storage.data() == nullptr) {
    return std::unexpected{codec_error(core::ErrorCode::invalid_input,
                                      "codec state storage is unavailable")};
  }
  return ::new (static_cast<void*>(storage.data())) State{};
}

}  // namespace laghu::adapters::internal
