#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "sparenode/authentication/credential_provider.hpp"
#include "sparenode/authentication/password_verifier.hpp"

namespace sparenode::authentication
{

/// @brief Maximum byte length of the MVP owner login identifier.
inline constexpr std::size_t maximum_owner_login_bytes = 128;

/// @brief Maximum encoded size accepted for one credential file.
inline constexpr std::size_t maximum_credential_file_bytes = 4096;

/// @brief Validates one owner login using the file-provider identifier contract.
/// @param[in] login Candidate login bytes.
/// @return `true` for a nonempty bounded value without ASCII control bytes.
[[nodiscard]] bool is_valid_owner_login(std::string_view login) noexcept;

/// @brief Identifies why a file-backed owner credential is unusable.
enum class CredentialProvisioningErrorCode : std::uint8_t
{
    open_failed,        ///< The configured credential file could not be opened.
    not_regular_file,   ///< The configured credential source is not a regular file.
    read_failed,        ///< The credential file could not be read completely.
    file_too_large,     ///< The credential file exceeds its explicit byte boundary.
    malformed_file,     ///< The credential file does not follow its strict grammar.
    missing_login,      ///< The credential file contains no owner login.
    missing_verifier,   ///< The credential file contains no encoded verifier.
    invalid_login,      ///< The login is empty, oversized, or contains control bytes.
    invalid_verifier,   ///< The verifier is malformed or uses an unsupported profile.
    resource_exhausted, ///< Credential parsing or storage could not allocate memory.
};

/// @brief Provides exactly one owner credential loaded from a separate bounded file.
class FileCredentialProvider final : public CredentialProvider
{
  public:
    /// @brief Loads and validates one configured owner credential file.
    /// @param[in] path Credential file selected by validated runtime configuration.
    /// @param[in] verifier Validator for the supported encoded Argon2id profile.
    /// @return Ready single-owner provider, or a non-sensitive provisioning failure.
    [[nodiscard]] static Result<FileCredentialProvider, CredentialProvisioningErrorCode>
    load(const std::filesystem::path &path, const PasswordVerifier &verifier);

    /// @brief Parses already-read credential source through the same strict grammar.
    /// @param[in] source Complete bounded UTF-8 credential file contents.
    /// @param[in] verifier Validator for the supported encoded Argon2id profile.
    /// @return Ready provider, or a non-sensitive parsing or validation failure.
    [[nodiscard]] static Result<FileCredentialProvider, CredentialProvisioningErrorCode>
    create_from_source(std::string_view source, const PasswordVerifier &verifier);

    /// @copydoc CredentialProvider::find_by_login
    [[nodiscard]] Result<std::optional<CredentialRecord>, CredentialProviderError>
    find_by_login(std::string_view login) const override;

  private:
    /// @brief Stores the one fully validated owner credential.
    /// @param[in,out] credential Credential transferred into immutable provider state.
    explicit FileCredentialProvider(CredentialRecord credential) noexcept;

    CredentialRecord credential_; ///< Single validated owner record.
};

/// @brief Returns a stable diagnostic for a provisioning failure without secret values.
/// @param[in] code Failure category to describe.
/// @return Static English text suitable for the startup boundary.
[[nodiscard]] const char *to_string(CredentialProvisioningErrorCode code) noexcept;

} // namespace sparenode::authentication
