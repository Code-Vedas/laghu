// SPDX-License-Identifier: AGPL-3.0-only
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

#include <yyjson.h>

#include <laghu/adapters/structured_data.hpp>

namespace laghu::adapters {
namespace {

constexpr std::size_t yyjson_pool_minimum_bytes = 8U * sizeof(void*);

enum class TraversalState : std::uint8_t {
  enter,
  array,
  object,
};

struct TraversalFrame final {
  yyjson_val* value{};
  std::size_t depth{};
  TraversalState state{TraversalState::enter};
  yyjson_arr_iter array_iterator{};
  yyjson_obj_iter object_iterator{};
};

struct DocumentState final {
  bool active{};
  std::uint64_t epoch{1};
};

[[nodiscard]] constexpr core::Error core_error(core::ErrorCode code,
                                                std::string_view diagnostic) noexcept {
  return core::Error{core::ErrorDomain::core, code, 0, diagnostic};
}

[[nodiscard]] core::Error yyjson_error(core::DependencyStatus status,
                                       std::int32_t native_code,
                                       const DependencyLogSink& log_sink) noexcept {
  const core::Error error = normalize_dependency_error(
      core::DependencyId::yyjson, core::DependencyOperation::json_parse, status, native_code);
  log_dependency_error(log_sink, error);
  return error;
}

[[nodiscard]] core::Result<void> validate_limits(const JsonDocumentLimits& limits) noexcept {
  if (limits.maximum_document_bytes == 0 || limits.maximum_nesting == 0 ||
      limits.maximum_object_members == 0 || limits.maximum_string_bytes == 0 ||
      limits.maximum_aggregate_values == 0) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "JSON document limits must all be positive")};
  }
  if (limits.maximum_nesting > std::numeric_limits<std::size_t>::max() /
                                   sizeof(TraversalFrame)) {
    return std::unexpected{core_error(core::ErrorCode::overflow,
                                      "JSON nesting limit overflows traversal storage")};
  }
  return {};
}

[[nodiscard]] bool equal_key(const yyjson_val* left, const yyjson_val* right) noexcept {
  const std::size_t left_size = yyjson_get_len(left);
  const std::size_t right_size = yyjson_get_len(right);
  if (left_size != right_size) {
    return false;
  }
  const char* const left_characters = yyjson_get_str(left);
  const char* const right_characters = yyjson_get_str(right);
  if (left_characters == nullptr || right_characters == nullptr) {
    return false;
  }
  for (std::size_t index = 0; index < left_size; ++index) {
    if (left_characters[index] != right_characters[index]) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] core::Result<void> validate_object(
    yyjson_val* object, const JsonDocumentLimits& limits) noexcept {
  if (yyjson_obj_size(object) > limits.maximum_object_members) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "JSON object exceeds the caller member limit")};
  }
  yyjson_obj_iter outer = yyjson_obj_iter_with(object);
  while (yyjson_val* const key = yyjson_obj_iter_next(&outer)) {
    const char* const characters = yyjson_get_str(key);
    if (characters == nullptr || yyjson_get_len(key) > limits.maximum_string_bytes) {
      return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                        "JSON object key exceeds the caller string limit")};
    }
    std::size_t matches{};
    yyjson_obj_iter inner = yyjson_obj_iter_with(object);
    while (yyjson_val* const candidate = yyjson_obj_iter_next(&inner)) {
      if (equal_key(key, candidate)) {
        ++matches;
      }
    }
    if (matches != 1U) {
      return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                        "JSON object contains duplicate members")};
    }
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_document(
    yyjson_doc* document, TraversalFrame* frames, std::size_t frame_capacity,
    const JsonDocumentLimits& limits) noexcept {
  if (yyjson_doc_get_val_count(document) > limits.maximum_aggregate_values) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "JSON document exceeds the caller aggregate limit")};
  }
  yyjson_val* const root = yyjson_doc_get_root(document);
  if (root == nullptr || frames == nullptr || frame_capacity == 0) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "yyjson returned an invalid document")};
  }

  std::size_t frame_count = 1;
  frames[0] = TraversalFrame{root, 1U, TraversalState::enter, {}, {}};
  while (frame_count != 0) {
    TraversalFrame& frame = frames[frame_count - 1U];
    if (frame.depth > limits.maximum_nesting) {
      return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                        "JSON document exceeds the caller nesting limit")};
    }
    if (frame.state == TraversalState::enter) {
      if (yyjson_is_str(frame.value) && yyjson_get_len(frame.value) > limits.maximum_string_bytes) {
        return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                          "JSON string exceeds the caller string limit")};
      }
      if (yyjson_is_arr(frame.value)) {
        frame.array_iterator = yyjson_arr_iter_with(frame.value);
        frame.state = TraversalState::array;
        continue;
      }
      if (yyjson_is_obj(frame.value)) {
        if (const auto valid = validate_object(frame.value, limits); !valid.has_value()) {
          return std::unexpected{valid.error()};
        }
        frame.object_iterator = yyjson_obj_iter_with(frame.value);
        frame.state = TraversalState::object;
        continue;
      }
      --frame_count;
      continue;
    }

    yyjson_val* child{};
    if (frame.state == TraversalState::array) {
      child = yyjson_arr_iter_next(&frame.array_iterator);
    } else {
      yyjson_val* const key = yyjson_obj_iter_next(&frame.object_iterator);
      child = key == nullptr ? nullptr : yyjson_obj_iter_get_val(key);
    }
    if (child == nullptr) {
      --frame_count;
      continue;
    }
    if (frame_count == frame_capacity) {
      return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                        "JSON document exceeds the caller nesting limit")};
    }
    frames[frame_count] = TraversalFrame{child, frame.depth + 1U, TraversalState::enter, {}, {}};
    ++frame_count;
  }
  return {};
}

