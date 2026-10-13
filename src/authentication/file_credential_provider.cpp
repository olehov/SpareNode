#include "sparenode/authentication/file_credential_provider.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>
#include <string>
#include <utility>

#include "sparenode/configuration/config_lexer.hpp"
#include "sparenode/configuration/config_token.hpp"

namespace sparenode::authentication
{
namespace
{

using configuration::ConfigLexer;
using configuration::ConfigToken;
using configuration::ConfigTokenKind;

/// @brief Temporarily owns parsed credential values before semantic validation.
struct ParsedCredentialValues
{
    std::optional<std::string> login;            ///< Exact owner login from the file.
    std::optional<std::string> encoded_verifier; ///< Self-contained Argon2id verifier.
};

/// @brief Parses the small credential grammar from the shared UTF-8 token stream.
class CredentialSourceParser final
{
  public:
    /// @brief Creates parser state over one initialized lexer.
    /// @param[in,out] lexer Lexer transferred into single-pass parser ownership.
    explicit CredentialSourceParser(ConfigLexer lexer) noexcept : lexer_(std::move(lexer))
    {
    }

    /// @brief Parses exactly one owner block and no trailing content.
    /// @return Parsed values, or `malformed_file` for every syntax failure.
    [[nodiscard]] Result<ParsedCredentialValues, CredentialProvisioningErrorCode> parse()
    {
        if (!advance() || !is_identifier("owner") || !advance() ||
            !consume(ConfigTokenKind::left_brace))
        {
            return unexpected(CredentialProvisioningErrorCode::malformed_file);
        }

        ParsedCredentialValues values;
        while (current_.kind != ConfigTokenKind::right_brace)
        {
            if (current_.kind == ConfigTokenKind::end_of_input || !parse_directive(values))
            {
                return unexpected(CredentialProvisioningErrorCode::malformed_file);
            }
        }
        if (!advance() || current_.kind != ConfigTokenKind::end_of_input)
        {
            return unexpected(CredentialProvisioningErrorCode::malformed_file);
        }
        return values;
    }

  private:
    /// @brief Advances to the next token while collapsing lexical details at the secret boundary.
    /// @return `true` when another token, including end-of-input, was produced.
    [[nodiscard]] bool advance()
    {
        auto token = lexer_.next();
        if (!token)
        {
            return false;
        }
        current_ = std::move(token).value();
        return true;
    }

    /// @brief Consumes one required token kind.
    /// @param[in] kind Required lexical category.
    /// @return `true` after consuming the token and advancing.
    [[nodiscard]] bool consume(const ConfigTokenKind kind)
    {
        return current_.kind == kind && advance();
    }

    /// @brief Reports whether the current token is an exact identifier.
    /// @param[in] value Required case-sensitive spelling.
    /// @return `true` only for the selected identifier.
    [[nodiscard]] bool is_identifier(const std::string_view value) const noexcept
    {
        return current_.kind == ConfigTokenKind::identifier && current_.lexeme == value;
    }

    /// @brief Parses one unique login or password-verifier directive.
    /// @param[in,out] values Destination for the decoded directive value.
    /// @return `false` for unknown, duplicate, or structurally invalid directives.
    [[nodiscard]] bool parse_directive(ParsedCredentialValues &values)
    {
        if (current_.kind != ConfigTokenKind::identifier)
        {
            return false;
        }
        const auto name = current_.lexeme;
        if (!advance() || current_.kind != ConfigTokenKind::string_literal ||
            !current_.decoded_string.has_value())
        {
            return false;
        }
        auto value = std::move(*current_.decoded_string);
        if (!advance() || !consume(ConfigTokenKind::semicolon))
        {
            return false;
        }

        if (name == "login" && !values.login.has_value())
        {
            values.login = std::move(value);
            return true;
        }
        if (name == "password_verifier" && !values.encoded_verifier.has_value())
        {
            values.encoded_verifier = std::move(value);
            return true;
        }
        return false;
    }

    ConfigLexer lexer_;   ///< Single-pass shared configuration-language lexer.
    ConfigToken current_; ///< Most recently produced token.
};

#ifdef _WIN32

/// @brief Closes one credential file handle on every return path.
class CredentialFileHandle final
{
  public:
    explicit CredentialFileHandle(const HANDLE handle) noexcept : handle_(handle)
    {
    }

    ~CredentialFileHandle()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            static_cast<void>(CloseHandle(handle_));
        }
    }

    CredentialFileHandle(const CredentialFileHandle &) = delete;
    CredentialFileHandle &operator=(const CredentialFileHandle &) = delete;

