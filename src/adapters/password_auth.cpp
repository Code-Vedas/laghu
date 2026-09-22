// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include <crypt.h>

#include <laghu/adapters/password_auth.hpp>
#include <laghu/adapters/internal/password_auth.hpp>

namespace laghu::adapters {
namespace {

constexpr std::size_t encoded_password_capacity = 256;
constexpr std::size_t password_c_string_capacity =
    PasswordVerificationLimits::maximum_supported_password_bytes + 1;
constexpr std::uint32_t bcrypt_minimum_cost = 4;
constexpr std::uint32_t bcrypt_maximum_cost = 31;
constexpr std::uint32_t sha512_minimum_rounds = 1000;
constexpr std::uint32_t sha512_maximum_rounds = 999999999;
constexpr std::uint32_t sha512_crypt_standard_rounds = 5000;

static_assert(CRYPT_MAX_PASSPHRASE_SIZE == password_c_string_capacity,
              "Laghu password limit must match libxcrypt's passphrase contract");
static_assert(internal::SecretPassword::storage_capacity == password_c_string_capacity,
              "Laghu password secret storage must match libxcrypt's passphrase contract");

enum class PasswordScheme : std::uint8_t {
  bcrypt,
  sha512,
};

struct ParsedPasswordHash final {
  PasswordScheme scheme{};
  std::uint32_t work_factor{};
};

class PasswordWorkerLease final {
 public:
  explicit constexpr PasswordWorkerLease(std::size_t& active) noexcept : active_(&active) {
    ++*active_;
  }

  PasswordWorkerLease(const PasswordWorkerLease&) = delete;
  PasswordWorkerLease& operator=(const PasswordWorkerLease&) = delete;

  ~PasswordWorkerLease() { --*active_; }

