// SPDX-License-Identifier: AGPL-3.0-only
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include "laghu_test_support.hpp"

#include <laghu/adapters/geoip.hpp>

namespace {

using laghu::adapters::GeoIpAddress;
using laghu::adapters::GeoIpAddressFamily;
using laghu::adapters::GeoIpDatabase;
using laghu::core::GenerationId;
using laghu::core::TextView;

[[nodiscard]] constexpr GenerationId generation(std::uint64_t value) noexcept {
  return *GenerationId::from_uint64(value);
}

[[nodiscard]] constexpr GeoIpAddress ipv4(
    std::uint8_t first, std::uint8_t second,
    std::uint8_t third, std::uint8_t fourth) noexcept {
  GeoIpAddress address{};
  address.bytes[0] = static_cast<std::byte>(first);
  address.bytes[1] = static_cast<std::byte>(second);
  address.bytes[2] = static_cast<std::byte>(third);
  address.bytes[3] = static_cast<std::byte>(fourth);
  return address;
}

[[nodiscard]] bool write_all(int descriptor, const std::byte* data,
                             std::size_t size) noexcept {
  while (size != 0U) {
    const ssize_t written = ::write(descriptor, data, size);
    if (written <= 0) return false;
    data += static_cast<std::size_t>(written);
    size -= static_cast<std::size_t>(written);
  }
  return true;
}

[[nodiscard]] bool copy_prefix(std::string_view source,
                               std::string_view destination,
                               std::size_t maximum) noexcept {
  const int input = ::open(source.data(), O_RDONLY);
  if (input < 0) return false;
  const int output = ::open(destination.data(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (output < 0) {
    (void)::close(input);
    return false;
  }
  std::array<std::byte, 4096> buffer{};
  std::size_t copied{};
  bool valid = true;
  while (copied < maximum) {
    const std::size_t requested = std::min(buffer.size(), maximum - copied);
    const ssize_t count = ::read(input, buffer.data(), requested);
    if (count < 0) {
      valid = false;
      break;
    }
    if (count == 0) break;
    if (!write_all(output, buffer.data(), static_cast<std::size_t>(count))) {
      valid = false;
      break;
    }
    copied += static_cast<std::size_t>(count);
  }
  valid = ::close(input) == 0 && ::close(output) == 0 && valid;
  return valid && copied != 0U;
}

[[nodiscard]] bool known_unknown_and_ipv6() noexcept {
  auto database = GeoIpDatabase::open_for_reload(
      TextView::from(LAGHU_GEOIP_FIXTURE), generation(1));
  if (!database.has_value()) return false;
  const auto london = database->lookup(ipv4(81, 2, 69, 160));
  if (!london.has_value() || !london->known ||
      london->generation.value() != 1U ||
      london->country.code.value() != "GB" ||
      london->city.name.value() != "London" || london->asn.known) return false;

  const auto unknown = database->lookup(ipv4(10, 0, 0, 1));
  if (!unknown.has_value() || unknown->known || unknown->country.code.known ||
      unknown->subdivision.name.known || unknown->city.name.known ||
      unknown->asn.known) return false;

  GeoIpAddress ipv6{};
  ipv6.family = GeoIpAddressFamily::ipv6;
  ipv6.bytes[0] = std::byte{0x20};
  ipv6.bytes[1] = std::byte{0x01};
  ipv6.bytes[2] = std::byte{0x02};
  ipv6.bytes[3] = std::byte{0x18};
  return database->lookup(ipv6).has_value();
}

[[nodiscard]] bool replacement_preserves_copied_results() noexcept {
  auto current = GeoIpDatabase::open_for_reload(
      TextView::from(LAGHU_GEOIP_FIXTURE), generation(7));
  if (!current.has_value()) return false;
  const auto old_result = current->lookup(ipv4(81, 2, 69, 160));
  auto replacement = GeoIpDatabase::open_for_reload(
      TextView::from(LAGHU_GEOIP_FIXTURE), generation(8));
  if (!old_result.has_value() || !replacement.has_value()) return false;
  *current = std::move(*replacement);
  const auto new_result = current->lookup(ipv4(81, 2, 69, 160));
  return new_result.has_value() && old_result->generation.value() == 7U &&
      new_result->generation.value() == 8U &&
      old_result->city.name.value() == "London";
}

[[nodiscard]] bool malformed_and_truncated_are_rejected() noexcept {
  auto directory = laghu::test::TemporaryDirectory::create("geoip");
  if (!directory.has_value()) return false;
  std::array<char, laghu::test::fixture_path_capacity> malformed_path{};
  std::array<char, laghu::test::fixture_path_capacity> truncated_path{};
  const int malformed_size = std::snprintf(
      malformed_path.data(), malformed_path.size(), "%.*s/malformed.mmdb",
      static_cast<int>(directory->path().size()), directory->path().data());
  const int truncated_size = std::snprintf(
      truncated_path.data(), truncated_path.size(), "%.*s/truncated.mmdb",
      static_cast<int>(directory->path().size()), directory->path().data());
  if (malformed_size <= 0 || truncated_size <= 0 ||
      static_cast<std::size_t>(malformed_size) >= malformed_path.size() ||
      static_cast<std::size_t>(truncated_size) >= truncated_path.size()) return false;
  const int malformed = ::open(malformed_path.data(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  std::array<std::byte, 32> zeros{};
  if (malformed < 0 || !write_all(malformed, zeros.data(), zeros.size()) ||
      ::close(malformed) != 0 ||
      !copy_prefix(LAGHU_GEOIP_FIXTURE, truncated_path.data(), 128U)) return false;
  const auto bad = GeoIpDatabase::open_for_reload(
      TextView::from(malformed_path.data()), generation(2));
  const auto short_file = GeoIpDatabase::open_for_reload(
      TextView::from(truncated_path.data()), generation(3));
  return !bad.has_value() && !short_file.has_value();
}

[[nodiscard]] bool path_boundary_is_enforced() noexcept {
  const auto relative = GeoIpDatabase::open_for_reload(
      TextView::from("database.mmdb"), generation(1));
  const auto traversal = GeoIpDatabase::open_for_reload(
      TextView::from("/tmp/../database.mmdb"), generation(1));
  const auto wrong_extension = GeoIpDatabase::open_for_reload(
      TextView::from("/tmp/database.dat"), generation(1));
  return !relative.has_value() && !traversal.has_value() &&
      !wrong_extension.has_value();
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"geoip.known-unknown-ipv6", known_unknown_and_ipv6},
      laghu::test::TestCase{"geoip.replacement", replacement_preserves_copied_results},
      laghu::test::TestCase{"geoip.malformed-truncated",
                            malformed_and_truncated_are_rejected},
      laghu::test::TestCase{"geoip.path-boundary", path_boundary_is_enforced},
  };
  return laghu::test::run_tests(tests);
}
