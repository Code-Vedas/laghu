// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <laghu/adapters/dependency.hpp>
#include <laghu/core/bounded_arena.hpp>
#include <laghu/core/views.hpp>

namespace laghu::adapters {

// Every bound is caller-selected. `maximum_aggregate_values` counts all parsed
// values, including object keys, so one limit bounds the complete document
// graph rather than only its root container.
struct JsonDocumentLimits final {
  std::size_t maximum_document_bytes{};
  std::size_t maximum_nesting{};
  std::size_t maximum_object_members{};
  std::size_t maximum_string_bytes{};
  std::size_t maximum_aggregate_values{};
};

enum class JsonValueKind : std::uint8_t {
  null,
  boolean,
  unsigned_integer,
  signed_integer,
  real,
  text,
  array,
  object,
};

class JsonDocument;

// A JsonValue is a borrowed view of a JsonDocument. It becomes invalid when
// its document is destroyed or when the document arena is reset.
class JsonValue final {
 public:
  [[nodiscard]] core::Result<JsonValueKind> kind() const noexcept;
  [[nodiscard]] core::Result<bool> boolean() const noexcept;
  [[nodiscard]] core::Result<std::uint64_t> unsigned_integer() const noexcept;
  [[nodiscard]] core::Result<std::int64_t> signed_integer() const noexcept;
  [[nodiscard]] core::Result<double> real() const noexcept;
  [[nodiscard]] core::Result<core::TextView> text() const noexcept;
  [[nodiscard]] core::Result<std::size_t> array_size() const noexcept;
  [[nodiscard]] core::Result<std::size_t> object_size() const noexcept;
  [[nodiscard]] core::Result<JsonValue> array_at(std::size_t index) const noexcept;
  [[nodiscard]] core::Result<JsonValue> object_member(core::TextView key) const noexcept;

 private:
  friend class JsonDocument;

  constexpr JsonValue(const core::BoundedArena& arena, const void* document_state,
                      const void* native_value, std::uint64_t document_epoch,
                      std::uint64_t arena_generation,
                      std::size_t maximum_lookup_key_bytes) noexcept
      : arena_(&arena), document_state_(document_state), native_value_(native_value),
        document_epoch_(document_epoch), arena_generation_(arena_generation),
        maximum_lookup_key_bytes_(maximum_lookup_key_bytes) {}

  [[nodiscard]] core::Result<void> require_valid() const noexcept;

  const core::BoundedArena* arena_{};
  const void* document_state_{};
  const void* native_value_{};
  std::uint64_t document_epoch_{};
  std::uint64_t arena_generation_{};
  std::size_t maximum_lookup_key_bytes_{};
};

// The parser uses only the caller's BoundedArena. The document and every
// JsonValue borrowed from it must be destroyed before that arena is reset.
class JsonDocument final {
 public:
  JsonDocument(const JsonDocument&) = delete;
  JsonDocument& operator=(const JsonDocument&) = delete;
  JsonDocument(JsonDocument&& other) noexcept;
  JsonDocument& operator=(JsonDocument&& other) noexcept;
  ~JsonDocument();

  [[nodiscard]] static core::Result<JsonDocument> parse(
      core::WorkerId worker, core::ByteView document_bytes, core::BoundedArena& arena,
      JsonDocumentLimits limits, DependencyLogSink log_sink = {}) noexcept;

  [[nodiscard]] core::Result<JsonValue> root() const noexcept;

 private:
  friend class JsonValue;

  constexpr JsonDocument(void* native_document, const core::BoundedArena& arena,
                         std::uint64_t arena_generation,
                         std::size_t maximum_lookup_key_bytes) noexcept
      : native_document_(native_document), arena_(&arena), arena_generation_(arena_generation),
        maximum_lookup_key_bytes_(maximum_lookup_key_bytes) {}

  [[nodiscard]] core::Result<void> require_valid() const noexcept;
  void release() noexcept;
  void move_from(JsonDocument&& other) noexcept;

  void* native_document_{};
  const core::BoundedArena* arena_{};
  void* document_state_{};
  std::uint64_t arena_generation_{};
  std::size_t maximum_lookup_key_bytes_{};
};

static_assert(std::is_trivially_copyable_v<JsonDocumentLimits>);
static_assert(std::is_standard_layout_v<JsonDocumentLimits>);
static_assert(std::is_trivially_copyable_v<JsonValue>);
static_assert(std::is_standard_layout_v<JsonValue>);
static_assert(!std::is_copy_constructible_v<JsonDocument>);
static_assert(!std::is_copy_assignable_v<JsonDocument>);

}  // namespace laghu::adapters
