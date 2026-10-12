#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

#include "sparenode/authentication/password_verifier.hpp"
#include "sparenode/authentication/sensitive_password.hpp"
#include "sparenode/result.hpp"

namespace sparenode::authentication
{

/// @brief Identifies why an owner credential file could not be provisioned safely.
enum class CredentialFileProvisioningErrorCode : std::uint8_t
{
    invalid_login,      ///< Login violates the file provider's identifier boundary.
    empty_password,     ///< An empty owner password was supplied.
    password_too_large, ///< Password exceeds the authentication boundary.
    output_exists,      ///< Refusing to replace an existing credential file.
    open_failed,        ///< A new credential file could not be created exclusively.
    write_failed,       ///< The complete credential file could not be persisted.
    hashing_failed,     ///< Argon2id hashing could not complete safely.
    resource_exhausted, ///< Temporary bounded provisioning storage could not be allocated.
};

/// @brief Creates one persistent text credential file without retaining plaintext passwords.
class CredentialFileProvisioner final
{
  public:
    /// @brief Hashes a password and exclusively creates a validated credential file.
    /// @param[in] output_path New file path that must not already exist.
    /// @param[in] login Owner login written through configuration string escaping.
    /// @param[in,out] password Plaintext owner consumed and erased on return.
    /// @param[in] verifier Initialized Argon2id operation boundary.
    /// @return Success, or a non-sensitive provisioning failure.
    [[nodiscard]] static Result<void, CredentialFileProvisioningErrorCode>
    provision(const std::filesystem::path &output_path, std::string_view login,
              SensitivePassword password, const PasswordVerifier &verifier);
};

/// @brief Returns a non-sensitive diagnostic for credential-file provisioning.
/// @param[in] code Failure category to describe.
/// @return Static English text suitable for the interactive tool.
[[nodiscard]] const char *to_string(CredentialFileProvisioningErrorCode code) noexcept;

} // namespace sparenode::authentication