[[nodiscard]] core::Result<JsonValueKind> value_kind(const yyjson_val* value) noexcept {
  if (yyjson_is_null(value)) {
    return JsonValueKind::null;
  }
  if (yyjson_is_bool(value)) {
    return JsonValueKind::boolean;
  }
  if (yyjson_is_uint(value)) {
    return JsonValueKind::unsigned_integer;
  }
  if (yyjson_is_sint(value)) {
    return JsonValueKind::signed_integer;
  }
  if (yyjson_is_real(value)) {
    return JsonValueKind::real;
  }
  if (yyjson_is_str(value)) {
    return JsonValueKind::text;
  }
  if (yyjson_is_arr(value)) {
    return JsonValueKind::array;
  }
  if (yyjson_is_obj(value)) {
    return JsonValueKind::object;
  }
  return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                    "yyjson returned an unsupported value kind")};
}

[[nodiscard]] core::Result<void> require_kind(const yyjson_val* value,
                                               JsonValueKind expected) noexcept {
  const auto actual = value_kind(value);
  if (!actual.has_value()) {
    return std::unexpected{actual.error()};
  }
  if (*actual != expected) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "JSON value does not have the requested kind")};
  }
  return {};
}

}  // namespace

JsonDocument::JsonDocument(JsonDocument&& other) noexcept { move_from(std::move(other)); }

JsonDocument& JsonDocument::operator=(JsonDocument&& other) noexcept {
  if (this != &other) {
    release();
    move_from(std::move(other));
  }
  return *this;
}

JsonDocument::~JsonDocument() { release(); }

