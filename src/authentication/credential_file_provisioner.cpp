#ifdef _WIN32
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06000000
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// The SDDL declarations depend on Windows API family definitions from windows.h.
#include <sddl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "sparenode/authentication/credential_file_provisioner.hpp"

#include <cstddef>
#include <limits>
#include <new>
#include <string>
#include <utility>

#include "sparenode/authentication/file_credential_provider.hpp"

namespace sparenode::authentication
{
namespace
{

/// @brief Escapes one validated login for the shared quoted-string grammar.
[[nodiscard]] std::string escape_login(const std::string_view login)
{
    std::string escaped;
    escaped.reserve(login.size());
    for (const char byte : login)
    {
        if (byte == '\\' || byte == '"')
        {
            escaped.push_back('\\');
        }
        escaped.push_back(byte);
    }
    return escaped;
}

/// @brief Serializes the stable text representation consumed by FileCredentialProvider.
[[nodiscard]] std::string credential_source(const std::string_view login,
                                            const std::string_view encoded_verifier)
{
    return "owner {\n    login \"" + escape_login(login) + "\";\n    password_verifier \"" +
           std::string(encoded_verifier) + "\";\n}\n";
}

#ifdef _WIN32

/// @brief Creates a protected file ACL for the owner, SYSTEM, and administrators.
[[nodiscard]] PSECURITY_DESCRIPTOR create_credential_security_descriptor() noexcept
{
    constexpr wchar_t descriptor_source[] = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor_source, SDDL_REVISION_1,
                                                             &descriptor, nullptr) == FALSE)
    {
        return nullptr;
    }
    return descriptor;
}

/// @brief Exclusively creates and fully writes one Windows credential file.
[[nodiscard]] Result<void, CredentialFileProvisioningErrorCode>
write_exclusive_file(const std::filesystem::path &path, const std::string_view content) noexcept
{
    PSECURITY_DESCRIPTOR descriptor = create_credential_security_descriptor();
    if (descriptor == nullptr)
    {
        return unexpected(CredentialFileProvisioningErrorCode::open_failed);
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    const DWORD create_error = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    LocalFree(descriptor);
    if (handle == INVALID_HANDLE_VALUE)
    {
        if (create_error == ERROR_FILE_EXISTS || create_error == ERROR_ALREADY_EXISTS)
        {
            return unexpected(CredentialFileProvisioningErrorCode::output_exists);
        }
        return unexpected(CredentialFileProvisioningErrorCode::open_failed);
    }

    constexpr auto maximum_native_write =
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max());
    DWORD written = 0;
    const bool write_succeeded =
        content.size() <= maximum_native_write &&
        WriteFile(handle, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) !=
            FALSE &&
        static_cast<std::size_t>(written) == content.size() && FlushFileBuffers(handle) != FALSE;
    const bool close_succeeded = CloseHandle(handle) != FALSE;
    if (!write_succeeded || !close_succeeded)
    {
        DeleteFileW(path.c_str());
        return unexpected(CredentialFileProvisioningErrorCode::write_failed);
    }
    return {};
}

#else

/// @brief Persists creation of a credential-file directory entry on POSIX filesystems.
[[nodiscard]] bool sync_parent_directory(const std::filesystem::path &path) noexcept
{
    try
    {
        auto parent = path.parent_path();
        if (parent.empty())
        {
            parent = ".";
        }
        const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0)
        {
            return false;
        }
        const bool sync_succeeded = ::fsync(directory) == 0;
        const bool close_succeeded = ::close(directory) == 0;
        return sync_succeeded && close_succeeded;
    }
    catch (...)
    {
        return false;
    }
}

