#include "sparenode/authentication/authentication_service.hpp"

#include <mutex>
#include <new>
#include <utility>

#include "sparenode/authentication/detail/authentication_rate_limiter.hpp"

namespace sparenode::authentication
{
namespace
{

/// @brief Non-secret input used only to create a valid dummy Argon2id verifier.
constexpr std::string_view dummy_password = "SpareNode unknown-login verification";

} // namespace

/// @brief Owns process-wide dependencies and serializes expensive password verification.
class AuthenticationService::SharedState final
{
  public:
    /// @brief Stores validated dependencies and fixed-size limiter state.
    /// @param[in,out] provider Credential source transferred into shared state.
    /// @param[in,out] verifier Password verifier transferred into shared state.
    /// @param[in,out] dummy_verifier Unknown-login verifier transferred into shared state.
    /// @param[in] rate_limit_policy Fixed failure threshold and automatic cooldown.
    /// @param[in,out] clock Monotonic time source transferred into shared state.
    SharedState(std::shared_ptr<const CredentialProvider> provider,
                std::shared_ptr<const PasswordVerifier> verifier, std::string dummy_verifier,
                const AuthenticationRateLimitPolicy rate_limit_policy,
                AuthenticationClock clock) noexcept
        : provider(std::move(provider)), verifier(std::move(verifier)),
          dummy_verifier(std::move(dummy_verifier)), limiter(rate_limit_policy),
          clock(std::move(clock))
    {
    }

    std::shared_ptr<const CredentialProvider> provider; ///< Source-independent lookup port.
    std::shared_ptr<const PasswordVerifier> verifier;   ///< Password cryptography port.
    std::string dummy_verifier;                         ///< Unknown-login timing verifier.
    detail::AuthenticationRateLimiter limiter;          ///< Fixed-size global failure state.
    AuthenticationClock clock;                          ///< Injectable monotonic time source.
    std::mutex mutex; ///< Prevents concurrent attempts from bypassing the failure threshold.
};

Result<AuthenticationService, AuthenticationServiceCreateErrorCode>
AuthenticationService::create(std::shared_ptr<const CredentialProvider> provider,
                              std::shared_ptr<const PasswordVerifier> verifier,
                              const AuthenticationRateLimitPolicy rate_limit_policy,
                              AuthenticationClock clock)
{
    if (!provider)
    {
        return unexpected(AuthenticationServiceCreateErrorCode::missing_provider);
    }
    if (!verifier)
    {
        return unexpected(AuthenticationServiceCreateErrorCode::verifier_unavailable);
    }
    if (rate_limit_policy.maximum_failures == 0 ||
        rate_limit_policy.cooldown <= std::chrono::steady_clock::duration::zero())
    {
        return unexpected(AuthenticationServiceCreateErrorCode::invalid_rate_limit_policy);
    }
    if (!clock)
    {
        clock = [] { return std::chrono::steady_clock::now(); };
    }

    try
    {
        const auto input = std::span(dummy_password.data(), dummy_password.size());
        auto dummy_result = verifier->hash(input);
        if (!dummy_result)
        {
            return unexpected(AuthenticationServiceCreateErrorCode::verifier_unavailable);
        }
        auto state = std::make_shared<SharedState>(std::move(provider), std::move(verifier),
                                                   std::move(dummy_result).value(),
                                                   rate_limit_policy, std::move(clock));
        return AuthenticationService(std::move(state));
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(AuthenticationServiceCreateErrorCode::verifier_unavailable);
    }
}

AuthenticationService::AuthenticationService(std::shared_ptr<SharedState> state) noexcept
    : state_(std::move(state))
{
}

Result<AuthenticatedCredential, AuthenticationErrorCode>
AuthenticationService::authenticate(const std::string_view login,
                                    SensitivePassword &&submitted_password) const
{
    SensitivePassword password(std::move(submitted_password));
    try
    {
        std::lock_guard lock(state_->mutex);
        const auto now = state_->clock();
        if (!state_->limiter.allows(now))
        {
            return unexpected(AuthenticationErrorCode::throttled);
        }

        const auto credential_result = state_->provider->find_by_login(login);
        if (!credential_result)
        {
            return unexpected(AuthenticationErrorCode::unavailable);
        }

        const auto &credential = credential_result.value();
        const std::string_view encoded_verifier =
            credential.has_value() ? credential->encoded_password_verifier : state_->dummy_verifier;
        const auto matches = state_->verifier->verify(password.bytes(), encoded_verifier);
        if (!matches)
        {
            return unexpected(AuthenticationErrorCode::unavailable);
        }
        if (!credential.has_value() || !matches.value())
        {
            // Anchor cooldown after the intentionally expensive password verification completes.
            state_->limiter.record_failure(state_->clock());
            return unexpected(AuthenticationErrorCode::invalid_credentials);
        }
        state_->limiter.record_success();
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
