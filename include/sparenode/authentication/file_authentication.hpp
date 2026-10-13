#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

#include "sparenode/authentication/authentication_service.hpp"
#include "sparenode/configuration/runtime/location_config.hpp"

namespace sparenode::authentication
{

/// @brief Identifies why the MVP file-backed authentication stack cannot start.
enum class FileAuthenticationErrorCode : std::uint8_t
{
    cryptography_unavailable,  ///< libsodium initialization failed.
    credential_open_failed,    ///< The configured credential file could not be opened.
    credential_not_regular,    ///< The configured credential source is not a regular file.
    credential_is_shared,      ///< A filesystem location could expose the credential file.
    credential_read_failed,    ///< The credential file could not be read completely.
    credential_file_too_large, ///< The credential file exceeds its bounded size.
    malformed_credential_file, ///< Credential file syntax is invalid.
    missing_owner_login,       ///< Required owner login material is absent.
    missing_owner_verifier,    ///< Required encoded password verifier is absent.
    invalid_owner_login,       ///< Owner login violates the bounded identifier contract.
    invalid_owner_verifier,    ///< Password verifier is malformed or unsupported.
    resource_exhausted,        ///< Authentication state could not be allocated.
};

/// @brief Builds the complete MVP authentication stack from a separate credential file.
/// @param[in] credential_path File selected by validated runtime configuration.
/// @param[in] locations Filesystem roots that must not contain the credential file.
/// @return Ready source-independent service, or a non-sensitive startup failure.
[[nodiscard]] Result<AuthenticationService, FileAuthenticationErrorCode>
create_file_authentication_service(
    const std::filesystem::path &credential_path,
    std::span<const configuration::runtime::LocationConfig> locations);

/// @brief Returns a non-sensitive startup diagnostic for one file-auth failure.
/// @param[in] code Failure category to describe.
/// @return Static English text that never contains credential values.
[[nodiscard]] const char *to_string(FileAuthenticationErrorCode code) noexcept;

} // namespace sparenode::authentication