core::Result<JsonDocument> JsonDocument::parse(core::WorkerId worker, core::ByteView document_bytes,
                                                core::BoundedArena& arena,
                                                JsonDocumentLimits limits,
                                                DependencyLogSink log_sink) noexcept {
  if (const auto valid_limits = validate_limits(limits); !valid_limits.has_value()) {
    return std::unexpected{valid_limits.error()};
  }
  if (document_bytes.empty()) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "JSON document must not be empty")};
  }
  if (document_bytes.size() > limits.maximum_document_bytes) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "JSON document exceeds the caller byte limit")};
  }
  if (arena.used() > arena.maximum_capacity()) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "bounded arena usage exceeds its capacity")};
  }

  // yyjson's read API takes mutable input. Preserve the caller's ByteView and
  // keep the parser copy inside the same bounded arena as all parser state.
  const auto input_storage =
      arena.try_allocate(worker, document_bytes.size(), alignof(char));
  if (!input_storage.has_value()) {
    return std::unexpected{input_storage.error()};
  }
  const auto input_bytes = input_storage->bytes();
  if (!input_bytes.has_value()) {
    return std::unexpected{input_bytes.error()};
  }
  std::memcpy(input_bytes->data(), document_bytes.data(), document_bytes.size());

  const auto state_storage =
      arena.try_allocate(worker, sizeof(DocumentState), alignof(DocumentState));
  if (!state_storage.has_value()) {
    return std::unexpected{state_storage.error()};
  }
  const auto state_bytes = state_storage->bytes();
  if (!state_bytes.has_value()) {
    return std::unexpected{state_bytes.error()};
  }
  auto* const document_state =
      std::construct_at(reinterpret_cast<DocumentState*>(state_bytes->data()));

  const std::size_t frame_bytes = limits.maximum_nesting * sizeof(TraversalFrame);
  const auto frame_storage = arena.try_allocate(worker, frame_bytes, alignof(TraversalFrame));
  if (!frame_storage.has_value()) {
    return std::unexpected{frame_storage.error()};
  }
  const auto frame_bytes_view = frame_storage->bytes();
  if (!frame_bytes_view.has_value()) {
    return std::unexpected{frame_bytes_view.error()};
  }
  auto* const frames = reinterpret_cast<TraversalFrame*>(frame_bytes_view->data());

  const std::size_t remaining_capacity = arena.maximum_capacity() - arena.used();
  if (remaining_capacity < yyjson_pool_minimum_bytes) {
    return std::unexpected{core_error(core::ErrorCode::exhaustion,
                                      "bounded arena cannot hold a yyjson allocation pool")};
  }
  const auto pool_storage =
      arena.try_allocate(worker, remaining_capacity, alignof(std::max_align_t));
  if (!pool_storage.has_value()) {
    return std::unexpected{pool_storage.error()};
  }
  const auto pool_bytes = pool_storage->bytes();
  if (!pool_bytes.has_value()) {
    return std::unexpected{pool_bytes.error()};
  }
  yyjson_alc allocator{};
  if (!yyjson_alc_pool_init(&allocator, pool_bytes->data(), pool_bytes->size())) {
    return std::unexpected{core_error(core::ErrorCode::exhaustion,
                                      "bounded arena cannot initialize a yyjson allocation pool")};
  }

  yyjson_read_err read_error{};
  auto* const mutable_input = reinterpret_cast<char*>(input_bytes->data());
  yyjson_doc* const native_document = yyjson_read_opts(
      mutable_input, document_bytes.size(), YYJSON_READ_NOFLAG, &allocator, &read_error);
  if (native_document == nullptr) {
    return std::unexpected{yyjson_error(core::DependencyStatus::invalid_input,
                                        static_cast<std::int32_t>(read_error.code), log_sink)};
  }
  if (const auto valid_document = validate_document(native_document, frames,
                                                    limits.maximum_nesting, limits);
      !valid_document.has_value()) {
    yyjson_doc_free(native_document);
    return std::unexpected{valid_document.error()};
  }
  document_state->active = true;
  JsonDocument result{native_document, arena, pool_storage->generation(),
                      limits.maximum_string_bytes};
  result.document_state_ = document_state;
  return result;
}

core::Result<JsonValue> JsonDocument::root() const noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  yyjson_val* const native_root = yyjson_doc_get_root(static_cast<yyjson_doc*>(native_document_));
  if (native_root == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "yyjson document has no root value")};
  }
  const auto* const document_state = static_cast<const DocumentState*>(document_state_);
  return JsonValue{*arena_, document_state, native_root, document_state->epoch, arena_generation_,
                   maximum_lookup_key_bytes_};
}

core::Result<void> JsonDocument::require_valid() const noexcept {
  if (native_document_ == nullptr || arena_ == nullptr || document_state_ == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "JSON document is no longer valid")};
  }
  if (arena_->generation() != arena_generation_) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "JSON document was invalidated by an arena reset")};
  }
  const auto* const document_state = static_cast<const DocumentState*>(document_state_);
  if (!document_state->active) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "JSON document is no longer valid")};
  }
  return {};
}

void JsonDocument::release() noexcept {
  const bool arena_is_current =
      arena_ != nullptr && arena_->generation() == arena_generation_;
  if (native_document_ != nullptr && arena_is_current) {
    yyjson_doc_free(static_cast<yyjson_doc*>(native_document_));
  }
  native_document_ = nullptr;
  if (document_state_ != nullptr && arena_is_current) {
    auto* const document_state = static_cast<DocumentState*>(document_state_);
    document_state->active = false;
    if (document_state->epoch != std::numeric_limits<std::uint64_t>::max()) {
      ++document_state->epoch;
    }
  }
  arena_ = nullptr;
  document_state_ = nullptr;
  arena_generation_ = 0;
  maximum_lookup_key_bytes_ = 0;
}

void JsonDocument::move_from(JsonDocument&& other) noexcept {
  native_document_ = other.native_document_;
  arena_ = other.arena_;
  document_state_ = other.document_state_;
  arena_generation_ = other.arena_generation_;
  maximum_lookup_key_bytes_ = other.maximum_lookup_key_bytes_;
  other.native_document_ = nullptr;
  other.arena_ = nullptr;
  other.document_state_ = nullptr;
  other.arena_generation_ = 0;
  other.maximum_lookup_key_bytes_ = 0;
}

