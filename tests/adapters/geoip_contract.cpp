// SPDX-License-Identifier: AGPL-3.0-only
#include <type_traits>

#include <laghu/adapters/geoip.hpp>

static_assert(std::is_trivially_copyable_v<laghu::adapters::GeoIpAddress>);
static_assert(std::is_trivially_copyable_v<laghu::adapters::GeoIpCountry>);
static_assert(!std::is_copy_constructible_v<laghu::adapters::GeoIpDatabase>);

int main() { return 0; }
