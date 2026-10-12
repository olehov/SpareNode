#include "sparenode/authentication/file_authentication.hpp"

#include <memory>
#include <new>
#include <span>
#include <system_error>
#include <utility>

#include "sparenode/authentication/file_credential_provider.hpp"
#include "sparenode/authentication/password_verifier.hpp"

namespace sparenode::authentication
{
namespace
{

/// @brief Rejects a credential file reachable below any configured filesystem location.
[[nodiscard]] Result<bool, FileAuthenticationErrorCode>
credential_is_shared(const std::filesystem::path &credential_path,
                     const std::span<const configuration::runtime::LocationConfig> locations)
{
    std::error_code path_error;
    const auto canonical_credential = std::filesystem::canonical(credential_path, path_error);
    if (path_error)
    {
        return unexpected(FileAuthenticationErrorCode::credential_open_failed);
    }

    for (const auto &location : locations)
    {
        auto current = canonical_credential.parent_path();
        while (!current.empty())
        {
            path_error.clear();
            if (std::filesystem::equivalent(current, location.root().path(), path_error))
            {
                return true;
            }
            if (path_error)
            {
                return unexpected(FileAuthenticationErrorCode::credential_read_failed);
            }
            const auto parent = current.parent_path();
            if (parent == current)
            {
                break;
            }
            current = parent;
        }
    }
    return false;
}

/// @brief Maps concrete file-provisioning failures into the application startup contract.
[[nodiscard]] FileAuthenticationErrorCode
map_provisioning_error(const CredentialProvisioningErrorCode code) noexcept
{
    switch (code)
    {
    case CredentialProvisioningErrorCode::open_failed:
        return FileAuthenticationErrorCode::credential_open_failed;
    case CredentialProvisioningErrorCode::not_regular_file:
        return FileAuthenticationErrorCode::credential_not_regular;
    case CredentialProvisioningErrorCode::read_failed:
        return FileAuthenticationErrorCode::credential_read_failed;
    case CredentialProvisioningErrorCode::file_too_large:
        return FileAuthenticationErrorCode::credential_file_too_large;
    case CredentialProvisioningErrorCode::malformed_file:
        return FileAuthenticationErrorCode::malformed_credential_file;
    case CredentialProvisioningErrorCode::missing_login:
        return FileAuthenticationErrorCode::missing_owner_login;
    case CredentialProvisioningErrorCode::missing_verifier:
        return FileAuthenticationErrorCode::missing_owner_verifier;
    case CredentialProvisioningErrorCode::invalid_login:
        return FileAuthenticationErrorCode::invalid_owner_login;
    case CredentialProvisioningErrorCode::invalid_verifier:
        return FileAuthenticationErrorCode::invalid_owner_verifier;
    case CredentialProvisioningErrorCode::resource_exhausted:
        return FileAuthenticationErrorCode::resource_exhausted;
    }
    return FileAuthenticationErrorCode::credential_read_failed;
}

} // namespace

Result<AuthenticationService, FileAuthenticationErrorCode> create_file_authentication_service(
    const std::filesystem::path &credential_path,
    const std::span<const configuration::runtime::LocationConfig> locations)
{
    auto verifier = PasswordVerifier::create();
    if (!verifier)
    {
        return unexpected(FileAuthenticationErrorCode::cryptography_unavailable);
    }

    const auto shared = credential_is_shared(credential_path, locations);
    if (!shared)
    {
        return unexpected(shared.error());
    }
    if (shared.value())
    {
        return unexpected(FileAuthenticationErrorCode::credential_is_shared);
    }

    auto provider = FileCredentialProvider::load(credential_path, verifier.value());
    if (!provider)
    {
        return unexpected(map_provisioning_error(provider.error()));
    }

    try
    {
        auto provider_owner = std::make_shared<FileCredentialProvider>(std::move(provider).value());
        auto service = AuthenticationService::create(std::move(provider_owner), verifier.value());
        if (!service)
        {
            return unexpected(FileAuthenticationErrorCode::resource_exhausted);
        }
        return std::move(service).value();
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(FileAuthenticationErrorCode::resource_exhausted);
    }
}

const char *to_string(const FileAuthenticationErrorCode code) noexcept
{
    switch (code)
    {
    case FileAuthenticationErrorCode::cryptography_unavailable:
        return "password cryptography could not be initialized";
    case FileAuthenticationErrorCode::credential_open_failed:
        return "unable to open the configured credential file";
    case FileAuthenticationErrorCode::credential_not_regular:
        return "configured credential source must be a regular file";
    case FileAuthenticationErrorCode::credential_is_shared:
        return "credential file must be outside every configured filesystem location";
    case FileAuthenticationErrorCode::credential_read_failed:
        return "unable to read the configured credential file";
    case FileAuthenticationErrorCode::credential_file_too_large:
        return "credential file exceeds its size limit";
    case FileAuthenticationErrorCode::malformed_credential_file:
        return "credential file syntax is invalid";
    case FileAuthenticationErrorCode::missing_owner_login:
        return "credential file must contain one owner login";
    case FileAuthenticationErrorCode::missing_owner_verifier:
        return "credential file must contain one password verifier";
    case FileAuthenticationErrorCode::invalid_owner_login:
        return "owner login is empty, oversized, or contains control bytes";
    case FileAuthenticationErrorCode::invalid_owner_verifier:
        return "owner password verifier is malformed or unsupported";
    case FileAuthenticationErrorCode::resource_exhausted:
        return "authentication resources could not be allocated";
    }
    return "unknown authentication startup error";
}

} // namespace sparenode::authentication
