#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "sparenode/authentication/authentication_rate_limit_policy.hpp"
#include "sparenode/authentication/credential_provider.hpp"
#include "sparenode/authentication/password_verifier.hpp"
#include "sparenode/authentication/principal.hpp"
#include "sparenode/authentication/sensitive_password.hpp"

namespace sparenode::authentication
{

/// @brief Supplies monotonic time and permits deterministic rate-limit tests.
using AuthenticationClock = std::function<std::chrono::steady_clock::time_point()>;

/// @brief Identity established by successful credential verification.
struct AuthenticatedCredential
{
    Principal principal;         ///< Source-independent identity admitted to a session.
    std::string canonical_login; ///< Provider-owned login for internal decisions.
};

/// @brief Identifies failure to construct the authentication service.
enum class AuthenticationServiceCreateErrorCode : std::uint8_t
{
    missing_provider,          ///< No credential provider was supplied.
    verifier_unavailable,      ///< The dummy verifier could not be created safely.
    invalid_rate_limit_policy, ///< Failure threshold or automatic cooldown is not positive.
};

/// @brief Identifies a public authentication outcome without revealing account existence.
enum class AuthenticationErrorCode : std::uint8_t
{
    invalid_credentials, ///< Login is unknown or the password does not match.
    throttled,           ///< Process-wide failed-attempt cooldown is active.
    unavailable,         ///< Provider or verification infrastructure failed closed.
};

/// @brief Authenticates submitted credentials without depending on their concrete source.
class AuthenticationService final
{
  public:
    /// @brief Creates a service and one process-local dummy verification record.
    /// @param[in] provider Abstract credential source retained for service lifetime.
    /// @param[in] verifier Abstract verifier retained for service lifetime.
    /// @param[in] rate_limit_policy Fixed failure threshold and automatic cooldown.
    /// @param[in] clock Optional monotonic clock override.
    /// @return Ready authentication service, or a fail-closed construction error.
    [[nodiscard]] static Result<AuthenticationService, AuthenticationServiceCreateErrorCode>
    create(std::shared_ptr<const CredentialProvider> provider,
           std::shared_ptr<const PasswordVerifier> verifier,
           AuthenticationRateLimitPolicy rate_limit_policy = {}, AuthenticationClock clock = {});

    /// @brief Verifies one login attempt and erases its plaintext password before returning.
    /// @param[in] login Untrusted bounded login identifier.
    /// @param[in,out] password Owned temporary plaintext consumed by this call.
    /// @return Authenticated identity, generic invalid credentials, or unavailable service.
    [[nodiscard]] Result<AuthenticatedCredential, AuthenticationErrorCode>
    authenticate(std::string_view login, SensitivePassword &&password) const;

  private:
    class SharedState;

    /// @brief Retains synchronized process-wide verification and rate-limit state.
    /// @param[in,out] state Fully initialized shared state transferred into the service.
    explicit AuthenticationService(std::shared_ptr<SharedState> state) noexcept;

    std::shared_ptr<SharedState> state_; ///< Shared by every copied application-service adapter.
};

} // namespace sparenode::authentication
