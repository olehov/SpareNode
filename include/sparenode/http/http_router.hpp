#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "sparenode/http/http_request.hpp"
#include "sparenode/http/http_response.hpp"
#include "sparenode/result.hpp"

namespace sparenode::http
{

/// @brief Identifies a failure returned by an application route handler.
enum class HttpRouteErrorCode : std::uint8_t
{
    handler_failure,            ///< The selected application handler could not complete.
    response_validation_failure ///< A router-generated response violated its invariant.
};

/// @brief Preserves a stable route failure category and an optional subsystem code.
struct HttpRouteError
{
    HttpRouteErrorCode code{}; ///< Stable failure category.
    int detail{};              ///< Handler-defined detail code, otherwise zero.
};

/// @brief Exposes path values captured while matching one route pattern.
class HttpRouteParameters final
{
  public:
    /// @brief Returns the suffix captured by a terminal `*` pattern component.
    /// @return Borrowed raw path suffix, or an empty view for exact routes.
    [[nodiscard]] std::string_view wildcard_suffix() const noexcept;

  private:
    friend class HttpRouter;

    /// @brief Stores the suffix borrowed from the request target.
    /// @param[in] wildcard_suffix Path bytes matched by the terminal wildcard.
    explicit HttpRouteParameters(std::string_view wildcard_suffix) noexcept;

    std::string_view wildcard_suffix_; ///< Request path suffix captured by `*`.
};

/// @brief Produces a response for one matched request and its borrowed route parameters.
///
/// Handlers may be called concurrently after route registration is complete. Captured mutable
/// state must therefore provide its own synchronization.
using HttpRouteHandler = std::move_only_function<Result<HttpResponse, HttpRouteError>(
    const HttpRequestView &request, const HttpRouteParameters &parameters) const>;

/// @brief Identifies why a route could not be added to the bounded route table.
enum class HttpRouteRegistrationErrorCode : std::uint8_t
{
    invalid_method,  ///< Method value is outside the supported transport enumeration.
    invalid_pattern, ///< Pattern is not an absolute path or misuses `*`, `?`, or `#`.
    duplicate_route, ///< The same method and pattern are already registered.
    empty_handler,   ///< No callable handler was supplied.
    too_many_routes  ///< The bounded route table is full.
};

/// @brief Describes one rejected route registration.
struct HttpRouteRegistrationError
{
    HttpRouteRegistrationErrorCode code{}; ///< Stable registration failure category.
};

/// @brief Dispatches validated HTTP requests through a bounded method-and-path route table.
///
/// Patterns are either exact origin paths such as `/api/status` or paths ending in one terminal
/// wildcard such as `/api/file/*`. Matching ignores the request query string. Exact routes take
/// precedence over wildcard routes, and the longest matching wildcard prefix wins.
class HttpRouter final
{
  public:
    /// @brief Maximum number of routes retained by one router.
    static constexpr std::size_t maximum_route_count = 128;

    /// @brief Creates an empty route table.
    HttpRouter() = default;
    HttpRouter(const HttpRouter &) = delete;
    HttpRouter &operator=(const HttpRouter &) = delete;
    /// @brief Transfers all registered routes and handlers.
    /// @param[in,out] other Router whose route table is transferred.
    HttpRouter(HttpRouter &&other) noexcept = default;
    /// @brief Replaces this route table with transferred routes and handlers.
    /// @param[in,out] other Router whose route table is transferred.
    /// @return This router after replacement.
    HttpRouter &operator=(HttpRouter &&other) noexcept = default;
    /// @brief Releases registered patterns and handlers.
    ~HttpRouter() = default;

    /// @brief Registers one exact or terminal-wildcard route.
    /// @param[in] method HTTP method required by the route.
    /// @param[in] pattern Absolute origin path or a path ending in `/*`.
    /// @param[in] handler Callable that receives matching requests.
    /// @return Success or a structured registration failure.
    [[nodiscard]] Result<void, HttpRouteRegistrationError>
    register_route(HttpMethod method, std::string pattern, HttpRouteHandler handler);

    /// @brief Dispatches a request or creates a standard 404/405 response.
    /// @param[in] request Complete validated request whose source buffer remains alive.
    /// @return Handler response, standard routing response, or a handler failure.
    [[nodiscard]] Result<HttpResponse, HttpRouteError>
    dispatch(const HttpRequestView &request) const;

    /// @brief Returns the current bounded route count.
    /// @return Number of successfully registered routes.
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    /// @brief Owns one validated route and its application handler.
    struct Route
    {
        HttpMethod method{};      ///< Method required for dispatch.
        std::string prefix;       ///< Exact path or wildcard prefix ending in `/`.
        bool wildcard{};          ///< Whether the pattern captures a suffix.
        HttpRouteHandler handler; ///< Application callback invoked for a match.
    };

    std::vector<Route> routes_; ///< Bounded route table searched by specificity.
};

/// @brief Returns stable text for one registration failure.
/// @param[in] code Registration category to describe.
/// @return Static diagnostic text.
[[nodiscard]] const char *to_string(HttpRouteRegistrationErrorCode code) noexcept;

} // namespace sparenode::http
