// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/checked_arithmetic.hpp>

int main() {
  return laghu::core::checked_add(true, false).has_value() ? 0 : 1;
}
