#include "sparenode/authentication/authentication_service.hpp"

#include <new>
#include <utility>

namespace sparenode::authentication
{
namespace
{

/// @brief Non-secret input used only to create a valid dummy Argon2id verifier.
constexpr std::string_view dummy_password = "SpareNode unknown-login verification";

} // namespace

Result<AuthenticationService, AuthenticationServiceCreateErrorCode>
AuthenticationService::create(std::shared_ptr<const CredentialProvider> provider,
                              std::shared_ptr<const PasswordVerifier> verifier)
{
    if (!provider)
    {
        return unexpected(AuthenticationServiceCreateErrorCode::missing_provider);
    }
    if (!verifier)
    {
        return unexpected(AuthenticationServiceCreateErrorCode::verifier_unavailable);
    }

    const auto input = std::span(dummy_password.data(), dummy_password.size());
    auto dummy_result = verifier->hash(input);
    if (!dummy_result)
    {
        return unexpected(AuthenticationServiceCreateErrorCode::verifier_unavailable);
    }
    return AuthenticationService(std::move(provider), std::move(verifier),
                                 std::move(dummy_result).value());
}

AuthenticationService::AuthenticationService(std::shared_ptr<const CredentialProvider> provider,
                                             std::shared_ptr<const PasswordVerifier> verifier,
                                             std::string dummy_verifier) noexcept
    : provider_(std::move(provider)), verifier_(std::move(verifier)),
      dummy_verifier_(std::move(dummy_verifier))
{
}

Result<AuthenticatedCredential, AuthenticationErrorCode>
AuthenticationService::authenticate(const std::string_view login,
                                    SensitivePassword &&submitted_password) const
{
    SensitivePassword password(std::move(submitted_password));
    try
    {
        const auto credential_result = provider_->find_by_login(login);
        if (!credential_result)
        {
            return unexpected(AuthenticationErrorCode::unavailable);
        }

        const auto &credential = credential_result.value();
        const std::string_view encoded_verifier =
            credential.has_value() ? credential->encoded_password_verifier : dummy_verifier_;
        const auto matches = verifier_->verify(password.bytes(), encoded_verifier);
        if (!matches)
        {
            return unexpected(AuthenticationErrorCode::unavailable);
        }
        if (!credential.has_value() || !matches.value())
        {
            return unexpected(AuthenticationErrorCode::invalid_credentials);
        }
        return AuthenticatedCredential{Principal{credential->identity},
                                       credential->canonical_login};
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(AuthenticationErrorCode::unavailable);
    }
    catch (...)
    {
        return unexpected(AuthenticationErrorCode::unavailable);
    }
}

} // namespace sparenode::authentication
