// SPDX-License-Identifier: AGPL-3.0-only
#include <bit>
#include <cstdint>
static_assert(std::byteswap(std::uint32_t{0x11223344U}) == 0x44332211U);
