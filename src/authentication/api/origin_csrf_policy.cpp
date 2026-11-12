#include "sparenode/authentication/api/origin_csrf_policy.hpp"

#include <string_view>
#include <utility>

#include "sparenode/http/api/json_response.hpp"
#include "sparenode/http/detail/ascii.hpp"
#include "sparenode/http/http_status_code.hpp"
#include "sparenode/http/request/detail/host_authority.hpp"

namespace sparenode::authentication::api
{
namespace
{

/// @brief Preserves whether one security-sensitive header is absent, unique, or ambiguous.
struct UniqueHeader
{
    std::string_view value; ///< Borrowed unique field value when present.
    bool present{};         ///< Whether one matching field was observed.
    bool ambiguous{};       ///< Whether more than one matching field was observed.
};

/// @brief Extracts exactly one field without accepting comma-merging for security metadata.
[[nodiscard]] UniqueHeader unique_header(const http::HttpRequestView &request,
                                         const std::string_view name) noexcept
{
    UniqueHeader result;
    for (const auto &field : request.fields())
    {
        if (!http::detail::ascii_case_insensitive_equal(field.name, name))
        {
            continue;
        }
        if (result.present)
        {
            result.ambiguous = true;
            return result;
        }
        result.value = field.value;
        result.present = true;
    }
    return result;
}

/// @brief Reports whether the HTTP method can mutate authenticated application state.
[[nodiscard]] bool is_state_changing(const http::HttpMethod method) noexcept
{
    return method == http::HttpMethod::post || method == http::HttpMethod::put ||
           method == http::HttpMethod::delete_method;
}

/// @brief Splits one validated authority into a host and optional port.
struct AuthorityParts
{
    std::string_view host; ///< DNS name or complete bracketed IP literal.
    std::string_view port; ///< Decimal port without its separator, or empty.
};

/// @brief Separates a validated Host authority without misreading IPv6 colons as a port.
[[nodiscard]] AuthorityParts split_authority(const std::string_view authority) noexcept
{
    if (authority.starts_with('['))
    {
        const auto closing = authority.find(']');
        if (closing != std::string_view::npos && closing + 1 < authority.size())
        {
            return {authority.substr(0, closing + 1), authority.substr(closing + 2)};
        }
        return {authority, {}};
    }
    const auto colon = authority.find(':');
    return colon == std::string_view::npos
               ? AuthorityParts{authority, {}}
               : AuthorityParts{authority.substr(0, colon), authority.substr(colon + 1)};
}

/// @brief Compares authorities after assigning each scheme's default port.
[[nodiscard]] bool same_authority(const std::string_view left, const std::string_view right,
                                  const bool secure) noexcept
{
    if (!http::detail::is_valid_host_authority(left) ||
        !http::detail::is_valid_host_authority(right))
    {
        return false;
    }
    auto left_parts = split_authority(left);
    auto right_parts = split_authority(right);
    constexpr std::string_view http_default_port = "80";
    constexpr std::string_view https_default_port = "443";
    const auto default_port = secure ? https_default_port : http_default_port;
    const auto left_port = left_parts.port.empty() ? default_port : left_parts.port;
    const auto right_port = right_parts.port.empty() ? default_port : right_parts.port;
    return http::detail::ascii_case_insensitive_equal(left_parts.host, right_parts.host) &&
           left_port == right_port;
}

/// @brief Validates a serialized browser origin against effective scheme and Host.
[[nodiscard]] bool has_same_origin(const std::string_view origin,
                                   const http::HttpRequestView &request) noexcept
{
    constexpr std::string_view scheme_separator = "://";
    const auto separator = origin.find(scheme_separator);
    if (separator == std::string_view::npos)
    {
        return false;
    }
    const auto scheme = origin.substr(0, separator);
    const auto expected_scheme =
        request.transport().secure ? std::string_view{"https"} : std::string_view{"http"};
    if (!http::detail::ascii_case_insensitive_equal(scheme, expected_scheme))
    {
        return false;
    }
    const auto authority = origin.substr(separator + scheme_separator.size());
    if (authority.empty() || authority.find_first_of("/?#@") != std::string_view::npos)
    {
        return false;
    }
    return same_authority(authority, request.header("Host"), request.transport().secure);
}

/// @brief Creates one non-disclosing response for every browser mutation rejection.
[[nodiscard]] Result<http::HttpResponse, http::HttpRouteError> csrf_rejected()
{
    return http::api::make_json_response(http::HttpStatusCode::forbidden,
                                         "{\"error\":\"csrf_validation_failed\"}");
}

/// @brief Enforces the stateless marker plus available browser origin metadata.
[[nodiscard]] bool accepts_browser_mutation(const http::HttpRequestView &request) noexcept
{
    if (!is_state_changing(request.method()))
    {
        return true;
    }

    const auto marker = unique_header(request, csrf_header_name);
    if (marker.ambiguous || !marker.present || marker.value != csrf_header_value)
    {
        return false;
    }

    const auto origin = unique_header(request, "Origin");
    if (origin.ambiguous || (origin.present && !has_same_origin(origin.value, request)))
    {
        return false;
    }

    const auto fetch_site = unique_header(request, "Sec-Fetch-Site");
    return !fetch_site.ambiguous &&
           (!fetch_site.present ||
            http::detail::ascii_case_insensitive_equal(fetch_site.value, "same-origin"));
}

} // namespace

http::api::ApiRequestDecorator make_origin_csrf_policy()
{
    return [](http::api::ApiRequestHandler next)
    {
        return http::api::ApiRequestHandler(
            [next = std::move(next)](http::api::ApiRequestContext &context)
                -> Result<http::HttpResponse, http::HttpRouteError>
            {
                if (!accepts_browser_mutation(context.request))
                {
                    return csrf_rejected();
                }
                return next(context);
            });
    };
}

http::HttpRouteHandler enforce_origin_csrf(http::HttpRouteHandler handler)
{
    return [handler = std::move(handler)](const http::HttpRequestView &request,
                                          const http::HttpRouteParameters &parameters)
               -> Result<http::HttpResponse, http::HttpRouteError>
    {
        if (!accepts_browser_mutation(request))
        {
            return csrf_rejected();
        }
        return handler(request, parameters);
    };
}

} // namespace sparenode::authentication::api
