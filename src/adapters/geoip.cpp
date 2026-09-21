// SPDX-License-Identifier: AGPL-3.0-only
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>

#include <netinet/in.h>
#include <sys/socket.h>

#include <maxminddb.h>

#include <laghu/adapters/dependency.hpp>
#include <laghu/adapters/geoip.hpp>

namespace laghu::adapters {
namespace {

constexpr std::size_t path_capacity = 4096;

struct GeoIpState final {
  MMDB_s database{};
  core::GenerationId generation;
  DependencyLogSink log{};
};

[[nodiscard]] core::Error core_error(core::ErrorCode code,
                                     const char* diagnostic) noexcept {
  return {core::ErrorDomain::core, code, 0, diagnostic};
}

[[nodiscard]] core::DependencyStatus status_for(int status) noexcept {
  switch (status) {
    case MMDB_FILE_OPEN_ERROR:
    case MMDB_IO_ERROR:
      return core::DependencyStatus::io;
    case MMDB_OUT_OF_MEMORY_ERROR:
      return core::DependencyStatus::exhaustion;
    case MMDB_INVALID_DATA_ERROR:
    case MMDB_INVALID_METADATA_ERROR:
    case MMDB_CORRUPT_SEARCH_TREE_ERROR:
    case MMDB_UNKNOWN_DATABASE_FORMAT_ERROR:
    case MMDB_DECODER_LIMIT_ERROR:
      return core::DependencyStatus::corrupt_data;
    case MMDB_INVALID_LOOKUP_PATH_ERROR:
    case MMDB_LOOKUP_PATH_DOES_NOT_MATCH_DATA_ERROR:
    case MMDB_INVALID_NODE_NUMBER_ERROR:
    case MMDB_IPV6_LOOKUP_IN_IPV4_DATABASE_ERROR:
    case MMDB_INVALID_NETWORK_ADDRESS_ERROR:
      return core::DependencyStatus::invalid_input;
    default:
      return core::DependencyStatus::unknown;
  }
}

[[nodiscard]] core::Error native_error(
    core::DependencyOperation operation, int status,
    const DependencyLogSink& sink) noexcept {
  const core::Error error = normalize_dependency_error(
      core::DependencyId::libmaxminddb, operation, status_for(status), status);
  log_dependency_error(sink, error);
  return error;
}

[[nodiscard]] bool valid_configured_path(std::string_view path) noexcept {
  if (path.empty() || path.front() != '/' || path.ends_with('/') ||
      !path.ends_with(".mmdb")) return false;
  if (path.find("//") != std::string_view::npos ||
      path.find("/./") != std::string_view::npos ||
      path.find("/../") != std::string_view::npos || path.ends_with("/.") ||
      path.ends_with("/..")) return false;
  return true;
}

template <std::size_t Capacity>
[[nodiscard]] core::Result<void> copy_text(
    MMDB_entry_s& entry, GeoIpText<Capacity>& output,
    const char* const* path, const DependencyLogSink& sink) noexcept {
  MMDB_entry_data_s data{};
  const int status = MMDB_aget_value(&entry, &data, path);
  if (status == MMDB_LOOKUP_PATH_DOES_NOT_MATCH_DATA_ERROR) return {};
  if (status != MMDB_SUCCESS) {
    return std::unexpected{native_error(
        core::DependencyOperation::geoip_lookup, status, sink)};
  }
  if (!data.has_data) return {};
  if (data.type != MMDB_DATA_TYPE_UTF8_STRING) {
    return std::unexpected{native_error(
        core::DependencyOperation::geoip_lookup, MMDB_INVALID_DATA_ERROR, sink)};
  }
  if (data.data_size > output.bytes.size()) {
    return std::unexpected{core_error(
        core::ErrorCode::invalid_range, "GeoIP text exceeds Laghu field capacity")};
  }
  std::copy_n(data.utf8_string, data.data_size, output.bytes.begin());
  output.size = static_cast<std::uint16_t>(data.data_size);
  output.known = true;
  return {};
}

[[nodiscard]] core::Result<void> copy_asn(
    MMDB_entry_s& entry, GeoIpAsn& output,
    const DependencyLogSink& sink) noexcept {
  const char* const path[]{"autonomous_system_number", nullptr};
  MMDB_entry_data_s data{};
  const int status = MMDB_aget_value(&entry, &data, path);
  if (status == MMDB_LOOKUP_PATH_DOES_NOT_MATCH_DATA_ERROR) return {};
  if (status != MMDB_SUCCESS) {
    return std::unexpected{native_error(
        core::DependencyOperation::geoip_lookup, status, sink)};
  }
  if (!data.has_data) return {};
  if (data.type != MMDB_DATA_TYPE_UINT32) {
    return std::unexpected{native_error(
        core::DependencyOperation::geoip_lookup, MMDB_INVALID_DATA_ERROR, sink)};
  }
  output.number = data.uint32;
  output.known = true;
  return {};
}

}  // namespace

GeoIpDatabase::GeoIpDatabase(GeoIpDatabase&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}

GeoIpDatabase& GeoIpDatabase::operator=(GeoIpDatabase&& other) noexcept {
  if (this != &other) {
    release();
    state_ = std::exchange(other.state_, nullptr);
  }
  return *this;
}

GeoIpDatabase::~GeoIpDatabase() { release(); }

core::Result<GeoIpDatabase> GeoIpDatabase::open_for_reload(
    core::TextView configured_path, core::GenerationId generation,
    DependencyLogSink log_sink) noexcept {
  const auto path = configured_path.to_c_string<path_capacity>();
  if (!path.has_value()) return std::unexpected{path.error()};
  if (!valid_configured_path(path->view())) {
    return std::unexpected{core_error(
        core::ErrorCode::invalid_input,
        "GeoIP reload path must be an absolute normalized .mmdb path")};
  }
  auto* state = new (std::nothrow) GeoIpState{{}, generation, log_sink};
  if (state == nullptr) {
    return std::unexpected{core_error(
        core::ErrorCode::exhaustion, "GeoIP handle allocation is exhausted")};
  }
  const int status = MMDB_open(path->c_str(), MMDB_MODE_MMAP, &state->database);
  if (status != MMDB_SUCCESS) {
    const core::Error error = native_error(
        core::DependencyOperation::geoip_open, status, log_sink);
    delete state;
    return std::unexpected{error};
  }
  return GeoIpDatabase{state};
}

core::Result<GeoIpResult> GeoIpDatabase::lookup(
    const GeoIpAddress& address) const noexcept {
  if (state_ == nullptr) {
    return std::unexpected{core_error(
        core::ErrorCode::invalid_state, "GeoIP database handle is inactive")};
  }
  const auto& state = *static_cast<const GeoIpState*>(state_);
  sockaddr_storage storage{};
  const sockaddr* native_address{};
  if (address.family == GeoIpAddressFamily::ipv4) {
    if (std::any_of(address.bytes.begin() + 4, address.bytes.end(),
                    [](std::byte value) { return value != std::byte{}; })) {
      return std::unexpected{core_error(
          core::ErrorCode::invalid_input, "IPv4 GeoIP address has nonzero tail bytes")};
    }
    auto& ipv4 = reinterpret_cast<sockaddr_in&>(storage);
    ipv4.sin_family = AF_INET;
    std::memcpy(&ipv4.sin_addr, address.bytes.data(), 4U);
    native_address = reinterpret_cast<const sockaddr*>(&ipv4);
  } else if (address.family == GeoIpAddressFamily::ipv6) {
    auto& ipv6 = reinterpret_cast<sockaddr_in6&>(storage);
    ipv6.sin6_family = AF_INET6;
    std::memcpy(&ipv6.sin6_addr, address.bytes.data(), address.bytes.size());
    native_address = reinterpret_cast<const sockaddr*>(&ipv6);
  } else {
    return std::unexpected{core_error(
        core::ErrorCode::invalid_input, "GeoIP address family is invalid")};
  }

  int status{};
  MMDB_lookup_result_s found = MMDB_lookup_sockaddr(
      &state.database, native_address, &status);
  if (status != MMDB_SUCCESS) {
    return std::unexpected{native_error(
        core::DependencyOperation::geoip_lookup, status, state.log)};
  }
  GeoIpResult result{state.generation};
  if (!found.found_entry) return result;
  result.known = true;
  const char* const country_path[]{"country", "iso_code", nullptr};
  const char* const subdivision_path[]{"subdivisions", "0", "names", "en", nullptr};
  const char* const city_path[]{"city", "names", "en", nullptr};
  if (const auto copied = copy_text(
          found.entry, result.country.code, country_path, state.log);
      !copied.has_value()) return std::unexpected{copied.error()};
  if (const auto copied = copy_text(
          found.entry, result.subdivision.name, subdivision_path, state.log);
      !copied.has_value()) return std::unexpected{copied.error()};
  if (const auto copied = copy_text(
          found.entry, result.city.name, city_path, state.log);
      !copied.has_value()) return std::unexpected{copied.error()};
  if (const auto copied = copy_asn(found.entry, result.asn, state.log);
      !copied.has_value()) return std::unexpected{copied.error()};
  return result;
}

core::Result<core::GenerationId> GeoIpDatabase::generation() const noexcept {
  if (state_ == nullptr) {
    return std::unexpected{core_error(
        core::ErrorCode::invalid_state, "GeoIP database handle is inactive")};
  }
  return static_cast<const GeoIpState*>(state_)->generation;
}

void GeoIpDatabase::release() noexcept {
  if (state_ == nullptr) return;
  auto* state = static_cast<GeoIpState*>(state_);
  MMDB_close(&state->database);
  delete state;
  state_ = nullptr;
}

}  // namespace laghu::adapters
