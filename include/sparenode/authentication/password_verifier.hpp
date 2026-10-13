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

/// @brief Wraps libsodium's Argon2id encoded-password API.
class PasswordVerifier final
{
  public:
    /// @brief Initializes the process-wide vetted cryptographic runtime.
    /// @return Ready verifier, or an initialization failure.
    [[nodiscard]] static Result<PasswordVerifier, PasswordVerifierErrorCode> create() noexcept;

    /// @brief Creates an encoded Argon2id verifier with a cryptographically random salt.
    /// @param[in] password Temporary plaintext bytes to hash without retaining them.
    /// @return Self-contained encoded verifier, or a bounded hashing failure.
    [[nodiscard]] Result<std::string, PasswordVerifierErrorCode>
    hash(std::span<const char> password) const;

    /// @brief Verifies plaintext against one prevalidated encoded Argon2id verifier.
    /// @param[in] password Temporary plaintext bytes to verify without retaining them.
    /// @param[in] encoded_verifier Algorithm, parameters, salt, and hash from a provider.
    /// @return Match result, or `invalid_verifier` for unsupported credential material.
    [[nodiscard]] Result<bool, PasswordVerifierErrorCode>
    verify(std::span<const char> password, std::string_view encoded_verifier) const noexcept;

    /// @brief Checks the verifier syntax, algorithm, and bounded parameter profile.
    /// @param[in] encoded_verifier Candidate provider material.
    /// @return `true` only for the currently supported Argon2id profile.
    [[nodiscard]] static bool is_usable(std::string_view encoded_verifier) noexcept;

  private:
    PasswordVerifier() = default;
};

} // namespace sparenode::authentication
