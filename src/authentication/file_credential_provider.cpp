#include "sparenode/authentication/file_credential_provider.hpp"

#include <algorithm>
#include <fstream>
#include <new>
#include <system_error>
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

/// @brief Reads a credential file through a fixed allocation that detects oversized input.
[[nodiscard]] Result<std::string, CredentialProvisioningErrorCode>
read_credential_source(const std::filesystem::path &path)
{
    try
    {
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(path, status_error);
        if (status_error || !std::filesystem::exists(status))
        {
            return unexpected(CredentialProvisioningErrorCode::open_failed);
        }
        if (!std::filesystem::is_regular_file(status))
        {
            return unexpected(CredentialProvisioningErrorCode::not_regular_file);
        }

        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            return unexpected(CredentialProvisioningErrorCode::open_failed);
        }

        std::string source(maximum_credential_file_bytes + 1, '\0');
        input.read(source.data(), static_cast<std::streamsize>(source.size()));
        const auto bytes_read = static_cast<std::size_t>(input.gcount());
        if (input.bad())
        {
            return unexpected(CredentialProvisioningErrorCode::read_failed);
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
