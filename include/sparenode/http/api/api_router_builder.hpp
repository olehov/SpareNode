#pragma once

#include <functional>
#include <string>
#include <vector>

#include "sparenode/foundation/result.hpp"
#include "sparenode/http/api/protected_route_policy.hpp"
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

/// @brief Describes one registered route without exposing its handler.
struct ApiRouteDescriptor
{
    HttpMethod method{}; ///< HTTP method matched by the route.
    std::string pattern; ///< Exact or wildcard origin path.
};

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

    /// @brief Enumerates routes that explicitly bypass the protected-policy pipeline.
    /// @return Public route descriptors in registration order.
    [[nodiscard]] const std::vector<ApiRouteDescriptor> &public_routes() const noexcept;

    /// @brief Enumerates routes that explicitly use the protected-policy pipeline.
    /// @return Protected route descriptors in registration order.
    [[nodiscard]] const std::vector<ApiRouteDescriptor> &protected_routes() const noexcept;

    /// @brief Transfers the composed router to its runtime owner.
    /// @return Router containing every successfully registered endpoint.
    [[nodiscard]] HttpRouter build() && noexcept;

  private:
    HttpRouter router_;                                 ///< Routes composed so far.
    ProtectedRouteDecorator protected_route_decorator_; ///< Optional access-policy boundary.
    std::vector<ApiRouteDescriptor> public_routes_;     ///< Explicit public endpoint catalog.
    std::vector<ApiRouteDescriptor> protected_routes_;  ///< Explicit protected endpoint catalog.
};

} // namespace sparenode::http::api
