// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <laghu/adapters/structured_data.hpp>
#include <laghu/core/contract.hpp>

namespace {

struct FuzzBlockSource final {
  std::array<std::byte, 65536> storage{};
};

[[nodiscard]] laghu::core::Result<laghu::core::MutableByteView> acquire(void* context,
                                                                          std::size_t minimum) noexcept {
  auto& source = *static_cast<FuzzBlockSource*>(context);
  if (minimum > source.storage.size()) {
    return std::unexpected{laghu::core::Error{laghu::core::ErrorDomain::core,
                                               laghu::core::ErrorCode::exhaustion, 0,
                                               "fuzz JSON block source is exhausted"}};
  }
  return laghu::core::MutableByteView::from(source.storage);
}

[[nodiscard]] laghu::core::Result<void> reset(void*) noexcept { return {}; }

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto worker = laghu::core::WorkerId::from_uint64(58);
  if (!worker.has_value()) {
    return 0;
  }
  const auto input = laghu::core::ByteView::from(
      std::span<const std::byte>{reinterpret_cast<const std::byte*>(data), size});
  if (!input.has_value()) {
    return 0;
  }
  FuzzBlockSource source{};
  laghu::core::MemoryBudget budget{*worker, source.storage.size()};
  laghu::core::BoundedArena arena{*worker, budget, {&source, acquire, reset}, 4096,
                                  source.storage.size()};
  {
    constexpr laghu::adapters::JsonDocumentLimits limits{32768, 64, 256, 4096, 4096};
    const auto document = laghu::adapters::JsonDocument::parse(*worker, *input, arena, limits);
    if (document.has_value()) {
      static_cast<void>(document->root());
    }
  }
  const auto boundary = arena.quiescent_boundary(*worker);
  if (boundary.has_value()) {
    static_cast<void>(arena.reset(*worker, *boundary));
  }
  return 0;
}
