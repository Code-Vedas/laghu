// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/x509.h>

#include <laghu/adapters/crypto_provider.hpp>
#include <laghu/adapters/internal/entropy.hpp>

namespace laghu::adapters {
namespace {

using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using PublicKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

template <std::size_t Size>
class SecretBytes final {
 public:
  explicit SecretBytes(core::ByteView input) noexcept {
    for (std::size_t index = 0; index < Size; ++index) {
      bytes_[index] = std::to_integer<unsigned char>(input.span()[index]);
    }
  }

  SecretBytes(const SecretBytes&) = delete;
  SecretBytes& operator=(const SecretBytes&) = delete;

  ~SecretBytes() { OPENSSL_cleanse(bytes_.data(), bytes_.size()); }

  [[nodiscard]] const unsigned char* data() const noexcept { return bytes_.data(); }
  [[nodiscard]] static constexpr std::size_t size() noexcept { return Size; }

 private:
  std::array<unsigned char, Size> bytes_{};
};

[[nodiscard]] PublicKey make_ed25519_private_key(core::ByteView seed) noexcept {
  SecretBytes<32> temporary_seed{seed};
  return PublicKey{
      EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, temporary_seed.data(),
                                   temporary_seed.size()),
      EVP_PKEY_free};
}

[[nodiscard]] core::Error invalid_range(std::string_view diagnostic) noexcept {
  return core::Error{core::ErrorDomain::crypto, core::ErrorCode::invalid_range, 0, diagnostic};
}

[[nodiscard]] constexpr core::DependencyId crypto_dependency_id() noexcept {
#if defined(LAGHU_CRYPTO_DEPENDENCY_OPENSSL)
  return core::DependencyId::openssl;
#elif defined(LAGHU_CRYPTO_DEPENDENCY_LIBRESSL)
  return core::DependencyId::libressl;
#else
#error "Laghu crypto adapter requires one selected TLS dependency identity"
#endif
}

[[nodiscard]] std::int32_t bounded_provider_error_code(unsigned long value) noexcept {
  constexpr unsigned long maximum =
      static_cast<unsigned long>(std::numeric_limits<std::int32_t>::max());
  return value > maximum ? std::numeric_limits<std::int32_t>::max()
                         : static_cast<std::int32_t>(value);
}

[[nodiscard]] core::Error crypto_failure(core::DependencyOperation operation,
                                         const DependencyLogSink* log_sink) noexcept {
  const unsigned long provider_error = ERR_get_error();
  ERR_clear_error();
  const core::Error error = normalize_dependency_error(
      crypto_dependency_id(), operation, core::DependencyStatus::crypto,
      bounded_provider_error_code(provider_error));
  if (log_sink != nullptr) {
    log_dependency_error(*log_sink, error);
  }
  return error;
}

[[nodiscard]] const unsigned char* as_unsigned(core::ByteView bytes) noexcept {
  return reinterpret_cast<const unsigned char*>(bytes.data());
}

[[nodiscard]] unsigned char* as_unsigned(std::byte* bytes) noexcept {
  return reinterpret_cast<unsigned char*>(bytes);
}

[[nodiscard]] const unsigned char* as_unsigned(const std::byte* bytes) noexcept {
  return reinterpret_cast<const unsigned char*>(bytes);
}

[[nodiscard]] core::Result<core::Sha256Digest> sha256_with_log(
    core::ByteView input, const DependencyLogSink* log_sink) noexcept {
  ERR_clear_error();
  DigestContext context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
  if (context == nullptr) {
    return std::unexpected{crypto_failure(core::DependencyOperation::sha256, log_sink)};
  }
  std::array<std::byte, core::Sha256Digest::size> output{};
  unsigned int output_size = 0;
  if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context.get(), as_unsigned(input), input.size()) != 1 ||
      EVP_DigestFinal_ex(context.get(), as_unsigned(output.data()), &output_size) != 1 ||
      output_size != output.size()) {
    return std::unexpected{crypto_failure(core::DependencyOperation::sha256, log_sink)};
  }
  return core::Sha256Digest::from_array(output);
}

[[nodiscard]] core::Result<core::Sha256Digest> hmac_sha256_with_log(
    core::ByteView key, core::ByteView input, const DependencyLogSink* log_sink) noexcept {
  if (key.size() > static_cast<std::size_t>(INT_MAX)) {
    return std::unexpected{invalid_range("HMAC key length exceeds provider limit")};
  }
  ERR_clear_error();
  std::array<std::byte, core::Sha256Digest::size> output{};
  unsigned int output_size = 0;
  const auto key_size = static_cast<int>(key.size());
  if (HMAC(EVP_sha256(), as_unsigned(key), key_size, as_unsigned(input), input.size(),
           as_unsigned(output.data()), &output_size) == nullptr ||
      output_size != output.size()) {
    return std::unexpected{crypto_failure(core::DependencyOperation::hmac_sha256, log_sink)};
  }
  return core::Sha256Digest::from_array(output);
}

