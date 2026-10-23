#include "sparenode/authentication/api/session_authentication_policy.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/http/http_status_code.hpp"
#include "sparenode/http/response/http_response.hpp"

namespace sparenode::authentication::api
{
namespace
{

struct CookieCredential
{
    std::string_view value; ///< Borrowed opaque credential when exactly one is present.
    bool malformed{};       ///< Duplicate or syntactically ambiguous cookie input.
};

/// @brief Compares an HTTP field name without locale-dependent case conversion.
[[nodiscard]] bool ascii_case_insensitive_equal(const std::string_view left,
                                                const std::string_view right) noexcept
{
    return left.size() == right.size() &&
           std::ranges::equal(left, right,
                              [](const unsigned char lhs, const unsigned char rhs)
                              {
                                  const auto lower = [](const unsigned char byte) {
                                      return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
                                  };
                                  return lower(lhs) == lower(rhs);
                              });
}

/// @brief Removes HTTP optional whitespace from one cookie component.
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

/// @brief Extracts exactly one unquoted session credential from Cookie fields.
[[nodiscard]] CookieCredential cookie_field_credential(std::string_view remaining) noexcept
{
    CookieCredential result;
    while (!remaining.empty())
    {
        const auto delimiter = remaining.find(';');
        const auto component = trim_optional_whitespace(remaining.substr(0, delimiter));
        const auto separator = component.find('=');
        if (separator != std::string_view::npos &&
            component.substr(0, separator) == session_cookie_name)
        {
            if (!result.value.empty())
            {
                return {{}, true};
            }
            result.value = component.substr(separator + 1);
            if (result.value.empty())
            {
                return {{}, true};
            }
        }
        if (delimiter == std::string_view::npos)
        {
            break;
        }
        remaining.remove_prefix(delimiter + 1);
    }
    return result;
}

/// @brief Rejects ambiguous multiple Cookie fields before selecting the session credential.
[[nodiscard]] CookieCredential session_credential(const http::HttpRequestView &request) noexcept
{
    bool found_cookie_field{};
    CookieCredential result;
    for (const auto &field : request.fields())
    {
        if (!ascii_case_insensitive_equal(field.name, "cookie"))
        {
            continue;
        }
        if (found_cookie_field)
        {
            return {{}, true};
        }
        found_cookie_field = true;
        result = cookie_field_credential(field.value);
    }
    return result;
}

/// @brief Converts stable JSON text into an owned HTTP response body.
[[nodiscard]] std::vector<std::byte> response_body(const std::string_view text)
{
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    return {bytes.begin(), bytes.end()};
}

/// @brief Creates the stable unauthenticated challenge without exposing credential details.
[[nodiscard]] Result<http::HttpResponse, http::HttpRouteError> authentication_required()
{
    auto response =
        http::HttpResponse::create(http::HttpStatusCode::unauthorized, "Unauthorized",
                                   {{"Content-Type", "application/json; charset=utf-8"},
                                    {"WWW-Authenticate", "SpareNodeSession realm=\"SpareNode\""}},
                                   response_body("{\"error\":\"authentication_required\"}"));
    if (!response)
    {
        return unexpected(
            http::HttpRouteError{http::HttpRouteErrorCode::response_validation_failure,
                                 static_cast<int>(response.error().code)});
    }
    return std::move(response).value();
}

/// @brief Creates a fail-closed response for unavailable in-memory session infrastructure.
[[nodiscard]] Result<http::HttpResponse, http::HttpRouteError> authentication_unavailable()
{
    auto response =
        http::HttpResponse::create(http::HttpStatusCode::service_unavailable, "Service Unavailable",
                                   {{"Content-Type", "application/json; charset=utf-8"}},
                                   response_body("{\"error\":\"authentication_unavailable\"}"));
    if (!response)
    {
        return unexpected(
            http::HttpRouteError{http::HttpRouteErrorCode::response_validation_failure,
                                 static_cast<int>(response.error().code)});
    }
    return std::move(response).value();
}

} // namespace

http::api::ApiRequestDecorator
make_session_authentication_policy(std::shared_ptr<session::SessionStore> sessions)
{
    return [sessions = std::move(sessions)](http::api::ApiRequestHandler next)
    {
        return http::api::ApiRequestHandler(
            [sessions, next = std::move(next)](http::api::ApiRequestContext &context)
                -> Result<http::HttpResponse, http::HttpRouteError>
            {
                if (sessions == nullptr)
                {
                    return authentication_unavailable();
                }
                const auto credential = session_credential(context.request);
                if (credential.malformed || credential.value.empty())
                {
                    return authentication_required();
                }
                auto principal = sessions->authenticate(credential.value);
                if (!principal)
                {
                    return authentication_unavailable();
                }
                if (!principal->has_value())
                {
                    return authentication_required();
                }
                context.authenticated_principal = std::move(principal).value().value();
                return next(context);
            });
    };
}

} // namespace sparenode::authentication::api
