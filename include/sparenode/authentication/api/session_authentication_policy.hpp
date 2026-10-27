#pragma once

#include <memory>
#include <string_view>

#include "sparenode/http/api/protected_route_policy.hpp"
#include "sparenode/session/session_store.hpp"

namespace sparenode::authentication::api
{

/// @brief Provisional host-only cookie name consumed by session authentication.
inline constexpr std::string_view session_cookie_name = "spnode_session";

/// @brief Represents one unambiguous session credential extracted from Cookie fields.
struct SessionCookieCredential
{
    std::string_view value; ///< Borrowed opaque credential when exactly one is present.
    bool malformed{};       ///< Duplicate or syntactically ambiguous cookie input.
};

/// @brief Extracts one session credential while rejecting ambiguous Cookie fields.
/// @param[in] request Complete bounded request metadata.
/// @return Borrowed credential state valid for the request lifetime.
[[nodiscard]] SessionCookieCredential
session_cookie_credential(const http::HttpRequestView &request) noexcept;

/// @brief Builds the protected-route stage that authenticates one session cookie.
/// @param[in] sessions Process-wide bounded session store retained by the decorator.
/// @return Policy that rejects missing/invalid sessions before operation handlers run.
[[nodiscard]] http::api::ApiRequestDecorator
make_session_authentication_policy(std::shared_ptr<session::SessionStore> sessions);

} // namespace sparenode::authentication::api