 private:
  std::size_t* active_{};
};

[[nodiscard]] constexpr bool is_crypt_character(char character) noexcept {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '.' || character == '/';
}

[[nodiscard]] core::Error input_error(core::ErrorCode code,
                                      std::string_view diagnostic) noexcept {
  return core::Error{core::ErrorDomain::core, code, 0, diagnostic};
}

[[nodiscard]] core::Result<void> validate_limits(
    const PasswordVerificationLimits& limits) noexcept {
  if (limits.maximum_password_bytes == 0 ||
      limits.maximum_password_bytes > PasswordVerificationLimits::maximum_supported_password_bytes ||
      limits.minimum_bcrypt_cost < bcrypt_minimum_cost ||
      limits.maximum_bcrypt_cost > bcrypt_maximum_cost ||
      limits.minimum_bcrypt_cost > limits.maximum_bcrypt_cost ||
      limits.minimum_sha512_rounds < sha512_minimum_rounds ||
      limits.maximum_sha512_rounds > sha512_maximum_rounds ||
      limits.minimum_sha512_rounds > limits.maximum_sha512_rounds) {
    return std::unexpected{input_error(core::ErrorCode::invalid_input,
                                       "password verification limits are invalid")};
  }
  return {};
}

[[nodiscard]] bool parse_decimal(std::string_view text, std::uint32_t& value) noexcept {
  if (text.empty()) {
    return false;
  }
  std::uint32_t parsed{};
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
    const std::uint32_t digit = static_cast<std::uint32_t>(character - '0');
    if (parsed > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U) {
      return false;
    }
    parsed = parsed * 10U + digit;
  }
  value = parsed;
  return true;
}

[[nodiscard]] core::Result<ParsedPasswordHash> parse_bcrypt(
    std::string_view encoded_hash) noexcept {
  if (encoded_hash.size() != 60 || encoded_hash[0] != '$' || encoded_hash[1] != '2' ||
      (encoded_hash[2] != 'a' && encoded_hash[2] != 'b' && encoded_hash[2] != 'y') ||
      encoded_hash[3] != '$' || encoded_hash[6] != '$') {
    return std::unexpected{input_error(core::ErrorCode::invalid_input,
                                       "bcrypt encoding is malformed")};
  }
  std::uint32_t cost{};
  if (!parse_decimal(encoded_hash.substr(4, 2), cost) || cost < bcrypt_minimum_cost ||
      cost > bcrypt_maximum_cost) {
    return std::unexpected{input_error(core::ErrorCode::invalid_input,
                                       "bcrypt work factor is malformed")};
  }
  for (std::size_t index = 7; index < encoded_hash.size(); ++index) {
    if (!is_crypt_character(encoded_hash[index])) {
      return std::unexpected{input_error(core::ErrorCode::invalid_input,
                                         "bcrypt encoding is malformed")};
    }
  }
  return ParsedPasswordHash{PasswordScheme::bcrypt, cost};
}

[[nodiscard]] core::Result<ParsedPasswordHash> parse_sha512(
    std::string_view encoded_hash) noexcept {
  if (!encoded_hash.starts_with("$6$")) {
    return std::unexpected{input_error(core::ErrorCode::invalid_input,
                                       "SHA-512 crypt encoding is malformed")};
  }
  std::size_t cursor = 3;
  std::uint32_t rounds = sha512_crypt_standard_rounds;
  if (encoded_hash.substr(cursor).starts_with("rounds=")) {
    cursor += 7;
    const std::size_t rounds_end = encoded_hash.find('$', cursor);
    if (rounds_end == std::string_view::npos ||
        !parse_decimal(encoded_hash.substr(cursor, rounds_end - cursor), rounds) ||
        rounds < sha512_minimum_rounds || rounds > sha512_maximum_rounds) {
      return std::unexpected{input_error(core::ErrorCode::invalid_input,
                                         "SHA-512 crypt rounds are malformed")};
    }
    cursor = rounds_end + 1;
  }
  const std::size_t salt_end = encoded_hash.find('$', cursor);
  if (salt_end == std::string_view::npos || salt_end == cursor || salt_end - cursor > 16) {
    return std::unexpected{input_error(core::ErrorCode::invalid_input,
                                       "SHA-512 crypt salt is malformed")};
  }
  for (std::size_t index = cursor; index < salt_end; ++index) {
    if (!is_crypt_character(encoded_hash[index])) {
      return std::unexpected{input_error(core::ErrorCode::invalid_input,
                                         "SHA-512 crypt salt is malformed")};
    }
  }
  const std::string_view checksum = encoded_hash.substr(salt_end + 1);
  if (checksum.size() != 86) {
    return std::unexpected{input_error(core::ErrorCode::invalid_input,
                                       "SHA-512 crypt checksum is malformed")};
  }
  for (const char character : checksum) {
    if (!is_crypt_character(character)) {
      return std::unexpected{input_error(core::ErrorCode::invalid_input,
                                         "SHA-512 crypt checksum is malformed")};
    }
  }
  return ParsedPasswordHash{PasswordScheme::sha512, rounds};
}

[[nodiscard]] core::Result<ParsedPasswordHash> parse_password_hash(
    std::string_view encoded_hash) noexcept {
  if (encoded_hash.starts_with("$2")) {
    return parse_bcrypt(encoded_hash);
  }
  if (encoded_hash.starts_with("$6$")) {
    return parse_sha512(encoded_hash);
  }
  return std::unexpected{input_error(core::ErrorCode::invalid_input,
                                     "password scheme is not permitted")};
}

[[nodiscard]] core::Result<void> validate_work_factor(
    const ParsedPasswordHash& parsed, const PasswordVerificationLimits& limits) noexcept {
  if ((parsed.scheme == PasswordScheme::bcrypt &&
       (parsed.work_factor < limits.minimum_bcrypt_cost ||
        parsed.work_factor > limits.maximum_bcrypt_cost)) ||
      (parsed.scheme == PasswordScheme::sha512 &&
       (parsed.work_factor < limits.minimum_sha512_rounds ||
        parsed.work_factor > limits.maximum_sha512_rounds))) {
    return std::unexpected{input_error(core::ErrorCode::invalid_range,
                                       "password work factor is outside caller limits")};
  }
  return {};
}

[[nodiscard]] core::Result<std::string_view> bounded_crypt_output(const char* value) noexcept {
  if (value == nullptr) {
    return std::unexpected{core::Error{core::ErrorDomain::dependency,
                                       core::ErrorCode::dependency, 0,
                                       "libxcrypt returned no password result"}};
  }
  for (std::size_t index = 0; index < encoded_password_capacity; ++index) {
    if (value[index] == '\0') {
      return std::string_view{value, index};
    }
  }
  return std::unexpected{core::Error{core::ErrorDomain::dependency,
                                     core::ErrorCode::corrupt_data, 0,
                                     "libxcrypt output exceeded bounded capacity"}};
}

[[nodiscard]] bool constant_time_hash_equal(std::string_view expected,
                                            std::string_view actual) noexcept {
  std::array<unsigned char, encoded_password_capacity> expected_bytes{};
  std::array<unsigned char, encoded_password_capacity> actual_bytes{};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    expected_bytes[index] = static_cast<unsigned char>(expected[index]);
  }
  for (std::size_t index = 0; index < actual.size(); ++index) {
    actual_bytes[index] = static_cast<unsigned char>(actual[index]);
  }
  unsigned int difference{};
  for (std::size_t index = 0; index < expected_bytes.size(); ++index) {
    difference |= static_cast<unsigned int>(expected_bytes[index] ^ actual_bytes[index]);
  }
  return difference == 0U;
}

