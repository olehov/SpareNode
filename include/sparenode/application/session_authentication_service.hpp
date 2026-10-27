#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "sparenode/authentication/authentication_service.hpp"
#include "sparenode/authentication/principal.hpp"
#include "sparenode/authentication/sensitive_password.hpp"
#include "sparenode/foundation/result.hpp"
#include "sparenode/session/session_store.hpp"

namespace sparenode::application
{

/// @brief Identifies a public login failure without exposing credential details.
enum class SessionLoginErrorCode : std::uint8_t
{
    invalid_credentials, ///< Login is unknown or the password does not match.
    unavailable          ///< Authentication or session infrastructure failed closed.
};

/// @brief Identifies a logout failure without exposing session material.
enum class SessionLogoutErrorCode : std::uint8_t
{
    unavailable ///< Session infrastructure could not complete invalidation.
};

/// @brief Orchestrates credential verification and bounded session lifecycle operations.
class SessionAuthenticationService final
{
  public:
    /// @brief Creates the application service from ready process-wide dependencies.
    /// @param[in] authentication Credential verification service retained by value.
    /// @param[in] sessions Shared bounded session store retained for process lifetime.
    /// @return Ready service, or unavailable when no session store was supplied.
    [[nodiscard]] static Result<SessionAuthenticationService, SessionLoginErrorCode>
    create(authentication::AuthenticationService authentication,
           std::shared_ptr<session::SessionStore> sessions);

    /// @brief Verifies credentials and creates a fresh session for the resulting principal.
    /// @param[in] login_identifier Untrusted bounded login identifier.
    /// @param[in,out] password Owned plaintext password consumed by authentication.
    /// @param[in] previous_session Optional credential invalidated before replacement.
    /// @return Opaque session credential, generic invalid credentials, or unavailable service.
    [[nodiscard]] Result<std::string, SessionLoginErrorCode>
    login(std::string_view login_identifier, authentication::SensitivePassword &&password,
          std::string_view previous_session = {}) const;

    /// @brief Invalidates the submitted session credential when it is still active.
    /// @param[in] credential Opaque credential presented by the client cookie.
    /// @return Invalidated principal when active, empty when absent, or unavailable service.
    [[nodiscard]] Result<std::optional<authentication::Principal>, SessionLogoutErrorCode>
    logout(std::string_view credential) const;

    /// @brief Returns the credential verifier used by the login operation.
    /// @return Immutable process-wide authentication service.
    [[nodiscard]] const authentication::AuthenticationService &authentication() const noexcept;

    /// @brief Shares the bounded store used by login, logout, and protected-route policy.
    /// @return Process-wide mutable session store.
    [[nodiscard]] const std::shared_ptr<session::SessionStore> &sessions() const noexcept;

  private:
    /// @brief Stores fully initialized application dependencies.
    /// @param[in] authentication Credential verification service retained by value.
    /// @param[in] sessions Shared bounded session store retained for process lifetime.
    SessionAuthenticationService(authentication::AuthenticationService authentication,
                                 std::shared_ptr<session::SessionStore> sessions) noexcept;

    authentication::AuthenticationService authentication_; ///< Credential verification policy.
    std::shared_ptr<session::SessionStore> sessions_;      ///< Bounded active session state.
};

} // namespace sparenode::application
