#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "sparenode/application/session_authentication_service.hpp"
#include "sparenode/foundation/result.hpp"
#include "sparenode/http/api/api_router_builder.hpp"
#include "sparenode/http/request/request_body_storage_policy.hpp"

namespace sparenode::authentication::api
{

/// @brief Exact public endpoint used to establish an owner session.
inline constexpr std::string_view login_path = "/api/auth/login";

/// @brief Maximum complete JSON credential body retained in request memory.
inline constexpr std::size_t maximum_login_body_bytes = 4096;

/// @brief Identifies why authentication API routes could not be composed.
enum class AuthenticationApiErrorCode : std::uint8_t
{
    missing_service,           ///< No login/session application service was supplied.
    route_registration_failed, ///< An authentication route conflicts with the API table.
    resource_allocation_failed ///< Bounded route state could not be allocated.
};

/// @brief Preserves authentication API composition failure details.
struct AuthenticationApiError
{
    AuthenticationApiErrorCode code{};            ///< Stable construction category.
    http::HttpRouteRegistrationError route_error; ///< Optional router failure detail.
};

/// @brief Registers the public login adapter through the generic API router boundary.
/// @param[in,out] builder Shared router composition boundary.
/// @param[in] service Process-wide authentication/session application service.
/// @return Success, or a structured startup failure.
[[nodiscard]] Result<void, AuthenticationApiError> register_authentication_api_routes(
    http::api::ApiRouterBuilder &builder,
    std::shared_ptr<const application::SessionAuthenticationService> service);

/// @brief Keeps the exact login credential body in bounded memory instead of temporary storage.
/// @param[in] head Validated request metadata available before body ingestion.
/// @return Route-specific storage choice and decoded-byte limit.
[[nodiscard]] http::RequestBodyStoragePolicy
authentication_body_storage_policy(const http::HttpRequestHead &head) noexcept;

/// @brief Returns stable text for one authentication API composition failure.
/// @param[in] code Failure category to describe.
/// @return Static diagnostic text.
[[nodiscard]] const char *to_string(AuthenticationApiErrorCode code) noexcept;

} // namespace sparenode::authentication::api
