#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "sparenode/authentication/credential_provider.hpp"
#include "sparenode/authentication/password_verifier.hpp"
#include "sparenode/authentication/sensitive_password.hpp"

namespace sparenode::authentication
{

/// @brief Identity established by successful credential verification.
struct AuthenticatedCredential
{
    std::string identity;        ///< Opaque identity used by the future session layer.
    std::string canonical_login; ///< Provider-owned login for internal decisions.
};

/// @brief Identifies failure to construct the authentication service.
enum class AuthenticationServiceCreateErrorCode : std::uint8_t
{
    missing_provider,     ///< No credential provider was supplied.
    verifier_unavailable, ///< The dummy verifier could not be created safely.
};

/// @brief Identifies a public authentication outcome without revealing account existence.
enum class AuthenticationErrorCode : std::uint8_t
{
    invalid_credentials, ///< Login is unknown or the password does not match.
    unavailable,         ///< Provider or verification infrastructure failed closed.
};

/// @brief Authenticates submitted credentials without depending on their concrete source.
class AuthenticationService final
{
  public:
    /// @brief Creates a service and one process-local dummy verification record.
    /// @param[in] provider Abstract credential source retained for service lifetime.
    /// @param[in] verifier Initialized vetted password verifier.
    /// @return Ready authentication service, or a fail-closed construction error.
    [[nodiscard]] static Result<AuthenticationService, AuthenticationServiceCreateErrorCode>
    create(std::shared_ptr<const CredentialProvider> provider, PasswordVerifier verifier);

    /// @brief Verifies one login attempt and erases its plaintext password before returning.
    /// @param[in] login Untrusted bounded login identifier.
    /// @param[in,out] password Owned temporary plaintext consumed by this call.
    /// @return Authenticated identity, generic invalid credentials, or unavailable service.
    [[nodiscard]] Result<AuthenticatedCredential, AuthenticationErrorCode>
    authenticate(std::string_view login, SensitivePassword &&password) const;

  private:
    /// @brief Stores fully initialized service dependencies and dummy verifier.
    /// @param[in,out] provider Abstract credential source transferred into the service.
    /// @param[in] verifier Initialized password verifier copied into the service.
    /// @param[in,out] dummy_verifier Encoded dummy record transferred into the service.
    AuthenticationService(std::shared_ptr<const CredentialProvider> provider,
                          PasswordVerifier verifier, std::string dummy_verifier) noexcept;

    std::shared_ptr<const CredentialProvider> provider_; ///< Source-independent credential lookup.
    PasswordVerifier verifier_;                          ///< Vetted Argon2id operation boundary.
    std::string dummy_verifier_; ///< Valid verifier used for unknown login timing work.
};

} // namespace sparenode::authentication