[[nodiscard]] core::DependencyStatus crypt_status(int native_code) noexcept {
  if (native_code == EINVAL) {
    return core::DependencyStatus::invalid_input;
  }
  if (native_code == ERANGE) {
    return core::DependencyStatus::invalid_range;
  }
  if (native_code == ENOMEM) {
    return core::DependencyStatus::exhaustion;
  }
  if (native_code == ENOSYS || native_code == ENOTSUP || native_code == EOPNOTSUPP) {
    return core::DependencyStatus::unavailable;
  }
  return core::DependencyStatus::io;
}

}  // namespace

namespace internal {

core::Error password_auth_native_error(std::int32_t native_code,
                                       const DependencyLogSink& log_sink) noexcept {
  const core::Error error = normalize_dependency_error(
      core::DependencyId::libxcrypt, core::DependencyOperation::password_verify,
      crypt_status(native_code), native_code);
  log_dependency_error(log_sink, error);
  return error;
}

}  // namespace internal

core::Result<bool> verify_password(PasswordAuthWorker& auth_worker, core::WorkerId worker,
                                   core::TextView password, core::TextView encoded_hash,
                                   PasswordVerificationLimits limits,
                                   DependencyLogSink log_sink) noexcept {
  if (const auto valid_limits = validate_limits(limits); !valid_limits.has_value()) {
    return std::unexpected{valid_limits.error()};
  }
  if (auth_worker.worker_.value() != worker.value()) {
    return std::unexpected{input_error(core::ErrorCode::invalid_state,
                                       "password work requires its designated auth worker")};
  }
  if (auth_worker.maximum_in_flight_ == 0) {
    return std::unexpected{input_error(core::ErrorCode::invalid_input,
                                       "password auth worker requires a positive bound")};
  }
  if (password.size() > limits.maximum_password_bytes) {
    return std::unexpected{input_error(core::ErrorCode::invalid_range,
                                       "password exceeds caller limit")};
  }
  auto bounded_password = internal::SecretPassword::from(password);
  if (!bounded_password.has_value()) {
    return std::unexpected{bounded_password.error()};
  }

  const auto bounded_hash = encoded_hash.to_c_string<encoded_password_capacity>();
  if (!bounded_hash.has_value()) {
    return std::unexpected{bounded_hash.error()};
  }
  const auto parsed_hash = parse_password_hash(bounded_hash->view());
  if (!parsed_hash.has_value()) {
    return std::unexpected{parsed_hash.error()};
  }
  if (parsed_hash->scheme == PasswordScheme::bcrypt && password.size() > 72) {
    return std::unexpected{input_error(core::ErrorCode::invalid_range,
                                       "bcrypt password exceeds 72 bytes")};
  }
  if (const auto work = validate_work_factor(*parsed_hash, limits); !work.has_value()) {
    return std::unexpected{work.error()};
  }
  if (auth_worker.active_ >= auth_worker.maximum_in_flight_) {
    return std::unexpected{input_error(core::ErrorCode::exhaustion,
                                       "password auth worker capacity is exhausted")};
  }
  PasswordWorkerLease lease{auth_worker.active_};

  crypt_data native_data{};
  errno = 0;
  const char* native_result =
      crypt_r(bounded_password->c_str(), bounded_hash->c_str(), &native_data);
  if (native_result == nullptr || native_result[0] == '*') {
    return std::unexpected{internal::password_auth_native_error(errno, log_sink)};
  }
  const auto output = bounded_crypt_output(native_result);
  if (!output.has_value()) {
    const core::Error error = normalize_dependency_error(
        core::DependencyId::libxcrypt, core::DependencyOperation::password_verify,
        core::DependencyStatus::corrupt_data, 0);
    log_dependency_error(log_sink, error);
    return std::unexpected{error};
  }
  return constant_time_hash_equal(bounded_hash->view(), *output);
}

}  // namespace laghu::adapters