core::Result<void> JsonValue::require_valid() const noexcept {
  if (arena_ == nullptr || document_state_ == nullptr || native_value_ == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "JSON value is no longer valid")};
  }
  if (arena_->generation() != arena_generation_) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "JSON value was invalidated by an arena reset")};
  }
  const auto* const document_state = static_cast<const DocumentState*>(document_state_);
  if (!document_state->active || document_state->epoch != document_epoch_) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "JSON value is no longer valid")};
  }
  return {};
}

core::Result<JsonValueKind> JsonValue::kind() const noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  return value_kind(static_cast<const yyjson_val*>(native_value_));
}

core::Result<bool> JsonValue::boolean() const noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  const auto* const value = static_cast<const yyjson_val*>(native_value_);
  if (const auto kind = require_kind(value, JsonValueKind::boolean); !kind.has_value()) {
    return std::unexpected{kind.error()};
  }
  return yyjson_get_bool(value);
}

core::Result<std::uint64_t> JsonValue::unsigned_integer() const noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  const auto* const value = static_cast<const yyjson_val*>(native_value_);
  if (const auto kind = require_kind(value, JsonValueKind::unsigned_integer); !kind.has_value()) {
    return std::unexpected{kind.error()};
  }
  return yyjson_get_uint(value);
}

core::Result<std::int64_t> JsonValue::signed_integer() const noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  const auto* const value = static_cast<const yyjson_val*>(native_value_);
  if (const auto kind = require_kind(value, JsonValueKind::signed_integer); !kind.has_value()) {
    return std::unexpected{kind.error()};
  }
  return yyjson_get_sint(value);
}

core::Result<double> JsonValue::real() const noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  const auto* const value = static_cast<const yyjson_val*>(native_value_);
  if (const auto kind = require_kind(value, JsonValueKind::real); !kind.has_value()) {
    return std::unexpected{kind.error()};
  }
  return yyjson_get_real(value);
}

core::Result<core::TextView> JsonValue::text() const noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  const auto* const value = static_cast<const yyjson_val*>(native_value_);
  if (const auto kind = require_kind(value, JsonValueKind::text); !kind.has_value()) {
    return std::unexpected{kind.error()};
  }
  const char* const characters = yyjson_get_str(value);
  if (characters == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_state,
                                      "yyjson string has no character data")};
  }
  return core::TextView::from(characters, yyjson_get_len(value));
}

core::Result<std::size_t> JsonValue::array_size() const noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  const auto* const value = static_cast<const yyjson_val*>(native_value_);
  if (const auto kind = require_kind(value, JsonValueKind::array); !kind.has_value()) {
    return std::unexpected{kind.error()};
  }
  return yyjson_arr_size(value);
}

core::Result<std::size_t> JsonValue::object_size() const noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  const auto* const value = static_cast<const yyjson_val*>(native_value_);
  if (const auto kind = require_kind(value, JsonValueKind::object); !kind.has_value()) {
    return std::unexpected{kind.error()};
  }
  return yyjson_obj_size(value);
}

core::Result<JsonValue> JsonValue::array_at(std::size_t index) const noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  const auto* const value = static_cast<const yyjson_val*>(native_value_);
  if (const auto kind = require_kind(value, JsonValueKind::array); !kind.has_value()) {
    return std::unexpected{kind.error()};
  }
  yyjson_val* const child = yyjson_arr_get(value, index);
  if (child == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "JSON array index is outside the array")};
  }
  return JsonValue{*arena_, document_state_, child, document_epoch_, arena_generation_,
                   maximum_lookup_key_bytes_};
}

core::Result<JsonValue> JsonValue::object_member(core::TextView key) const noexcept {
  if (const auto valid = require_valid(); !valid.has_value()) {
    return std::unexpected{valid.error()};
  }
  const auto* const value = static_cast<const yyjson_val*>(native_value_);
  if (const auto kind = require_kind(value, JsonValueKind::object); !kind.has_value()) {
    return std::unexpected{kind.error()};
  }
  if (key.size() > maximum_lookup_key_bytes_) {
    return std::unexpected{core_error(core::ErrorCode::invalid_range,
                                      "JSON lookup key exceeds the caller string limit")};
  }
  const char* const key_characters = key.empty() ? "" : key.data();
  yyjson_val* const child = yyjson_obj_getn(value, key_characters, key.size());
  if (child == nullptr) {
    return std::unexpected{core_error(core::ErrorCode::invalid_input,
                                      "JSON object does not contain the requested member")};
  }
  return JsonValue{*arena_, document_state_, child, document_epoch_, arena_generation_,
                   maximum_lookup_key_bytes_};
}

}  // namespace laghu::adapters
