#include "sparenode/authentication/api/authentication_api.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/authentication/api/session_authentication_policy.hpp"
#include "sparenode/authentication/file_credential_provider.hpp"
#include "sparenode/authentication/password_verifier.hpp"
#include "sparenode/foundation/text/hexadecimal.hpp"
#include "sparenode/foundation/text/utf8.hpp"
#include "sparenode/http/detail/ascii.hpp"
#include "sparenode/http/http_status_code.hpp"
#include "sparenode/http/response/http_response.hpp"

namespace sparenode::authentication::api
{
namespace
{

/// @brief Owns validated credential values before the application service consumes them.
struct LoginRequest
{
    std::string login;          ///< Bounded owner login identifier.
    SensitivePassword password; ///< Erasable plaintext password storage.
};

/// @brief Erases parser-owned password text on normal and exceptional exits.
class SensitiveTextEraser final
{
  public:
    explicit SensitiveTextEraser(std::string &text) noexcept : text_(text)
    {
    }

    ~SensitiveTextEraser()
    {
        erase_sensitive_bytes(std::span(text_.data(), text_.size()));
    }

    SensitiveTextEraser(const SensitiveTextEraser &) = delete;
    SensitiveTextEraser &operator=(const SensitiveTextEraser &) = delete;

  private:
    std::string &text_; ///< Parser buffer erased before its allocation is released.
};

/// @brief Parses only the two-string login JSON schema from one bounded memory body.
class LoginJsonParser final
{
  public:
    explicit LoginJsonParser(const std::span<const std::byte> body) noexcept
        : input_(reinterpret_cast<const char *>(body.data()), body.size())
    {
    }

    [[nodiscard]] std::optional<LoginRequest> parse()
    {
        if (!text::utf8::is_valid(input_))
        {
            return std::nullopt;
        }
        skip_whitespace();
        if (!consume('{'))
        {
            return std::nullopt;
        }

        std::optional<std::string> login;
        std::optional<SensitivePassword> password;
        skip_whitespace();
        if (consume('}'))
        {
            return std::nullopt;
        }
        while (true)
        {
            if (!parse_member(login, password))
            {
                return std::nullopt;
            }

            skip_whitespace();
            if (consume('}'))
            {
                break;
            }
            if (!consume(','))
            {
                return std::nullopt;
            }
            skip_whitespace();
        }
        skip_whitespace();
        if (position_ != input_.size() || !login || !password)
        {
            return std::nullopt;
        }
        return LoginRequest{std::move(*login), std::move(*password)};
    }

  private:
    /// @brief Parses one unique login or password member into bounded storage.
    [[nodiscard]] bool parse_member(std::optional<std::string> &login,
                                    std::optional<SensitivePassword> &password)
    {
        auto name = parse_string();
        skip_whitespace();
        if (!name || !consume(':'))
        {
            return false;
        }
        skip_whitespace();
        auto value = parse_string();
        if (!value || value->empty())
        {
            return false;
        }
        if (*name == "login" && !login && value->size() <= maximum_owner_login_bytes)
        {
            login = std::move(*value);
            return true;
        }
        if (*name != "password")
        {
            return false;
        }
        const SensitiveTextEraser erase_password(*value);
        const bool valid_password = !password && value->size() <= maximum_password_bytes;
        if (valid_password)
        {
            password.emplace(std::vector<char>(value->begin(), value->end()));
        }
        return valid_password;
    }

    [[nodiscard]] bool consume(const char expected) noexcept
    {
        if (position_ >= input_.size() || input_[position_] != expected)
        {
            return false;
        }
        ++position_;
        return true;
    }

    void skip_whitespace() noexcept
    {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\r' || input_[position_] == '\n'))
        {
            ++position_;
        }
    }

    [[nodiscard]] std::optional<unsigned int> parse_hex_quad() noexcept
    {
        if (input_.size() - position_ < 4)
        {
            return std::nullopt;
        }
        unsigned int code_point{};
        for (std::size_t index = 0; index < 4; ++index)
        {
            const auto digit = text::hexadecimal::digit_value(input_[position_++]);
            if (!digit)
            {
                return std::nullopt;
            }
            code_point = (code_point << 4U) | *digit;
        }
        return code_point;
    }

