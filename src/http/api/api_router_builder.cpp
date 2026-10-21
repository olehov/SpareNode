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
    ApiRouteDescriptor descriptor{route.method, route.pattern};
    auto registered =
        router_.register_route(route.method, std::move(route.pattern), std::move(route.handler));
    if (registered)
    {
        public_routes_.push_back(std::move(descriptor));
    }
    return registered;
}

Result<void, HttpRouteRegistrationError>
ApiRouterBuilder::register_protected(ProtectedApiRoute route)
{
    if (protected_route_decorator_)
    {
        route.handler = protected_route_decorator_(std::move(route.handler));
    }
    ApiRouteDescriptor descriptor{route.method, route.pattern};
    auto registered =
        router_.register_route(route.method, std::move(route.pattern), std::move(route.handler));
    if (registered)
    {
        protected_routes_.push_back(std::move(descriptor));
    }
    return registered;
}

const std::vector<ApiRouteDescriptor> &ApiRouterBuilder::public_routes() const noexcept
{
    return public_routes_;
}

const std::vector<ApiRouteDescriptor> &ApiRouterBuilder::protected_routes() const noexcept
{
    return protected_routes_;
}

HttpRouter ApiRouterBuilder::build() && noexcept
{
    protected_route_decorator_ = nullptr;
    public_routes_.clear();
    protected_routes_.clear();
    return std::move(router_);
}

} // namespace sparenode::http::api
