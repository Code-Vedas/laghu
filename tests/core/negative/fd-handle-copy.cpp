// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/handles.hpp>

int main() {
  laghu::core::FileHandle original;
  laghu::core::FileHandle copy{original};
  return copy.is_valid() ? 0 : 1;
}
