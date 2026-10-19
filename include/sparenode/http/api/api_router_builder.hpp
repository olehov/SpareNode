#pragma once

#include <functional>
#include <string>

#include "sparenode/foundation/result.hpp"
#include "sparenode/http/routing/http_router.hpp"

namespace sparenode::http::api
{

/// @brief Describes an endpoint that can be dispatched without an access policy.
struct PublicApiRoute
{
    HttpMethod method{};      ///< HTTP method matched by the route.
    std::string pattern;      ///< Validated exact or wildcard origin path.
    HttpRouteHandler handler; ///< Operation invoked after route matching.
};

/// @brief Describes an endpoint that must pass the configured access policy.
struct ProtectedApiRoute
{
    HttpMethod method{};      ///< HTTP method matched by the route.
    std::string pattern;      ///< Validated exact or wildcard origin path.
    HttpRouteHandler handler; ///< Operation invoked after policy enforcement.
};

/// @brief Decorates a protected handler with authentication or another access policy.
using ProtectedRouteDecorator = std::move_only_function<HttpRouteHandler(HttpRouteHandler)>;

/// @brief Composes public and protected endpoints into one transport-neutral router.
class ApiRouterBuilder final
{
  public:
    /// @brief Creates a builder with an optional protected-route decorator.
    /// @param[in] protected_route_decorator Policy applied to every protected handler.
    explicit ApiRouterBuilder(ProtectedRouteDecorator protected_route_decorator = {});

    /// @brief Registers one route that bypasses the protected-route policy.
    /// @param[in] route Public endpoint and its handler.
    /// @return Success, or the router's structured registration failure.
    [[nodiscard]] Result<void, HttpRouteRegistrationError> register_public(PublicApiRoute route);

    /// @brief Applies the access policy and registers one protected route.
    /// @param[in] route Protected endpoint and its undecorated handler.
    /// @return Success, or the router's structured registration failure.
    [[nodiscard]] Result<void, HttpRouteRegistrationError>
    register_protected(ProtectedApiRoute route);

    /// @brief Transfers the composed router to its runtime owner.
    /// @return Router containing every successfully registered endpoint.
    [[nodiscard]] HttpRouter build() && noexcept;

  private:
    HttpRouter router_;                                 ///< Routes composed so far.
    ProtectedRouteDecorator protected_route_decorator_; ///< Optional access-policy boundary.
};

} // namespace sparenode::http::api
