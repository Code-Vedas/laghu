// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <expected>
#include <string_view>
#include <type_traits>

#include <laghu/core/contract.hpp>

std::size_t allocation_attempts{};

void* operator new(std::size_t size) {
  ++allocation_attempts;
  if (void* memory = std::malloc(size); memory != nullptr) {
    return memory;
  }
  std::abort();
}

void* operator new[](std::size_t size) {
  ++allocation_attempts;
  if (void* memory = std::malloc(size); memory != nullptr) {
    return memory;
  }
  std::abort();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

class MoveOnly final {
 public:
  constexpr explicit MoveOnly(int value) noexcept : value_(value) {}
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly& operator=(const MoveOnly&) = delete;
  constexpr MoveOnly(MoveOnly&&) noexcept = default;
  constexpr MoveOnly& operator=(MoveOnly&&) noexcept = default;

  [[nodiscard]] constexpr int value() const noexcept { return value_; }

 private:
  int value_;
};

[[nodiscard]] constexpr laghu::core::Result<MoveOnly> move_only_success() noexcept {
  return MoveOnly{47};
}

[[nodiscard]] bool check(bool condition) noexcept { return condition; }

static_assert(std::is_same_v<laghu::core::Result<int>,
                             std::expected<int, laghu::core::Error>>);
static_assert(!std::is_copy_constructible_v<laghu::core::Result<MoveOnly>>);
static_assert(std::is_move_constructible_v<laghu::core::Result<MoveOnly>>);
static_assert(std::is_trivially_copyable_v<laghu::core::Error>);
static_assert(std::is_standard_layout_v<laghu::core::Error>);
static_assert(sizeof(std::array<char, laghu::core::Error::diagnostic_context_capacity>) == 96);
static_assert(!std::is_convertible_v<int, laghu::core::Error>);
static_assert(noexcept(laghu::core::Error::from_errno(0)));
static_assert(noexcept(laghu::core::Error::from_dependency(
    laghu::core::DependencyStatus::unknown)));

}  // namespace

int main() {
  const std::size_t allocation_attempts_before = allocation_attempts;
  const auto success = move_only_success();
  if (!check(success.has_value() && success->value() == 47)) {
    return 1;
  }

  const auto invalid_input = laghu::core::Error::from_errno(EINVAL, "invalid input");
  if (!check(invalid_input.domain() == laghu::core::ErrorDomain::posix &&
             invalid_input.code() == laghu::core::ErrorCode::invalid_input &&
             invalid_input.native_code() == EINVAL &&
             invalid_input.retryability() == laghu::core::Retryability::never &&
             invalid_input.security_relevance() == laghu::core::SecurityRelevance::ordinary)) {
    return 2;
  }

  const auto deadline = laghu::core::Error::from_errno(ETIMEDOUT);
  if (!check(deadline.code() == laghu::core::ErrorCode::deadline &&
             deadline.retryability() == laghu::core::Retryability::may_retry)) {
    return 3;
  }

  const auto dependency = laghu::core::Error::from_dependency(
      laghu::core::DependencyStatus::checksum, -9, "dependency checksum");
  if (!check(dependency.domain() == laghu::core::ErrorDomain::dependency &&
             dependency.code() == laghu::core::ErrorCode::checksum &&
             dependency.native_code() == -9 &&
             dependency.security_relevance() == laghu::core::SecurityRelevance::security_relevant)) {
    return 4;
  }

  const std::array<char, 3> embedded_nul{'a', '\0', 'b'};
  const auto embedded = laghu::core::Error{laghu::core::ErrorDomain::core,
                                            laghu::core::ErrorCode::invalid_state, 0,
                                            {embedded_nul.data(), embedded_nul.size()}};
  if (!check(embedded.diagnostic_context().size() == embedded_nul.size() &&
             embedded.diagnostic_context()[1] == '\0' &&
             embedded.diagnostic_bytes()[3] == '\0' && !embedded.diagnostic_truncated())) {
    return 5;
  }

  std::array<char, laghu::core::Error::diagnostic_context_capacity + 1> oversized{};
  for (std::size_t index = 0; index < oversized.size(); ++index) {
    oversized[index] = static_cast<char>('a' + static_cast<int>(index % 26));
  }
  const auto truncated = laghu::core::Error{laghu::core::ErrorDomain::core,
                                             laghu::core::ErrorCode::invalid_state, 0,
                                             {oversized.data(), oversized.size()}};
  if (!check(truncated.diagnostic_context().size() ==
                 laghu::core::Error::diagnostic_context_capacity &&
             truncated.diagnostic_truncated() &&
             truncated.diagnostic_bytes().front() == oversized.front() &&
             truncated.diagnostic_bytes().back() == oversized[95])) {
    return 6;
  }

  const auto short_context = laghu::core::Error{laghu::core::ErrorDomain::core,
                                                 laghu::core::ErrorCode::invalid_state, 0, "ok"};
  if (!check(short_context.diagnostic_context() == std::string_view{"ok"} &&
             short_context.diagnostic_bytes()[2] == '\0' &&
             !short_context.diagnostic_truncated())) {
    return 7;
  }

  laghu::core::Result<int> failure = std::unexpected{invalid_input};
  if (!check(!failure.has_value() && failure.error().code() == laghu::core::ErrorCode::invalid_input)) {
    return 8;
  }
  if (!check(allocation_attempts == allocation_attempts_before)) {
    return 9;
  }
  return 0;
}
