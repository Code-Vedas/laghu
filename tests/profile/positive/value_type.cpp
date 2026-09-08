// SPDX-License-Identifier: AGPL-3.0-only
namespace laghu::profile_fixture {
struct request_id {
  int value;
};
constexpr int next(request_id id) noexcept { return id.value + 1; }
static_assert(next({22}) == 23);
}  // namespace laghu::profile_fixture