[[nodiscard]] core::Result<core::Ed25519Signature> ed25519_sign_with_log(
    core::ByteView private_key_seed, core::ByteView input,
    const DependencyLogSink* log_sink) noexcept {
  if (private_key_seed.size() != core::Ed25519PublicKey::size) {
    return std::unexpected{invalid_range("Ed25519 private seed must contain 32 bytes")};
  }

  ERR_clear_error();
  PublicKey private_key = make_ed25519_private_key(private_key_seed);
  if (private_key == nullptr) {
    return std::unexpected{
        crypto_failure(core::DependencyOperation::ed25519_private_key, log_sink)};
  }
  DigestContext context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
  if (context == nullptr) {
    return std::unexpected{crypto_failure(core::DependencyOperation::ed25519_sign, log_sink)};
  }

  std::array<std::byte, core::Ed25519Signature::size> output{};
  std::size_t output_size = output.size();
  if (EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, private_key.get()) != 1 ||
      EVP_DigestSign(context.get(), as_unsigned(output.data()), &output_size,
                     as_unsigned(input), input.size()) != 1 ||
      output_size != output.size()) {
    return std::unexpected{crypto_failure(core::DependencyOperation::ed25519_sign, log_sink)};
  }
  return core::Ed25519Signature::from_array(output);
}

[[nodiscard]] core::Result<bool> ed25519_verify_with_log(
    const core::Ed25519PublicKey& public_key, core::ByteView input,
    const core::Ed25519Signature& signature, const DependencyLogSink* log_sink) noexcept {
  ERR_clear_error();
  PublicKey key{EVP_PKEY_new_raw_public_key(
                    EVP_PKEY_ED25519, nullptr, as_unsigned(public_key.bytes().data()),
                    public_key.bytes().size()),
                EVP_PKEY_free};
  if (key == nullptr) {
    return std::unexpected{crypto_failure(core::DependencyOperation::ed25519_verify, log_sink)};
  }
  DigestContext context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
  if (context == nullptr) {
    return std::unexpected{crypto_failure(core::DependencyOperation::ed25519_verify, log_sink)};
  }
  if (EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
    return std::unexpected{crypto_failure(core::DependencyOperation::ed25519_verify, log_sink)};
  }
  const int result = EVP_DigestVerify(
      context.get(), as_unsigned(signature.bytes().data()),
      signature.bytes().size(), as_unsigned(input), input.size());
  if (result == 1) {
    return true;
  }
  if (result == 0) {
    ERR_clear_error();
    return false;
  }
  return std::unexpected{crypto_failure(core::DependencyOperation::ed25519_verify, log_sink)};
}

[[nodiscard]] core::Result<core::Sha256Digest> spki_sha256_with_log(
    core::ByteView der, const DependencyLogSink* log_sink) noexcept {
  if (der.empty() || der.size() > static_cast<std::size_t>(LONG_MAX)) {
    return std::unexpected{invalid_range("SPKI DER length is outside provider limits")};
  }

  ERR_clear_error();
  const unsigned char* cursor = as_unsigned(der);
  const unsigned char* const end =
      as_unsigned(der.span().subspan(der.size()).data());
  PublicKey public_key{d2i_PUBKEY(nullptr, &cursor, static_cast<long>(der.size())),
                       EVP_PKEY_free};
  if (public_key == nullptr || cursor != end) {
    return std::unexpected{crypto_failure(core::DependencyOperation::spki_decode, log_sink)};
  }
  return sha256_with_log(der, log_sink);
}

[[nodiscard]] core::Result<core::Sha256Digest> sha256(core::ByteView input) noexcept {
  return sha256_with_log(input, nullptr);
}

[[nodiscard]] core::Result<core::Sha256Digest> hmac_sha256(core::ByteView key,
                                                           core::ByteView input) noexcept {
  return hmac_sha256_with_log(key, input, nullptr);
}

[[nodiscard]] core::Result<core::Ed25519Signature> ed25519_sign(
    core::ByteView private_key_seed, core::ByteView input) noexcept {
  return ed25519_sign_with_log(private_key_seed, input, nullptr);
}

[[nodiscard]] core::Result<bool> ed25519_verify(
    const core::Ed25519PublicKey& public_key, core::ByteView input,
    const core::Ed25519Signature& signature) noexcept {
  return ed25519_verify_with_log(public_key, input, signature, nullptr);
}

[[nodiscard]] core::Result<core::Sha256Digest> spki_sha256(core::ByteView der) noexcept {
  return spki_sha256_with_log(der, nullptr);
}

[[nodiscard]] bool constant_time_equal(const core::Sha256Digest& left,
                                       const core::Sha256Digest& right) noexcept {
  return CRYPTO_memcmp(left.bytes().data(), right.bytes().data(), left.bytes().size()) == 0;
}

}  // namespace

core::CryptoProvider crypto_provider() noexcept {
  return core::CryptoProvider{sha256, hmac_sha256, ed25519_sign, ed25519_verify,
                              spki_sha256, constant_time_equal, internal::fill_entropy};
}

core::Result<core::Sha256Digest> CryptoProviderWithLog::sha256(
    core::ByteView input) const noexcept {
  return sha256_with_log(input, &sink_);
}

core::Result<core::Sha256Digest> CryptoProviderWithLog::hmac_sha256(
    core::ByteView key, core::ByteView input) const noexcept {
  return hmac_sha256_with_log(key, input, &sink_);
}

core::Result<core::Ed25519Signature> CryptoProviderWithLog::ed25519_sign(
    core::ByteView private_key_seed, core::ByteView input) const noexcept {
  return ed25519_sign_with_log(private_key_seed, input, &sink_);
}

core::Result<bool> CryptoProviderWithLog::ed25519_verify(
    const core::Ed25519PublicKey& public_key, core::ByteView input,
    const core::Ed25519Signature& signature) const noexcept {
  return ed25519_verify_with_log(public_key, input, signature, &sink_);
}

core::Result<core::Sha256Digest> CryptoProviderWithLog::spki_sha256(
    core::ByteView der) const noexcept {
  return spki_sha256_with_log(der, &sink_);
}

}  // namespace laghu::adapters
