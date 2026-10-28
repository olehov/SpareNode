#include "sparenode/authentication/api/session_authentication_policy.hpp"

#include <string_view>
#include <utility>

#include "sparenode/http/api/json_response.hpp"
#include "sparenode/http/detail/ascii.hpp"
#include "sparenode/http/http_status_code.hpp"
#include "sparenode/http/response/http_response.hpp"

namespace sparenode::authentication::api
{
namespace
{

/// @brief Extracts exactly one unquoted session credential from Cookie fields.
[[nodiscard]] SessionCookieCredential cookie_field_credential(std::string_view remaining) noexcept
{
    SessionCookieCredential result;
    while (!remaining.empty())
    {
        const auto delimiter = remaining.find(';');
        const auto component =
            http::detail::trim_optional_whitespace(remaining.substr(0, delimiter));
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
[[nodiscard]] SessionCookieCredential
extract_session_cookie_credential(const http::HttpRequestView &request) noexcept
{
    bool found_cookie_field{};
    SessionCookieCredential result;
    for (const auto &field : request.fields())
    {
        if (!http::detail::ascii_case_insensitive_equal(field.name, "cookie"))
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

/// @brief Creates the stable unauthenticated challenge without exposing credential details.
[[nodiscard]] Result<http::HttpResponse, http::HttpRouteError> authentication_required()
{
    return http::api::make_json_response(
        http::HttpStatusCode::unauthorized, "{\"error\":\"authentication_required\"}",
        {{"WWW-Authenticate", "SpareNodeSession realm=\"SpareNode\""}});
}

/// @brief Creates a fail-closed response for unavailable in-memory session infrastructure.
[[nodiscard]] Result<http::HttpResponse, http::HttpRouteError> authentication_unavailable()
{
    return http::api::make_json_response(http::HttpStatusCode::service_unavailable,
                                         "{\"error\":\"authentication_unavailable\"}");
}

} // namespace

SessionCookieCredential session_cookie_credential(const http::HttpRequestView &request) noexcept
{
    return extract_session_cookie_credential(request);
}

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
                const auto credential = session_cookie_credential(context.request);
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
