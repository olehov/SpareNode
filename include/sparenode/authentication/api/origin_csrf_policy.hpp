#pragma once

#include <string_view>

#include "sparenode/http/api/protected_route_policy.hpp"

namespace sparenode::authentication::api
{

/// @brief Non-simple request header required for browser-session mutations.
inline constexpr std::string_view csrf_header_name = "X-SpareNode-CSRF";

/// @brief Exact marker value accepted by the stateless CSRF policy.
inline constexpr std::string_view csrf_header_value = "1";

/// @brief Builds the protected-route stage that enforces browser Origin and CSRF evidence.
/// @return Stateless policy that rejects unsafe cross-origin or unmarked requests.
[[nodiscard]] http::api::ApiRequestDecorator make_origin_csrf_policy();

/// @brief Applies the shared Origin/CSRF policy to a session-aware public route.
///
/// Logout remains public so repeated and anonymous calls do not disclose session state, but it
/// must use the same browser mutation policy as protected filesystem operations.
/// @param[in] handler Route invoked only after browser mutation validation succeeds.
/// @return Route handler guarded by the shared Origin/CSRF validator.
[[nodiscard]] http::HttpRouteHandler enforce_origin_csrf(http::HttpRouteHandler handler);

} // namespace sparenode::authentication::api
