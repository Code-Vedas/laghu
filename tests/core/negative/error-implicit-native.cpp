// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/contract.hpp>

int main() {
  laghu::core::Error error = 7;
  return error.native_code();
}
