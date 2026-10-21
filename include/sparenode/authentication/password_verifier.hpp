#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "sparenode/foundation/result.hpp"

namespace sparenode::authentication
{

/// @brief Maximum plaintext password size accepted by the authentication boundary.
inline constexpr std::size_t maximum_password_bytes = 4096;

/// @brief Identifies a password hashing or verifier-validation failure.
enum class PasswordVerifierErrorCode : std::uint8_t
{
    initialization_failed, ///< The vetted cryptographic library could not initialize.
    empty_password,        ///< Provisioning attempted to hash an empty password.
    password_too_large,    ///< Plaintext exceeds the authentication body boundary.
    invalid_verifier,      ///< Encoded verifier is malformed or uses an unsupported profile.
    hashing_failed,        ///< Argon2id could not complete, usually due to resource pressure.
    resource_exhausted,    ///< Result storage could not be allocated.
};

/// @brief Abstracts password hashing and verification from concrete cryptography.
class PasswordVerifier
{
  public:
    virtual ~PasswordVerifier() = default;

    /// @brief Creates an encoded Argon2id verifier with a cryptographically random salt.
    /// @param[in] password Temporary plaintext bytes to hash without retaining them.
    /// @return Self-contained encoded verifier, or a bounded hashing failure.
    [[nodiscard]] virtual Result<std::string, PasswordVerifierErrorCode>
    hash(std::span<const char> password) const = 0;

    /// @brief Verifies plaintext against one prevalidated encoded Argon2id verifier.
    /// @param[in] password Temporary plaintext bytes to verify without retaining them.
    /// @param[in] encoded_verifier Algorithm, parameters, salt, and hash from a provider.
    /// @return Match result, or `invalid_verifier` for unsupported credential material.
    [[nodiscard]] virtual Result<bool, PasswordVerifierErrorCode>
    verify(std::span<const char> password, std::string_view encoded_verifier) const noexcept = 0;

    /// @brief Checks the verifier syntax, algorithm, and bounded parameter profile.
    /// @param[in] encoded_verifier Candidate provider material.
    /// @return `true` only for the currently supported Argon2id profile.
    [[nodiscard]] virtual bool is_usable(std::string_view encoded_verifier) const noexcept = 0;

  protected:
    /// @brief Allows construction only through concrete verifier adapters.
    PasswordVerifier() = default;
    /// @brief Supports copying concrete verifier adapters.
    PasswordVerifier(const PasswordVerifier &) = default;
    /// @brief Supports copy assignment for concrete verifier adapters.
    /// @return This verifier port.
    PasswordVerifier &operator=(const PasswordVerifier &) = default;
    /// @brief Supports moving concrete verifier adapters.
    PasswordVerifier(PasswordVerifier &&) noexcept = default;
    /// @brief Supports move assignment for concrete verifier adapters.
    /// @return This verifier port.
    PasswordVerifier &operator=(PasswordVerifier &&) noexcept = default;
};

} // namespace sparenode::authentication
