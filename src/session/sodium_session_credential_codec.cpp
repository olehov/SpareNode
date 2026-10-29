#include "sparenode/session/sodium_session_credential_codec.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <new>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace sparenode::session
{
namespace
{

/// @brief Supplies 256 bits of entropy before canonical hexadecimal encoding.
constexpr std::size_t credential_bytes = 32;

/// @brief Two hexadecimal characters encode each random credential byte.
constexpr std::size_t encoded_credential_bytes = credential_bytes * 2;

/// @brief Erases stack-held credential material on normal and exceptional exits.
template <typename Byte, std::size_t Size> struct SensitiveArray
{
    std::array<Byte, Size> bytes{};

    SensitiveArray() = default;
    SensitiveArray(const SensitiveArray &) = delete;
    SensitiveArray &operator=(const SensitiveArray &) = delete;
    ~SensitiveArray()
    {
        // This cryptographic adapter deliberately retains libsodium's erasure guarantee;
        // foundation::secure_erase owns ordinary application buffers without adding a sodium
        // dependency to lower layers.
        sodium_memzero(bytes.data(), bytes.size() * sizeof(Byte));
    }
};

/// @brief Accepts only the lowercase canonical representation emitted by this codec.
[[nodiscard]] bool valid_encoded_credential(const std::string_view credential) noexcept
{
    return credential.size() == encoded_credential_bytes &&
           std::ranges::all_of(
               credential, [](const char byte)
               { return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'); });
}

/// @brief Hashes decoded random bytes into the fixed server-side lookup value.
[[nodiscard]] SessionCredentialDigest
credential_digest(const std::span<const unsigned char, credential_bytes> bytes) noexcept
{
    SessionCredentialDigest digest{};
    crypto_generichash(reinterpret_cast<unsigned char *>(digest.data()), digest.size(),
                       bytes.data(), bytes.size(), nullptr, 0);
    return digest;
}

} // namespace

Result<SodiumSessionCredentialCodec, SessionCredentialErrorCode>
SodiumSessionCredentialCodec::create()
{
    if (sodium_init() < 0)
    {
        return unexpected(SessionCredentialErrorCode::cryptography_unavailable);
    }
    return SodiumSessionCredentialCodec{};
}

Result<GeneratedSessionCredential, SessionCredentialErrorCode>
SodiumSessionCredentialCodec::generate() const
{
    try
    {
        SensitiveArray<unsigned char, credential_bytes> random_bytes;
        randombytes_buf(random_bytes.bytes.data(), random_bytes.bytes.size());
        SensitiveArray<char, encoded_credential_bytes + 1> encoded;
        sodium_bin2hex(encoded.bytes.data(), encoded.bytes.size(), random_bytes.bytes.data(),
                       random_bytes.bytes.size());
        auto value = std::string(encoded.bytes.data(), encoded_credential_bytes);
        const auto credential_hash = credential_digest(random_bytes.bytes);
        return GeneratedSessionCredential{std::move(value), credential_hash};
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(SessionCredentialErrorCode::resource_allocation_failed);
    }
}

Result<SessionCredentialDigest, SessionCredentialErrorCode>
SodiumSessionCredentialCodec::digest(const std::string_view credential) const
{
    if (!valid_encoded_credential(credential))
    {
        return unexpected(SessionCredentialErrorCode::invalid_credential);
    }
    SensitiveArray<unsigned char, credential_bytes> decoded;
    std::size_t decoded_size{};
    if (sodium_hex2bin(decoded.bytes.data(), decoded.bytes.size(), credential.data(),
                       credential.size(), nullptr, &decoded_size, nullptr) != 0 ||
        decoded_size != decoded.bytes.size())
    {
        return unexpected(SessionCredentialErrorCode::invalid_credential);
    }
    return credential_digest(decoded.bytes);
}

} // namespace sparenode::session
