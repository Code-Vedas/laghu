// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

#include <laghu/adapters/dependency.hpp>
#include <laghu/core/identifiers.hpp>
#include <laghu/core/views.hpp>

namespace laghu::adapters {

enum class GeoIpAddressFamily : std::uint8_t { ipv4, ipv6 };

struct GeoIpAddress final {
  GeoIpAddressFamily family{GeoIpAddressFamily::ipv4};
  std::array<std::byte, 16> bytes{};
};

template <std::size_t Capacity>
struct GeoIpText final {
  std::array<char, Capacity> bytes{};
  std::uint16_t size{};
  bool known{};

  [[nodiscard]] constexpr std::string_view value() const noexcept {
    return {bytes.data(), size};
  }
};

struct GeoIpCountry final { GeoIpText<2> code; };
struct GeoIpSubdivision final { GeoIpText<96> name; };
struct GeoIpCity final { GeoIpText<128> name; };
struct GeoIpAsn final {
  std::uint32_t number{};
  bool known{};
};

struct GeoIpResult final {
  explicit constexpr GeoIpResult(core::GenerationId value) noexcept
      : generation(value) {}

  core::GenerationId generation;
  GeoIpCountry country{};
  GeoIpSubdivision subdivision{};
  GeoIpCity city{};
  GeoIpAsn asn{};
  bool known{};
};

class GeoIpReloadSource final {
 public:
  [[nodiscard]] static core::Result<GeoIpReloadSource> from_configuration(
      core::TextView configured_path,
      std::span<const core::TextView> allowed_paths) noexcept;

 private:
  friend class GeoIpDatabase;
  explicit GeoIpReloadSource(core::StaticCString<4096> path) noexcept
      : path_(path) {}
  core::StaticCString<4096> path_{};
};

// Opens only an owned source already matched against the configuration's exact
// path allowlist. Lookups accept neither paths nor reload authority.
class GeoIpDatabase final {
 public:
  GeoIpDatabase() noexcept = default;
  GeoIpDatabase(const GeoIpDatabase&) = delete;
  GeoIpDatabase& operator=(const GeoIpDatabase&) = delete;
  GeoIpDatabase(GeoIpDatabase&& other) noexcept;
  GeoIpDatabase& operator=(GeoIpDatabase&& other) noexcept;
  ~GeoIpDatabase();

  [[nodiscard]] static core::Result<GeoIpDatabase> open_for_reload(
      const GeoIpReloadSource& source, core::GenerationId generation,
      DependencyLogSink log_sink = {}) noexcept;
  [[nodiscard]] core::Result<GeoIpResult> lookup(
      const GeoIpAddress& address) const noexcept;
  [[nodiscard]] core::Result<core::GenerationId> generation() const noexcept;

 private:
  explicit GeoIpDatabase(void* state) noexcept : state_(state) {}
  void release() noexcept;
  void* state_{};
};

static_assert(std::is_trivially_copyable_v<GeoIpAddress>);
static_assert(std::is_trivially_copyable_v<GeoIpCountry>);
static_assert(std::is_trivially_copyable_v<GeoIpSubdivision>);
static_assert(std::is_trivially_copyable_v<GeoIpCity>);
static_assert(std::is_trivially_copyable_v<GeoIpAsn>);
static_assert(std::is_trivially_copyable_v<GeoIpReloadSource>);
static_assert(!std::is_copy_constructible_v<GeoIpDatabase>);

}  // namespace laghu::adapters
