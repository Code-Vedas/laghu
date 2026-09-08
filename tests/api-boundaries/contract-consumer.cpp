// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/contract.hpp>

int main() { return laghu::core::compiler_family_id() == 0 ? 1 : 0; }
