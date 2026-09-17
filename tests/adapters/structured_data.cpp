// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "laghu_test_support.hpp"

#include <laghu/adapters/structured_data.hpp>
#include <laghu/core/contract.hpp>

namespace {

using laghu::adapters::JsonDocument;
using laghu::adapters::JsonDocumentLimits;
using laghu::adapters::JsonValueKind;
using laghu::core::ArenaBlockSource;
using laghu::core::BoundedArena;
using laghu::core::ByteView;
using laghu::core::Error;
using laghu::core::ErrorCode;
using laghu::core::MemoryBudget;
using laghu::core::MutableByteView;
using laghu::core::Result;
using laghu::core::WorkerId;

struct FixedBlockSource final {
  std::array<std::byte, 4096> storage{};
  bool fail_acquire{};
  std::size_t acquire_calls{};
  std::size_t reset_calls{};
};

[[nodiscard]] Result<MutableByteView> acquire_fixed_block(void* context,
                                                           std::size_t minimum_capacity) noexcept {
  auto& source = *static_cast<FixedBlockSource*>(context);
  ++source.acquire_calls;
  if (source.fail_acquire || minimum_capacity > source.storage.size()) {
    return std::unexpected{Error{laghu::core::ErrorDomain::core, ErrorCode::exhaustion, 0,
                                 "injected JSON arena allocation failure"}};
  }
  return MutableByteView::from(source.storage);
}

[[nodiscard]] Result<void> reset_fixed_block(void* context) noexcept {
  auto& source = *static_cast<FixedBlockSource*>(context);
  ++source.reset_calls;
  return {};
}

[[nodiscard]] constexpr ArenaBlockSource fixed_source(FixedBlockSource& source) noexcept {
  return ArenaBlockSource{&source, acquire_fixed_block, reset_fixed_block};
}

[[nodiscard]] constexpr JsonDocumentLimits normal_limits() noexcept {
  return JsonDocumentLimits{512, 8, 8, 64, 64};
}

[[nodiscard]] Result<ByteView> bytes(std::string_view input) noexcept {
  return ByteView::from(std::as_bytes(std::span{input.data(), input.size()}));
}

[[nodiscard]] bool reset_arena(BoundedArena& arena, WorkerId worker) noexcept {
  const auto boundary = arena.quiescent_boundary(worker);
  return boundary.has_value() && arena.reset(worker, *boundary).has_value();
}

[[nodiscard]] bool check_golden_document(WorkerId worker) noexcept {
  FixedBlockSource source{};
  MemoryBudget budget{worker, source.storage.size()};
  BoundedArena arena{worker, budget, fixed_source(source), 256, source.storage.size()};
  const auto input = bytes("{\"name\":\"laghu\",\"version\":1,\"features\":[true,null]}");
  if (!input.has_value()) {
    return false;
  }
  bool result{};
  {
    const auto document = JsonDocument::parse(worker, *input, arena, normal_limits());
    if (!document.has_value()) {
      return false;
    }
    const auto root = document->root();
    const auto name = root.has_value() ? root->object_member(laghu::core::TextView::from("name"))
                                       : Result<laghu::adapters::JsonValue>{
                                             std::unexpected{root.error()}};
    const auto version = root.has_value()
                             ? root->object_member(laghu::core::TextView::from("version"))
                             : Result<laghu::adapters::JsonValue>{
                                   std::unexpected{root.error()}};
    const auto features = root.has_value()
                              ? root->object_member(laghu::core::TextView::from("features"))
                              : Result<laghu::adapters::JsonValue>{
                                    std::unexpected{root.error()}};
    const auto first_feature = features.has_value() ? features->array_at(0)
                                                     : Result<laghu::adapters::JsonValue>{
                                                           std::unexpected{features.error()}};
    const auto second_feature = features.has_value() ? features->array_at(1)
                                                      : Result<laghu::adapters::JsonValue>{
                                                            std::unexpected{features.error()}};
    const auto name_text = name.has_value() ? name->text()
                                            : Result<laghu::core::TextView>{
                                                  std::unexpected{name.error()}};
    const auto version_value = version.has_value() ? version->unsigned_integer()
                                                    : Result<std::uint64_t>{
                                                          std::unexpected{version.error()}};
    const auto enabled = first_feature.has_value() ? first_feature->boolean()
                                                   : Result<bool>{
                                                         std::unexpected{first_feature.error()}};
    const auto null_kind = second_feature.has_value() ? second_feature->kind()
                                                      : Result<JsonValueKind>{
                                                            std::unexpected{second_feature.error()}};
    const auto size = root.has_value() ? root->object_size()
                                       : Result<std::size_t>{std::unexpected{root.error()}};
    const auto oversized_lookup =
        root.has_value()
            ? root->object_member(laghu::core::TextView::from(
                  "this-lookup-key-is-intentionally-longer-than-the-caller-string-limit"))
            : Result<laghu::adapters::JsonValue>{std::unexpected{root.error()}};
    result = root.has_value() && root->kind().has_value() && *root->kind() == JsonValueKind::object &&
             name_text.has_value() && name_text->string_view() == "laghu" &&
             version_value.has_value() && *version_value == 1U && enabled.has_value() && *enabled &&
             null_kind.has_value() && *null_kind == JsonValueKind::null && size.has_value() && *size == 3U &&
             !root->object_member(laghu::core::TextView::from("missing")).has_value() &&
             !oversized_lookup.has_value() &&
             oversized_lookup.error().code() == ErrorCode::invalid_range;
  }
  return result && reset_arena(arena, worker) && source.reset_calls == 1U;
}

[[nodiscard]] bool check_strict_rejections(WorkerId worker) noexcept {
  constexpr std::array<std::string_view, 5> inputs{
      "{\"key\":1,}", "{/* comment */\"key\":1}", "{\"key\":NaN}",
      "{\"key\":1} trailing", "\"\xC0\xAF\""};
  for (const std::string_view input_text : inputs) {
    FixedBlockSource source{};
    MemoryBudget budget{worker, source.storage.size()};
    BoundedArena arena{worker, budget, fixed_source(source), 256, source.storage.size()};
    const auto input = bytes(input_text);
    if (!input.has_value()) {
      return false;
    }
    const auto document = JsonDocument::parse(worker, *input, arena, normal_limits());
    if (document.has_value() || document.error().code() != ErrorCode::invalid_input ||
        document.error().dependency_id() != laghu::core::DependencyId::yyjson ||
        !reset_arena(arena, worker)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool check_caller_limits(WorkerId worker) noexcept {
  struct LimitCase final {
    std::string_view input;
    JsonDocumentLimits limits;
  };
  constexpr std::array cases{
      LimitCase{"[[1]]", JsonDocumentLimits{32, 2, 8, 64, 64}},
      LimitCase{"{\"one\":1,\"two\":2}", JsonDocumentLimits{64, 8, 1, 64, 64}},
      LimitCase{"{\"name\":\"long\"}", JsonDocumentLimits{64, 8, 8, 3, 64}},
      LimitCase{"{\"name\":1}", JsonDocumentLimits{4, 8, 8, 64, 64}},
      LimitCase{"[1,2]", JsonDocumentLimits{32, 8, 8, 64, 2}},
  };
  for (const LimitCase& test_case : cases) {
    FixedBlockSource source{};
    MemoryBudget budget{worker, source.storage.size()};
    BoundedArena arena{worker, budget, fixed_source(source), 256, source.storage.size()};
    const auto input = bytes(test_case.input);
    if (!input.has_value()) {
      return false;
    }
    const auto document = JsonDocument::parse(worker, *input, arena, test_case.limits);
    if (document.has_value() || document.error().code() != ErrorCode::invalid_range ||
        !reset_arena(arena, worker)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool check_duplicate_member_rejection(WorkerId worker) noexcept {
  FixedBlockSource source{};
  MemoryBudget budget{worker, source.storage.size()};
  BoundedArena arena{worker, budget, fixed_source(source), 256, source.storage.size()};
  const auto input = bytes("{\"duplicate\":1,\"duplicate\":2}");
  if (!input.has_value()) {
    return false;
  }
  const auto document = JsonDocument::parse(worker, *input, arena, normal_limits());
  return !document.has_value() && document.error().code() == ErrorCode::invalid_input &&
         document.error().domain() == laghu::core::ErrorDomain::core && reset_arena(arena, worker);
}

[[nodiscard]] bool check_allocation_failure(WorkerId worker) noexcept {
  FixedBlockSource source{};
  source.fail_acquire = true;
  MemoryBudget budget{worker, source.storage.size()};
  BoundedArena arena{worker, budget, fixed_source(source), 256, source.storage.size()};
  const auto input = bytes("{\"key\":1}");
  if (!input.has_value()) {
    return false;
  }
  const auto document = JsonDocument::parse(worker, *input, arena, normal_limits());
  return !document.has_value() && document.error().code() == ErrorCode::exhaustion &&
         source.acquire_calls == 1U && reset_arena(arena, worker);
}

[[nodiscard]] bool check_borrowed_value_lifetime(WorkerId worker) noexcept {
  FixedBlockSource source{};
  MemoryBudget budget{worker, source.storage.size()};
  BoundedArena arena{worker, budget, fixed_source(source), 256, source.storage.size()};
  const auto input = bytes("{\"key\":1}");
  if (!input.has_value()) {
    return false;
  }
  bool move_preserved_value{};
  bool destroyed_document_invalidated_value{};
  {
    auto document = JsonDocument::parse(worker, *input, arena, normal_limits());
    if (!document.has_value()) {
      return false;
    }
    const auto value = document->root();
    if (!value.has_value()) {
      return false;
    }
    {
      JsonDocument moved{std::move(*document)};
      move_preserved_value = value->kind().has_value();
    }
    destroyed_document_invalidated_value = !value->kind().has_value();
  }
  return move_preserved_value && destroyed_document_invalidated_value && reset_arena(arena, worker);
}

[[nodiscard]] bool test_golden_document() noexcept {
  const auto worker = WorkerId::from_uint64(58);
  return worker.has_value() && check_golden_document(*worker);
}

[[nodiscard]] bool test_strict_rejections() noexcept {
  const auto worker = WorkerId::from_uint64(58);
  return worker.has_value() && check_strict_rejections(*worker);
}

[[nodiscard]] bool test_caller_limits() noexcept {
  const auto worker = WorkerId::from_uint64(58);
  return worker.has_value() && check_caller_limits(*worker);
}

[[nodiscard]] bool test_duplicate_member_rejection() noexcept {
  const auto worker = WorkerId::from_uint64(58);
  return worker.has_value() && check_duplicate_member_rejection(*worker);
}

[[nodiscard]] bool test_allocation_failure() noexcept {
  const auto worker = WorkerId::from_uint64(58);
  return worker.has_value() && check_allocation_failure(*worker);
}

[[nodiscard]] bool test_borrowed_value_lifetime() noexcept {
  const auto worker = WorkerId::from_uint64(58);
  return worker.has_value() && check_borrowed_value_lifetime(*worker);
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"adapters.structured_data.golden", test_golden_document},
      laghu::test::TestCase{"adapters.structured_data.strict", test_strict_rejections},
      laghu::test::TestCase{"adapters.structured_data.limits", test_caller_limits},
      laghu::test::TestCase{"adapters.structured_data.duplicate_members",
                            test_duplicate_member_rejection},
      laghu::test::TestCase{"adapters.structured_data.allocation_failure", test_allocation_failure},
      laghu::test::TestCase{"adapters.structured_data.borrowed_value_lifetime",
                            test_borrowed_value_lifetime},
  };
  return laghu::test::run_tests(tests);
}
