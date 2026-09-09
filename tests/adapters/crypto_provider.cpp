// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

#include <laghu/adapters/crypto_provider.hpp>
#include <laghu/adapters/internal/entropy.hpp>

namespace {

template <std::size_t Size>
[[nodiscard]] consteval std::array<std::byte, Size> hex_bytes(
    std::string_view text) noexcept {
  std::array<std::byte, Size> bytes{};
  for (std::size_t index = 0; index < Size; ++index) {
    const auto nibble = [](char character) consteval -> std::uint8_t {
      if (character >= '0' && character <= '9') {
        return static_cast<std::uint8_t>(character - '0');
      }
      return static_cast<std::uint8_t>(character - 'a' + 10);
    };
    bytes[index] = static_cast<std::byte>(
        static_cast<std::uint8_t>((nibble(text[index * 2]) << 4U) |
                                  nibble(text[index * 2 + 1])));
  }
  return bytes;
}

template <std::size_t Size>
[[nodiscard]] laghu::core::ByteView view_of(
    const std::array<std::byte, Size>& bytes) noexcept {
  return *laghu::core::ByteView::from(std::span<const std::byte>{bytes});
}

template <std::size_t Size>
[[nodiscard]] laghu::core::MutableByteView mutable_view_of(
    std::array<std::byte, Size>& bytes) noexcept {
  return *laghu::core::MutableByteView::from(std::span<std::byte>{bytes});
}

template <std::size_t Size>
[[nodiscard]] bool bytes_equal(const std::array<std::byte, Size>& left,
                               const std::array<std::byte, Size>& right) noexcept {
  for (std::size_t index = 0; index < Size; ++index) {
    if (left[index] != right[index]) {
      return false;
    }
  }
  return true;
}

struct EntropyFixture final {
  std::size_t calls{};
  std::size_t fail_on_call{};
  std::size_t largest_request{};
  std::uint8_t next_byte{};
};

[[nodiscard]] int injected_entropy(laghu::core::MutableByteView output,
                                   void* context) noexcept {
  auto& fixture = *static_cast<EntropyFixture*>(context);
  ++fixture.calls;
  if (output.size() > fixture.largest_request) {
    fixture.largest_request = output.size();
  }
  if (fixture.fail_on_call != 0 && fixture.calls == fixture.fail_on_call) {
    errno = EIO;
    return -1;
  }
  for (std::byte& byte : output.span()) {
    byte = static_cast<std::byte>(fixture.next_byte);
  }
  ++fixture.next_byte;
  return 0;
}

[[nodiscard]] bool check_sha256(const laghu::core::CryptoProvider& provider) noexcept {
  constexpr std::array<std::byte, 3> input{
      std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  constexpr auto expected = hex_bytes<32>(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const auto digest = provider.sha256(view_of(input));
  return digest.has_value() && bytes_equal(digest->bytes(), expected);
}

[[nodiscard]] bool check_hmac_sha256(
    const laghu::core::CryptoProvider& provider) noexcept {
  std::array<std::byte, 20> key{};
  key.fill(std::byte{0x0B});
  constexpr std::array<std::byte, 8> input{
      std::byte{'H'}, std::byte{'i'}, std::byte{' '}, std::byte{'T'},
      std::byte{'h'}, std::byte{'e'}, std::byte{'r'}, std::byte{'e'}};
  constexpr auto expected = hex_bytes<32>(
      "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  const auto digest = provider.hmac_sha256(view_of(key), view_of(input));
  if (!digest.has_value() || !bytes_equal(digest->bytes(), expected)) {
    return false;
  }

  key.fill(std::byte{0xA5});
  const auto changed = provider.hmac_sha256(view_of(key), view_of(input));
  return changed.has_value() && !provider.constant_time_equal(*digest, *changed);
}

[[nodiscard]] bool check_ed25519(const laghu::core::CryptoProvider& provider) noexcept {
  constexpr auto seed = hex_bytes<32>(
      "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
  constexpr auto public_bytes = hex_bytes<32>(
      "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
  constexpr auto signature_bytes = hex_bytes<64>(
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
      "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
  constexpr std::array<std::byte, 0> message{};

  const auto public_key = laghu::core::Ed25519PublicKey::from_array(public_bytes);
  const auto expected_signature =
      laghu::core::Ed25519Signature::from_array(signature_bytes);
  const auto signature = provider.ed25519_sign(view_of(seed), view_of(message));
  if (!signature.has_value() ||
      !bytes_equal(signature->bytes(), expected_signature.bytes())) {
    return false;
  }
  const auto verified = provider.ed25519_verify(public_key, view_of(message), *signature);
  if (!verified.has_value() || !*verified) {
    return false;
  }

  auto corrupted_bytes = signature_bytes;
  corrupted_bytes.back() ^= std::byte{0x01};
  const auto corrupted = laghu::core::Ed25519Signature::from_array(corrupted_bytes);
  const auto rejected = provider.ed25519_verify(public_key, view_of(message), corrupted);
  constexpr std::array<std::byte, 31> short_seed{};
  const auto invalid_seed = provider.ed25519_sign(view_of(short_seed), view_of(message));
  return rejected.has_value() && !*rejected && !invalid_seed.has_value() &&
         invalid_seed.error().code() == laghu::core::ErrorCode::invalid_range;
}

[[nodiscard]] bool check_spki_pin(const laghu::core::CryptoProvider& provider) noexcept {
  constexpr auto der = hex_bytes<44>(
      "302a300506032b6570032100"
      "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
  constexpr auto expected = hex_bytes<32>(
      "06e3fd8fda29bb60ab59557de61edb0aecdb231134be30e75b455f8e1b792fa9");
  const auto pin = provider.spki_sha256(view_of(der));
  if (!pin.has_value() || !bytes_equal(pin->bytes(), expected)) {
    return false;
  }

  std::array<std::byte, der.size() + 1> trailing{};
  for (std::size_t index = 0; index < der.size(); ++index) {
    trailing[index] = der[index];
  }
  constexpr std::array<std::byte, 3> malformed{
      std::byte{0x30}, std::byte{0x01}, std::byte{0x00}};
  const auto trailing_result = provider.spki_sha256(view_of(trailing));
  const auto malformed_result = provider.spki_sha256(view_of(malformed));
  return !trailing_result.has_value() && !malformed_result.has_value() &&
         trailing_result.error().code() == laghu::core::ErrorCode::crypto &&
         malformed_result.error().code() == laghu::core::ErrorCode::crypto;
}

[[nodiscard]] bool check_constant_time_equality(
    const laghu::core::CryptoProvider& provider) noexcept {
  constexpr auto equal_bytes = hex_bytes<32>(
      "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
  const auto equal = laghu::core::Sha256Digest::from_array(equal_bytes);
  for (std::size_t difference : {std::size_t{0}, std::size_t{15}, std::size_t{31}}) {
    auto changed_bytes = equal_bytes;
    changed_bytes[difference] ^= std::byte{0x80};
    const auto changed = laghu::core::Sha256Digest::from_array(changed_bytes);
    if (provider.constant_time_equal(equal, changed)) {
      return false;
    }
  }
  return provider.constant_time_equal(equal, equal);
}

[[nodiscard]] bool check_entropy(const laghu::core::CryptoProvider& provider) noexcept {
  std::array<std::byte, 600> output{};
  EntropyFixture fixture{};
  const auto success = laghu::adapters::internal::fill_entropy_with(
      mutable_view_of(output), injected_entropy, &fixture);
  if (!success.has_value() || fixture.calls != 3 ||
      fixture.largest_request > laghu::adapters::internal::entropy_call_limit ||
      output.front() != std::byte{0x00} || output[256] != std::byte{0x01}) {
    return false;
  }

  fixture = EntropyFixture{0, 2, 0, 0};
  const auto failure = laghu::adapters::internal::fill_entropy_with(
      mutable_view_of(output), injected_entropy, &fixture);
  if (failure.has_value() || fixture.calls != 2 ||
      failure.error().domain() != laghu::core::ErrorDomain::posix ||
      failure.error().code() != laghu::core::ErrorCode::io ||
      failure.error().native_code() != EIO) {
    return false;
  }

  std::array<std::byte, laghu::adapters::internal::entropy_request_limit + 1> oversized{};
  fixture = {};
  const auto maximum_view = mutable_view_of(oversized).slice(
      0, laghu::adapters::internal::entropy_request_limit);
  if (!maximum_view.has_value()) {
    return false;
  }
  const auto maximum = laghu::adapters::internal::fill_entropy_with(
      *maximum_view, injected_entropy, &fixture);
  if (!maximum.has_value() || fixture.calls != 16 ||
      fixture.largest_request != laghu::adapters::internal::entropy_call_limit) {
    return false;
  }

  fixture = {};
  const auto rejected = laghu::adapters::internal::fill_entropy_with(
      mutable_view_of(oversized), injected_entropy, &fixture);
  if (rejected.has_value() || fixture.calls != 0 ||
      rejected.error().code() != laghu::core::ErrorCode::invalid_range) {
      return false;
  }

  std::array<std::byte, 0> empty{};
  fixture = {};
  const auto empty_result = laghu::adapters::internal::fill_entropy_with(
      mutable_view_of(empty), injected_entropy, &fixture);
  if (!empty_result.has_value() || fixture.calls != 0) {
    return false;
  }

  std::array<std::byte, 32> os_entropy{};
  return provider.fill_entropy(mutable_view_of(os_entropy)).has_value();
}

static_assert(sizeof(laghu::core::Sha256Digest) == 32);
static_assert(sizeof(laghu::core::Ed25519PublicKey) == 32);
static_assert(sizeof(laghu::core::Ed25519Signature) == 64);
static_assert(std::is_trivially_copyable_v<laghu::core::CryptoProvider>);

}  // namespace

int main() {
  const auto provider = laghu::adapters::crypto_provider();
  if (!provider.valid()) {
    return 1;
  }
  if (!check_sha256(provider)) {
    return 2;
  }
  if (!check_hmac_sha256(provider)) {
    return 3;
  }
  if (!check_ed25519(provider)) {
    return 4;
  }
  if (!check_spki_pin(provider)) {
    return 5;
  }
  if (!check_constant_time_equality(provider)) {
    return 6;
  }
  if (!check_entropy(provider)) {
    return 7;
  }
  return 0;
}
