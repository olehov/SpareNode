#include "sparenode/http/api/api_router_builder.hpp"

#include <utility>

namespace sparenode::http::api
{

ApiRouterBuilder::ApiRouterBuilder(ProtectedRouteDecorator protected_route_decorator)
    : protected_route_decorator_(std::move(protected_route_decorator))
{
}

Result<void, HttpRouteRegistrationError> ApiRouterBuilder::register_public(PublicApiRoute route)
{
    return router_.register_route(route.method, std::move(route.pattern), std::move(route.handler));
}

Result<void, HttpRouteRegistrationError>
ApiRouterBuilder::register_protected(ProtectedApiRoute route)
{
    if (protected_route_decorator_)
    {
        route.handler = protected_route_decorator_(std::move(route.handler));
    }
    return router_.register_route(route.method, std::move(route.pattern), std::move(route.handler));
}

HttpRouter ApiRouterBuilder::build() && noexcept
{
    protected_route_decorator_ = nullptr;
    return std::move(router_);
}

} // namespace sparenode::http::api
