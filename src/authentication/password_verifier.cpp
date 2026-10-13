#include "sparenode/authentication/password_verifier.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <new>

namespace sparenode::authentication
{
namespace
{

/// @brief Current bounded online-password profile persisted inside every verifier.
constexpr unsigned long long password_operations = crypto_pwhash_OPSLIMIT_INTERACTIVE;

/// @brief Maximum memory dedicated to one online password operation.
constexpr std::size_t password_memory_bytes = crypto_pwhash_MEMLIMIT_INTERACTIVE;

/// @brief Algorithm remains explicit so a dependency default cannot silently change credentials.
constexpr int password_algorithm = crypto_pwhash_ALG_ARGON2ID13;

/// @brief Canonical libsodium Argon2id v1.3 profile accepted from external provisioning.
constexpr std::string_view supported_password_profile = "$argon2id$v=19$m=65536,t=2,p=1$";

/// @brief Copies an encoded verifier into fixed NUL-terminated library storage.
[[nodiscard]] bool copy_encoded_verifier(const std::string_view encoded_verifier,
                                         std::array<char, crypto_pwhash_STRBYTES> &storage) noexcept
{
    if (encoded_verifier.empty() || encoded_verifier.size() >= storage.size() ||
        encoded_verifier.find('\0') != std::string_view::npos)
    {
        return false;
    }
    std::ranges::copy(encoded_verifier, storage.begin());
    return true;
}

} // namespace

Result<PasswordVerifier, PasswordVerifierErrorCode> PasswordVerifier::create() noexcept
{
    if (sodium_init() < 0)
    {
        return unexpected(PasswordVerifierErrorCode::initialization_failed);
    }
    return PasswordVerifier{};
}

Result<std::string, PasswordVerifierErrorCode>
PasswordVerifier::hash(const std::span<const char> password) const
{
    if (password.empty())
    {
        return unexpected(PasswordVerifierErrorCode::empty_password);
    }
    if (password.size() > maximum_password_bytes)
    {
        return unexpected(PasswordVerifierErrorCode::password_too_large);
    }

    std::array<char, crypto_pwhash_STRBYTES> encoded{};
    if (crypto_pwhash_str_alg(encoded.data(), password.data(),
                              static_cast<unsigned long long>(password.size()), password_operations,
                              password_memory_bytes, password_algorithm) != 0)
    {
        return unexpected(PasswordVerifierErrorCode::hashing_failed);
    }

    try
    {
        return std::string(encoded.data());
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(PasswordVerifierErrorCode::resource_exhausted);
    }
}

Result<bool, PasswordVerifierErrorCode>
PasswordVerifier::verify(const std::span<const char> password,
                         const std::string_view encoded_verifier) const noexcept
{
    if (password.size() > maximum_password_bytes)
    {
        return unexpected(PasswordVerifierErrorCode::password_too_large);
    }

    std::array<char, crypto_pwhash_STRBYTES> encoded{};
    if (!is_usable(encoded_verifier) || !copy_encoded_verifier(encoded_verifier, encoded))
    {
        return unexpected(PasswordVerifierErrorCode::invalid_verifier);
    }

    return crypto_pwhash_str_verify(encoded.data(), password.data(),
                                    static_cast<unsigned long long>(password.size())) == 0;
}

bool PasswordVerifier::is_usable(const std::string_view encoded_verifier) noexcept
{
    if (!encoded_verifier.starts_with(supported_password_profile))
    {
        return false;
    }

    std::array<char, crypto_pwhash_STRBYTES> encoded{};
    if (!copy_encoded_verifier(encoded_verifier, encoded))
    {
        return false;
    }

    return crypto_pwhash_str_needs_rehash(encoded.data(), password_operations,
                                          password_memory_bytes) == 0;
}

} // namespace sparenode::authentication
