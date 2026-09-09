// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/identifiers.hpp>

void accept_connection(laghu::core::ConnectionId) noexcept {}

int main() {
  const auto worker = laghu::core::WorkerId::from_uint64(1);
  accept_connection(*worker);
  return 0;
}
