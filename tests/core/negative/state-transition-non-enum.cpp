// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/state_transitions.hpp>

int main() {
  using InvalidTable = laghu::core::TransitionTable<int, int>;
  return sizeof(InvalidTable);
}
