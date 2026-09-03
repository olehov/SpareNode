#include "sparenode/http/http_router.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace sparenode::http
{
namespace
{

constexpr std::size_t percent_encoded_sequence_size = 3;

/// @brief Checks one ASCII byte for hexadecimal digit syntax.
/// @param[in] byte Byte to classify.
/// @return `true` for an ASCII decimal digit or `A` through `F` in either case.
[[nodiscard]] constexpr bool is_hexadecimal_digit(const unsigned char byte) noexcept
{
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'F') ||
           (byte >= 'a' && byte <= 'f');
}

/// @brief Checks one unescaped byte against the supported RFC 3986 path character set.
/// @param[in] byte Byte to classify.
/// @return `true` when the byte may appear within one registered path segment.
[[nodiscard]] constexpr bool is_route_path_character(const unsigned char byte) noexcept
{
    const bool is_alpha_numeric = (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
                                  (byte >= 'a' && byte <= 'z');
    constexpr std::string_view punctuation = "-._~!$&'()+,;=:@";
    return is_alpha_numeric || punctuation.contains(static_cast<char>(byte));
}

/// @brief Validates one exact or terminal-wildcard route pattern.
/// @param[in] pattern Candidate absolute origin path.
/// @return `true` when the complete pattern belongs to the supported route grammar.
[[nodiscard]] bool is_valid_pattern(const std::string_view pattern) noexcept
{
    if (pattern.empty() || pattern.front() != '/' || pattern.contains('?') || pattern.contains('#'))
    {
        return false;
    }

    const std::size_t wildcard = pattern.find('*');
    if (wildcard != std::string_view::npos &&
        (wildcard != pattern.size() - 1 || wildcard == 0 || pattern[wildcard - 1] != '/'))
    {
        return false;
    }

    const std::string_view path = pattern.substr(0, wildcard);
    for (std::size_t index = 0; index < path.size(); ++index)
    {
        const auto byte = static_cast<unsigned char>(path[index]);
        if (byte == '/')
        {
            continue;
        }
        if (byte == '%')
        {
            if (path.size() - index < percent_encoded_sequence_size ||
                !is_hexadecimal_digit(static_cast<unsigned char>(path[index + 1])) ||
                !is_hexadecimal_digit(static_cast<unsigned char>(path[index + 2])))
            {
                return false;
            }
            index += percent_encoded_sequence_size - 1;
            continue;
        }
        if (!is_route_path_character(byte))
        {
            return false;
        }
    }
    return true;
}

/// @brief Removes the optional query component from a validated request target.
/// @param[in] target Complete origin-form request target.
/// @return Borrowed path component including its leading slash.
[[nodiscard]] std::string_view request_path(const std::string_view target) noexcept
{
    const std::size_t query = target.find('?');
    return target.substr(0, query);
}

/// @brief Tests one request path against a normalized route prefix.
/// @param[in] wildcard Whether the prefix captures every remaining byte.
/// @param[in] prefix Exact route path or wildcard prefix.
/// @param[in] path Request path to test.
/// @return `true` when the route accepts the complete path.
[[nodiscard]] bool path_matches(const bool wildcard, const std::string_view prefix,
                                const std::string_view path) noexcept
{
    return wildcard ? path.starts_with(prefix) : path == prefix;
}

/// @brief Extracts the request bytes captured by a matching wildcard route.
/// @param[in] wildcard Whether the selected route has a terminal wildcard.
/// @param[in] prefix Normalized route prefix.
/// @param[in] path Matched request path.
/// @return Borrowed suffix, or an empty view for an exact route.
[[nodiscard]] std::string_view captured_suffix(const bool wildcard, const std::string_view prefix,
                                               const std::string_view path) noexcept
{
    return wildcard ? path.substr(prefix.size()) : std::string_view{};
}

/// @brief Orders two matching routes by exactness and prefix length.
/// @param[in] candidate_wildcard Whether the candidate uses a wildcard.
/// @param[in] candidate_prefix_size Candidate prefix length.
/// @param[in] current_wildcard Whether the current selection uses a wildcard.
/// @param[in] current_prefix_size Current selection prefix length.
/// @return `true` when the candidate must replace the current selection.
[[nodiscard]] bool is_more_specific(const bool candidate_wildcard,
                                    const std::size_t candidate_prefix_size,
                                    const bool current_wildcard,
                                    const std::size_t current_prefix_size) noexcept
{
    if (candidate_wildcard != current_wildcard)
    {
        return !candidate_wildcard;
    }
    return candidate_prefix_size > current_prefix_size;
}

/// @brief Constructs one router-owned empty response through the validated response API.
/// @param[in] status_code Standard routing status.
/// @param[in] reason_phrase Standard routing reason phrase.
/// @param[in] headers Optional response fields transferred into the response.
/// @return Valid response or an internal response-validation failure.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
make_routing_response(const HttpStatusCode status_code, std::string reason_phrase,
                      std::vector<HttpResponseHeader> headers = {})
{
    auto response =
        HttpResponse::create(status_code, std::move(reason_phrase), std::move(headers), {});
    if (!response)
    {
        return unexpected(HttpRouteError{HttpRouteErrorCode::response_validation_failure, 0});
    }
    return std::move(response).value();
}

/// @brief Converts a supported method into its dense route-table index.
/// @param[in] method Validated method enumeration.
/// @return Zero-based method index.
[[nodiscard]] constexpr std::size_t method_index(const HttpMethod method) noexcept
{
    return static_cast<std::size_t>(method);
}

} // namespace

/// @brief Stores one wildcard suffix borrowed from the current request path.
HttpRouteParameters::HttpRouteParameters(const std::string_view wildcard_suffix) noexcept
    : wildcard_suffix_(wildcard_suffix)
{
}

/// @brief Returns the wildcard suffix captured for the selected route.
std::string_view HttpRouteParameters::wildcard_suffix() const noexcept
{
    return wildcard_suffix_;
}

/// @brief Validates and stores one bounded method-and-path route.
Result<void, HttpRouteRegistrationError>
HttpRouter::register_route(const HttpMethod method, std::string pattern, HttpRouteHandler handler)
{
    if (method_index(method) > method_index(HttpMethod::options))
    {
        return unexpected(
            HttpRouteRegistrationError{HttpRouteRegistrationErrorCode::invalid_method});
    }
    if (!is_valid_pattern(pattern))
    {
        return unexpected(
            HttpRouteRegistrationError{HttpRouteRegistrationErrorCode::invalid_pattern});
    }
    if (!handler)
    {
        return unexpected(
            HttpRouteRegistrationError{HttpRouteRegistrationErrorCode::empty_handler});
    }
    const bool wildcard = pattern.ends_with("/*");
    std::string prefix = wildcard ? pattern.substr(0, pattern.size() - 1) : std::move(pattern);
    if (std::ranges::any_of(routes_,
                            [&prefix, method, wildcard](const Route &route) {
                                return route.method == method && route.wildcard == wildcard &&
                                       route.prefix == prefix;
                            }))
    {
        return unexpected(
            HttpRouteRegistrationError{HttpRouteRegistrationErrorCode::duplicate_route});
    }
    if (routes_.size() >= maximum_route_count)
    {
        return unexpected(
            HttpRouteRegistrationError{HttpRouteRegistrationErrorCode::too_many_routes});
    }

    routes_.push_back(Route{method, std::move(prefix), wildcard, std::move(handler)});
    return {};
}

/// @brief Selects the most specific method-and-path handler for one request.
Result<HttpResponse, HttpRouteError> HttpRouter::dispatch(const HttpRequestView &request) const
{
    const std::string_view path = request_path(request.target());
    const Route *selected = nullptr;
    for (const Route &route : routes_)
    {
        if (route.method == request.method() && path_matches(route.wildcard, route.prefix, path) &&
            (selected == nullptr || is_more_specific(route.wildcard, route.prefix.size(),
                                                     selected->wildcard, selected->prefix.size())))
        {
            selected = &route;
        }
    }

    if (selected != nullptr)
    {
        return selected->handler(request, HttpRouteParameters(captured_suffix(
                                              selected->wildcard, selected->prefix, path)));
    }

    constexpr std::size_t method_count = method_index(HttpMethod::options) + 1;
    std::array<bool, method_count> allowed{};
    bool path_exists = false;
    for (const Route &route : routes_)
    {
        if (path_matches(route.wildcard, route.prefix, path))
        {
            path_exists = true;
            allowed[method_index(route.method)] = true;
        }
    }

    if (!path_exists)
    {
        return make_routing_response(HttpStatusCode::not_found, "Not Found");
    }

    std::string allow;
    for (std::size_t index = 0; index < allowed.size(); ++index)
    {
        if (!allowed[index])
        {
            continue;
        }
        if (!allow.empty())
        {
            allow += ", ";
        }
        allow += to_string(static_cast<HttpMethod>(index));
    }
    std::vector<HttpResponseHeader> headers;
    headers.push_back(HttpResponseHeader{"Allow", std::move(allow)});
    return make_routing_response(HttpStatusCode::method_not_allowed, "Method Not Allowed",
                                 std::move(headers));
}

/// @brief Returns the number of registered routes.
std::size_t HttpRouter::size() const noexcept
{
    return routes_.size();
}

/// @brief Converts one registration failure category into stable diagnostic text.
const char *to_string(const HttpRouteRegistrationErrorCode code) noexcept
{
    switch (code)
    {
    case HttpRouteRegistrationErrorCode::invalid_method:
        return "HTTP method is invalid";
    case HttpRouteRegistrationErrorCode::invalid_pattern:
        return "route pattern is invalid";
    case HttpRouteRegistrationErrorCode::duplicate_route:
        return "route is already registered";
    case HttpRouteRegistrationErrorCode::empty_handler:
        return "route handler is empty";
    case HttpRouteRegistrationErrorCode::too_many_routes:
        return "route table is full";
    }
    return "unknown route registration error";
}

} // namespace sparenode::http