    [[nodiscard]] HANDLE get() const noexcept
    {
        return handle_;
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE}; ///< Validated credential-file handle.
};

/// @brief Opens, validates, and reads the final Windows object through one stable handle.
[[nodiscard]] Result<std::string, CredentialProvisioningErrorCode>
read_credential_source(const std::filesystem::path &path)
{
    try
    {
        constexpr DWORD open_flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS |
                                     FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN;
        const auto native_handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                               OPEN_EXISTING, open_flags, nullptr);
        if (native_handle == INVALID_HANDLE_VALUE)
        {
            return unexpected(CredentialProvisioningErrorCode::open_failed);
        }
        const CredentialFileHandle handle(native_handle);

        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes,
                                         sizeof(attributes)) == FALSE)
        {
            return unexpected(CredentialProvisioningErrorCode::read_failed);
        }
        if ((attributes.FileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
        {
            return unexpected(CredentialProvisioningErrorCode::not_regular_file);
        }

        LARGE_INTEGER native_size{};
        if (GetFileSizeEx(handle.get(), &native_size) == FALSE || native_size.QuadPart < 0)
        {
            return unexpected(CredentialProvisioningErrorCode::read_failed);
        }
        if (static_cast<unsigned long long>(native_size.QuadPart) > maximum_credential_file_bytes)
        {
            return unexpected(CredentialProvisioningErrorCode::file_too_large);
        }

        std::string source(maximum_credential_file_bytes + 1, '\0');
        static_assert(maximum_credential_file_bytes + 1 <=
                      static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()));
        std::size_t bytes_read = 0;
        while (bytes_read < source.size())
        {
            DWORD received = 0;
            if (ReadFile(handle.get(), source.data() + bytes_read,
                         static_cast<DWORD>(source.size() - bytes_read), &received,
                         nullptr) == FALSE)
            {
                return unexpected(CredentialProvisioningErrorCode::read_failed);
            }
            if (received == 0)
            {
                break;
            }
            bytes_read += received;
        }
        if (bytes_read > maximum_credential_file_bytes)
        {
            return unexpected(CredentialProvisioningErrorCode::file_too_large);
        }
        source.resize(bytes_read);
        return source;
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(CredentialProvisioningErrorCode::resource_exhausted);
    }
    catch (...)
    {
        return unexpected(CredentialProvisioningErrorCode::read_failed);
    }
}

#else

/// @brief Closes one credential file descriptor on every return path.
class CredentialFileDescriptor final
{
  public:
    explicit CredentialFileDescriptor(const int descriptor) noexcept : descriptor_(descriptor)
    {
    }

    ~CredentialFileDescriptor()
    {
        if (descriptor_ >= 0)
        {
            static_cast<void>(::close(descriptor_));
        }
    }

    CredentialFileDescriptor(const CredentialFileDescriptor &) = delete;
    CredentialFileDescriptor &operator=(const CredentialFileDescriptor &) = delete;

    [[nodiscard]] int get() const noexcept
    {
        return descriptor_;
    }

  private:
    int descriptor_{-1}; ///< Validated credential-file descriptor.
};

/// @brief Opens, validates, and reads the final POSIX object through one stable descriptor.
[[nodiscard]] Result<std::string, CredentialProvisioningErrorCode>
read_credential_source(const std::filesystem::path &path)
{
    try
    {
        const int native_descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (native_descriptor < 0)
        {
            return unexpected(errno == ELOOP ? CredentialProvisioningErrorCode::not_regular_file
                                             : CredentialProvisioningErrorCode::open_failed);
        }
        const CredentialFileDescriptor descriptor(native_descriptor);

        struct stat attributes
        {
        };
        if (::fstat(descriptor.get(), &attributes) != 0)
        {
            return unexpected(CredentialProvisioningErrorCode::read_failed);
        }
        if (!S_ISREG(attributes.st_mode))
        {
            return unexpected(CredentialProvisioningErrorCode::not_regular_file);
        }
        if (attributes.st_size < 0 ||
            static_cast<unsigned long long>(attributes.st_size) > maximum_credential_file_bytes)
        {
            return unexpected(CredentialProvisioningErrorCode::file_too_large);
        }

        std::string source(maximum_credential_file_bytes + 1, '\0');
        std::size_t bytes_read = 0;
        while (bytes_read < source.size())
        {
            const auto received =
                ::read(descriptor.get(), source.data() + bytes_read, source.size() - bytes_read);
            if (received > 0)
            {
                bytes_read += static_cast<std::size_t>(received);
                continue;
            }
            if (received == 0)
            {
                break;
            }
            if (errno != EINTR)
            {
                return unexpected(CredentialProvisioningErrorCode::read_failed);
            }
        }
        if (bytes_read > maximum_credential_file_bytes)
        {
            return unexpected(CredentialProvisioningErrorCode::file_too_large);
        }
        source.resize(bytes_read);
        return source;
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(CredentialProvisioningErrorCode::resource_exhausted);
    }
    catch (...)
    {
        return unexpected(CredentialProvisioningErrorCode::read_failed);
    }
}

