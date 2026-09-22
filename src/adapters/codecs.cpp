// SPDX-License-Identifier: AGPL-3.0-only
#include <utility>

#include <laghu/adapters/codecs.hpp>
#include <laghu/adapters/internal/codecs.hpp>

namespace laghu::adapters {

CodecStream::CodecStream(CodecStream&& other) noexcept { move_from(std::move(other)); }

CodecStream& CodecStream::operator=(CodecStream&& other) noexcept {
  if (this != &other) {
    release();
    move_from(std::move(other));
  }
  return *this;
}

CodecStream::~CodecStream() { release(); }

core::Result<void> CodecStream::require_valid() const noexcept {
  if (state_ == nullptr || operations_ == nullptr || arena_ == nullptr ||
      arena_->generation() != generation_) {
    return std::unexpected{internal::codec_error(
        core::ErrorCode::invalid_state,
        "codec stream is inactive or its arena was reset")};
  }
  return {};
}

core::Result<CodecProgress> CodecStream::process(
    core::ByteView input, core::MutableByteView output) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  if (finalizing_) {
    return std::unexpected{internal::codec_error(
        core::ErrorCode::invalid_state,
        "codec stream finalization has already started")};
  }
  return operations_->process(state_, input, output, false);
}

core::Result<CodecProgress> CodecStream::finish(
    core::MutableByteView output) noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  finalizing_ = true;
  return operations_->process(state_, core::ByteView{}, output, true);
}

void CodecStream::cancel() noexcept {
  if (state_ != nullptr && operations_ != nullptr && arena_ != nullptr &&
      arena_->generation() == generation_) {
    operations_->cancel(state_);
  }
}

void CodecStream::release() noexcept {
  if (state_ != nullptr && operations_ != nullptr && arena_ != nullptr &&
      arena_->generation() == generation_) {
    operations_->destroy(state_);
  }
  state_ = nullptr;
  operations_ = nullptr;
  arena_ = nullptr;
  generation_ = 0U;
  pin_ = {};
  finalizing_ = false;
}

void CodecStream::move_from(CodecStream&& other) noexcept {
  state_ = std::exchange(other.state_, nullptr);
  operations_ = std::exchange(other.operations_, nullptr);
  arena_ = std::exchange(other.arena_, nullptr);
  generation_ = std::exchange(other.generation_, 0U);
  pin_ = std::move(other.pin_);
  finalizing_ = std::exchange(other.finalizing_, false);
}

}  // namespace laghu::adapters