    [[nodiscard]] std::optional<unsigned int> parse_unicode_escape() noexcept
    {
        auto code_point = parse_hex_quad();
        if (!code_point)
        {
            return std::nullopt;
        }
        constexpr unsigned int high_surrogate_first = 0xD800U;
        constexpr unsigned int high_surrogate_last = 0xDBFFU;
        constexpr unsigned int low_surrogate_first = 0xDC00U;
        constexpr unsigned int low_surrogate_last = 0xDFFFU;
        if (*code_point >= high_surrogate_first && *code_point <= high_surrogate_last)
        {
            if (!consume('\\') || !consume('u'))
            {
                return std::nullopt;
            }
            const auto low = parse_hex_quad();
            if (!low || *low < low_surrogate_first || *low > low_surrogate_last)
            {
                return std::nullopt;
            }
            constexpr unsigned int supplementary_base = 0x10000U;
            constexpr unsigned int surrogate_payload_bits = 10U;
            return supplementary_base +
                   ((*code_point - high_surrogate_first) << surrogate_payload_bits) +
                   (*low - low_surrogate_first);
        }
        if (*code_point >= low_surrogate_first && *code_point <= low_surrogate_last)
        {
            return std::nullopt;
        }
        return code_point;
    }

    /// @brief Decodes one escape after its leading backslash.
    [[nodiscard]] bool append_escape(std::string &output)
    {
        if (position_ >= input_.size())
        {
            return false;
        }
        const char escaped = input_[position_++];
        constexpr std::array simple_escapes{std::pair{'"', '"'},  std::pair{'\\', '\\'},
                                            std::pair{'/', '/'},  std::pair{'b', '\b'},
                                            std::pair{'f', '\f'}, std::pair{'n', '\n'},
                                            std::pair{'r', '\r'}, std::pair{'t', '\t'}};
        const auto simple = std::ranges::find_if(simple_escapes, [escaped](const auto &mapping)
                                                 { return mapping.first == escaped; });
        if (simple != simple_escapes.end())
        {
            output.push_back(simple->second);
            return true;
        }
        if (escaped != 'u')
        {
            return false;
        }
        const auto code_point = parse_unicode_escape();
        if (!code_point)
        {
            return false;
        }
        return text::utf8::append_code_point(output, static_cast<char32_t>(*code_point));
    }