#endif

} // namespace

bool is_valid_owner_login(const std::string_view login) noexcept
{
    if (login.empty() || login.size() > maximum_owner_login_bytes)
    {
        return false;
    }
    constexpr unsigned char first_printable_ascii_byte = 0x20U;
    constexpr unsigned char delete_control_byte = 0x7FU;
    return std::ranges::none_of(
        login, [](const unsigned char byte)
        { return byte < first_printable_ascii_byte || byte == delete_control_byte; });
}

Result<FileCredentialProvider, CredentialProvisioningErrorCode>
FileCredentialProvider::load(const std::filesystem::path &path, const PasswordVerifier &verifier)
{
    auto source = read_credential_source(path);
    if (!source)
    {
        return unexpected(source.error());
    }
    return create_from_source(source.value(), verifier);
}

Result<FileCredentialProvider, CredentialProvisioningErrorCode>
FileCredentialProvider::create_from_source(const std::string_view source,
                                           const PasswordVerifier &verifier)
{
    try
    {
        if (source.size() > maximum_credential_file_bytes)
        {
            return unexpected(CredentialProvisioningErrorCode::file_too_large);
        }
        auto lexer = ConfigLexer::create(source);
        if (!lexer)
        {
            return unexpected(CredentialProvisioningErrorCode::malformed_file);
        }
        auto parsed = CredentialSourceParser(std::move(lexer).value()).parse();
        if (!parsed)
        {
            return unexpected(parsed.error());
        }
        auto values = std::move(parsed).value();
        if (!values.login.has_value())
        {
            return unexpected(CredentialProvisioningErrorCode::missing_login);
        }
        if (!values.encoded_verifier.has_value())
        {
            return unexpected(CredentialProvisioningErrorCode::missing_verifier);
        }
        if (!is_valid_owner_login(*values.login))
        {
            return unexpected(CredentialProvisioningErrorCode::invalid_login);
        }
        if (!verifier.is_usable(*values.encoded_verifier))
        {
            return unexpected(CredentialProvisioningErrorCode::invalid_verifier);
        }
        return FileCredentialProvider(CredentialRecord{"owner", std::move(*values.login),
                                                       std::move(*values.encoded_verifier)});
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(CredentialProvisioningErrorCode::resource_exhausted);
    }
}

FileCredentialProvider::FileCredentialProvider(CredentialRecord credential) noexcept
    : credential_(std::move(credential))
{
}

Result<std::optional<CredentialRecord>, CredentialProviderError>
FileCredentialProvider::find_by_login(const std::string_view login) const
{
    if (login != credential_.canonical_login)
    {
        return std::optional<CredentialRecord>{};
    }
    try
    {
        return std::optional<CredentialRecord>{credential_};
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(CredentialProviderError{CredentialProviderErrorCode::resource_exhausted});
    }
}

const char *to_string(const CredentialProvisioningErrorCode code) noexcept
{
    switch (code)
    {
    case CredentialProvisioningErrorCode::open_failed:
        return "unable to open the configured credential file";
    case CredentialProvisioningErrorCode::not_regular_file:
        return "configured credential source must be a regular file";
    case CredentialProvisioningErrorCode::read_failed:
        return "unable to read the configured credential file";
    case CredentialProvisioningErrorCode::file_too_large:
        return "credential file exceeds its size limit";
    case CredentialProvisioningErrorCode::malformed_file:
        return "credential file syntax is invalid";
    case CredentialProvisioningErrorCode::missing_login:
        return "credential file must contain one owner login";
    case CredentialProvisioningErrorCode::missing_verifier:
        return "credential file must contain one password verifier";
    case CredentialProvisioningErrorCode::invalid_login:
        return "owner login is empty, oversized, or contains control bytes";
    case CredentialProvisioningErrorCode::invalid_verifier:
        return "owner password verifier is malformed or unsupported";
    case CredentialProvisioningErrorCode::resource_exhausted:
        return "owner credential storage could not be allocated";
    }
    return "unknown owner credential provisioning error";
}

} // namespace sparenode::authentication
