#include "sparenode/http/api/protected_route_policy.hpp"

#include <array>
#include <memory>
#include <ranges>
#include <utility>

namespace sparenode::http::api
{
namespace
{

/// @brief Owns policies in the only order accepted for protected routes.
class ProtectedRoutePolicyPipeline final
{
  public:
    /// @brief Transfers named policies into their fixed evaluation slots.
    explicit ProtectedRoutePolicyPipeline(ProtectedRoutePolicies policies)
        : policies_{std::move(policies.transport),   std::move(policies.session_authentication),
                    std::move(policies.origin_csrf), std::move(policies.authorization),
                    std::move(policies.throttling),  std::move(policies.resource_coordination)}
    {
    }

    /// @brief Wraps one operation handler in the fixed policy order.
    [[nodiscard]] HttpRouteHandler decorate(HttpRouteHandler handler) const
    {
        ApiRequestHandler decorated = [handler = std::move(handler)](ApiRequestContext &context)
        { return handler(context.request, context.parameters); };
        for (const auto &policy : policies_ | std::views::reverse)
        {
            if (policy)
            {
                decorated = policy(std::move(decorated));
            }
        }
        return HttpRouteHandler(
            [decorated = std::move(decorated)](const HttpRequestView &request,
                                               const HttpRouteParameters &parameters)
            {
                ApiRequestContext context{request, parameters, {}};
                return decorated(context);
            });
    }

  private:
    std::array<ApiRequestDecorator, 6> policies_; ///< Fixed pre-handler security sequence.
};

} // namespace

ProtectedRouteDecorator make_protected_route_decorator(ProtectedRoutePolicies policies)
{
    auto pipeline = std::make_shared<const ProtectedRoutePolicyPipeline>(std::move(policies));
    return [pipeline = std::move(pipeline)](HttpRouteHandler handler)
    { return pipeline->decorate(std::move(handler)); };
}

} // namespace sparenode::http::api