    [[nodiscard]] std::optional<std::string> parse_string()
    {
        if (!consume('"'))
        {
            return std::nullopt;
        }
        constexpr unsigned char first_unescaped_json_byte = 0x20U;
        std::string output;
        while (position_ < input_.size())
        {
            const auto byte = static_cast<unsigned char>(input_[position_++]);
            if (byte == '"')
            {
                return output;
            }
            if (byte < first_unescaped_json_byte)
            {
                return std::nullopt;
            }
            if (byte != '\\')
            {
                output.push_back(static_cast<char>(byte));
                continue;
            }
            if (!append_escape(output))
            {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::string_view input_; ///< Complete bounded request payload.
    std::size_t position_{}; ///< Next unconsumed payload byte.
};

[[nodiscard]] std::string_view trim_optional_whitespace(std::string_view value) noexcept
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
    {
        value.remove_suffix(1);
    }
    return value;
}

/// @brief Accepts exactly one JSON media type with an optional UTF-8 charset parameter.
[[nodiscard]] bool has_json_content_type(const http::HttpRequestView &request) noexcept
{
    std::optional<std::string_view> value;
    for (const auto &field : request.fields())
    {
        if (!http::detail::ascii_case_insensitive_equal(field.name, "Content-Type"))
        {
            continue;
        }
        if (value)
        {
            return false;
        }
        value = field.value;
    }
    if (!value)
    {
        return false;
    }
    const auto separator = value->find(';');
    const auto media_type = trim_optional_whitespace(value->substr(0, separator));
    if (!http::detail::ascii_case_insensitive_equal(media_type, "application/json"))
    {
        return false;
    }
    if (separator == std::string_view::npos)
    {
        return true;
    }
    auto parameter = trim_optional_whitespace(value->substr(separator + 1));
    const auto equals = parameter.find('=');
    if (equals == std::string_view::npos || parameter.find(';') != std::string_view::npos)
    {
        return false;
    }
    const auto name = trim_optional_whitespace(parameter.substr(0, equals));
    const auto charset = trim_optional_whitespace(parameter.substr(equals + 1));
    return http::detail::ascii_case_insensitive_equal(name, "charset") &&
           http::detail::ascii_case_insensitive_equal(charset, "utf-8");
}

[[nodiscard]] std::vector<std::byte> response_body(const std::string_view text)
{
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    return {bytes.begin(), bytes.end()};
}

[[nodiscard]] Result<http::HttpResponse, http::HttpRouteError>
json_response(const http::HttpStatusCode status, std::string reason, const std::string_view body)
{
    auto response = http::HttpResponse::create(
        status, std::move(reason), {{"Content-Type", "application/json; charset=utf-8"}},
        response_body(body));
    if (!response)
    {
        return unexpected(
            http::HttpRouteError{http::HttpRouteErrorCode::response_validation_failure,
                                 static_cast<int>(response.error().code)});
    }
    return std::move(response).value();
}

[[nodiscard]] Result<http::HttpResponse, http::HttpRouteError> malformed_login()
{
    return json_response(http::HttpStatusCode::bad_request, "Bad Request",
                         "{\"error\":\"invalid_login_request\"}");
}

[[nodiscard]] Result<http::HttpResponse, http::HttpRouteError> unsupported_media_type()
{
    return json_response(http::HttpStatusCode::unsupported_media_type, "Unsupported Media Type",
                         "{\"error\":\"unsupported_media_type\"}");
}

[[nodiscard]] Result<http::HttpResponse, http::HttpRouteError> invalid_credentials()
{
    return json_response(http::HttpStatusCode::unauthorized, "Unauthorized",
                         "{\"error\":\"invalid_credentials\"}");
}

[[nodiscard]] Result<http::HttpResponse, http::HttpRouteError> authentication_unavailable()
{
    return json_response(http::HttpStatusCode::service_unavailable, "Service Unavailable",
                         "{\"error\":\"authentication_unavailable\"}");
}

[[nodiscard]] Result<http::HttpResponse, http::HttpRouteError>
login(const http::HttpRequestView &request,
      const application::SessionAuthenticationService &service)
{
    if (!has_json_content_type(request))
    {
        return unsupported_media_type();
    }
    if (request.body_size() == 0 || request.body_size() > maximum_login_body_bytes ||
        request.body().size() != request.body_size() || request.temporary_body())
    {
        return malformed_login();
    }
    auto parsed = LoginJsonParser(request.body()).parse();
    if (!parsed)
    {
        return malformed_login();
    }
    const auto previous_session = session_cookie_credential(request);
    if (previous_session.malformed)
    {
        return malformed_login();
    }
    auto authenticated =
        service.login(parsed->login, std::move(parsed->password), previous_session.value);
    if (!authenticated)
    {
        return authenticated.error() == application::SessionLoginErrorCode::invalid_credentials
                   ? invalid_credentials()
                   : authentication_unavailable();
    }
    auto response = http::HttpResponse::create(
        http::HttpStatusCode::no_content, "No Content",
        {{"Set-Cookie",
          "spnode_session=" + std::move(authenticated).value() + "; Path=/; HttpOnly"}},
        {});
    if (!response)
    {
        return unexpected(
            http::HttpRouteError{http::HttpRouteErrorCode::response_validation_failure,
                                 static_cast<int>(response.error().code)});
    }
    return std::move(response).value();
}

} // namespace

Result<void, AuthenticationApiError> register_authentication_api_routes(
    http::api::ApiRouterBuilder &builder,
    std::shared_ptr<const application::SessionAuthenticationService> service)
{
    if (!service)
    {
        return unexpected(AuthenticationApiError{AuthenticationApiErrorCode::missing_service, {}});
    }
    try
    {
        auto registered = builder.register_public(
            {http::HttpMethod::post, std::string(login_path),
             [service = std::move(service)](const http::HttpRequestView &request,
                                            const http::HttpRouteParameters &)
             {
                 try
                 {
                     return login(request, *service);
                 }
                 catch (const std::bad_alloc &)
                 {
                     return authentication_unavailable();
                 }
             }});
        if (!registered)
        {
            return unexpected(AuthenticationApiError{
                AuthenticationApiErrorCode::route_registration_failed, registered.error()});
        }
        return {};
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            AuthenticationApiError{AuthenticationApiErrorCode::resource_allocation_failed, {}});
    }
}

http::RequestBodyStoragePolicy
authentication_body_storage_policy(const http::HttpRequestHead &head) noexcept
{
    const auto query = head.target.find('?');
    const auto path = head.target.substr(0, query);
    if (head.method == http::HttpMethod::post && path == login_path)
    {
        return {http::RequestBodyStorageMode::memory, maximum_login_body_bytes};
    }
    return {};
}

const char *to_string(const AuthenticationApiErrorCode code) noexcept
{
    switch (code)
    {
    case AuthenticationApiErrorCode::missing_service:
        return "authentication API requires a session service";
    case AuthenticationApiErrorCode::route_registration_failed:
        return "authentication API route registration failed";
    case AuthenticationApiErrorCode::resource_allocation_failed:
        return "authentication API memory allocation failed";
    }
    return "unknown authentication API error";
}

} // namespace sparenode::authentication::api
