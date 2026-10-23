#pragma once

#include <functional>
#include <optional>
#include <string>

#include "sparenode/authentication/principal.hpp"
#include "sparenode/foundation/result.hpp"
#include "sparenode/http/routing/http_router.hpp"

namespace sparenode::http::api
{

/// @brief Presents one matched request to transport-neutral API policies.
struct ApiRequestContext
{
    const HttpRequestView &request;        ///< Parsed request before operation-specific access.
    const HttpRouteParameters &parameters; ///< Matched route parameters, still undecoded.
    std::optional<authentication::Principal>
        authenticated_principal; ///< Identity established by a session.
};

/// @brief Handles one request after all outer protected-route stages have accepted it.
using ApiRequestHandler =
    std::move_only_function<Result<HttpResponse, HttpRouteError>(ApiRequestContext &) const>;

/// @brief Wraps one request handler with a reusable policy stage.
///
/// A decorator may reject the request directly or invoke the supplied next handler. Resources
/// captured in the wrapper's call frame therefore remain alive through every inner stage and the
/// operation handler, which supports request-scoped throttling and resource-coordination leases.
using ApiRequestDecorator = std::move_only_function<ApiRequestHandler(ApiRequestHandler) const>;

/// @brief Decorates every handler registered as a protected API route.
using ProtectedRouteDecorator = std::move_only_function<HttpRouteHandler(HttpRouteHandler)>;

/// @brief Supplies optional policies in their fixed security evaluation order.
///
/// Execution is transport, session authentication, Origin/CSRF, authorization,
/// throttling, resource coordination, and finally the operation handler. Keeping
/// this order here ensures filesystem path decoding cannot precede access policy.
struct ProtectedRoutePolicies
{
    ApiRequestDecorator transport;              ///< Validates the effective secure transport.
    ApiRequestDecorator session_authentication; ///< Establishes the authenticated principal.
    ApiRequestDecorator origin_csrf;            ///< Enforces browser Origin and CSRF policy.
    ApiRequestDecorator authorization;          ///< Authorizes the established principal.
    ApiRequestDecorator throttling;             ///< Applies bounded request admission policy.
    ApiRequestDecorator resource_coordination;  ///< Coordinates access to mutable resources.
};

/// @brief Builds one reusable decorator that applies the fixed protected-policy pipeline.
/// @param[in,out] policies Policies transferred into process-lifetime shared state.
/// @return Decorator suitable for `ApiRouterBuilder` protected registrations.
[[nodiscard]] ProtectedRouteDecorator
make_protected_route_decorator(ProtectedRoutePolicies policies);

} // namespace sparenode::http::api