/// @brief Exclusively creates and fully writes one POSIX credential file with owner-only mode.
[[nodiscard]] Result<void, CredentialFileProvisioningErrorCode>
write_exclusive_file(const std::filesystem::path &path, const std::string_view content) noexcept
{
    constexpr mode_t owner_read_write = S_IRUSR | S_IWUSR;
    const int descriptor = ::open(
        path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, owner_read_write);
    if (descriptor < 0)
    {
        return unexpected(errno == EEXIST ? CredentialFileProvisioningErrorCode::output_exists
                                          : CredentialFileProvisioningErrorCode::open_failed);
    }

    std::size_t offset = 0;
    bool write_succeeded = true;
    while (offset < content.size())
    {
        const auto written = ::write(descriptor, content.data() + offset, content.size() - offset);
        if (written > 0)
        {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
        {
            continue;
        }
        write_succeeded = false;
        break;
    }
    if (write_succeeded && ::fsync(descriptor) != 0)
    {
        write_succeeded = false;
    }
    if (::close(descriptor) != 0)
    {
        write_succeeded = false;
    }
    if (write_succeeded && !sync_parent_directory(path))
    {
        write_succeeded = false;
    }
    if (!write_succeeded)
    {
        ::unlink(path.c_str());
        return unexpected(CredentialFileProvisioningErrorCode::write_failed);
    }
    return {};
}

#endif

/// @brief Maps password-verifier failures into provisioning-tool categories.
[[nodiscard]] CredentialFileProvisioningErrorCode
map_verifier_error(const PasswordVerifierErrorCode code) noexcept
{
    switch (code)
    {
    case PasswordVerifierErrorCode::empty_password:
        return CredentialFileProvisioningErrorCode::empty_password;
    case PasswordVerifierErrorCode::password_too_large:
        return CredentialFileProvisioningErrorCode::password_too_large;
    case PasswordVerifierErrorCode::resource_exhausted:
        return CredentialFileProvisioningErrorCode::resource_exhausted;
    case PasswordVerifierErrorCode::initialization_failed:
    case PasswordVerifierErrorCode::invalid_verifier:
    case PasswordVerifierErrorCode::hashing_failed:
        return CredentialFileProvisioningErrorCode::hashing_failed;
    }
    return CredentialFileProvisioningErrorCode::hashing_failed;
}

} // namespace

Result<void, CredentialFileProvisioningErrorCode> CredentialFileProvisioner::provision(
    const std::filesystem::path &output_path, const std::string_view login,
    SensitivePassword &&submitted_password, const PasswordVerifier &verifier)
{
    SensitivePassword password(std::move(submitted_password));
    if (!is_valid_owner_login(login))
    {
        return unexpected(CredentialFileProvisioningErrorCode::invalid_login);
    }

    auto encoded = verifier.hash(password.bytes());
    if (!encoded)
    {
        return unexpected(map_verifier_error(encoded.error()));
    }

    try
    {
        const auto source = credential_source(login, encoded.value());
        return write_exclusive_file(output_path, source);
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(CredentialFileProvisioningErrorCode::resource_exhausted);
    }
}

const char *to_string(const CredentialFileProvisioningErrorCode code) noexcept
{
    switch (code)
    {
    case CredentialFileProvisioningErrorCode::invalid_login:
        return "login is empty, oversized, or contains control bytes";
    case CredentialFileProvisioningErrorCode::empty_password:
        return "password must not be empty";
    case CredentialFileProvisioningErrorCode::password_too_large:
        return "password exceeds the supported size limit";
    case CredentialFileProvisioningErrorCode::output_exists:
        return "credential file already exists and was not replaced";
    case CredentialFileProvisioningErrorCode::open_failed:
        return "unable to create the credential file";
    case CredentialFileProvisioningErrorCode::write_failed:
        return "unable to persist the complete credential file";
    case CredentialFileProvisioningErrorCode::hashing_failed:
        return "password hashing could not complete";
    case CredentialFileProvisioningErrorCode::resource_exhausted:
        return "credential provisioning resources could not be allocated";
    }
    return "unknown credential provisioning error";
}

} // namespace sparenode::authentication
