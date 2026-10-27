#include "sparenode/application/session_authentication_service.hpp"

#include <utility>

namespace sparenode::application
{

Result<SessionAuthenticationService, SessionLoginErrorCode>
SessionAuthenticationService::create(authentication::AuthenticationService authentication,
                                     std::shared_ptr<session::SessionStore> sessions)
{
    if (!sessions)
    {
        return unexpected(SessionLoginErrorCode::unavailable);
    }
    return SessionAuthenticationService(std::move(authentication), std::move(sessions));
}

SessionAuthenticationService::SessionAuthenticationService(
    authentication::AuthenticationService authentication,
    std::shared_ptr<session::SessionStore> sessions) noexcept
    : authentication_(std::move(authentication)), sessions_(std::move(sessions))
{
}

Result<std::string, SessionLoginErrorCode>
SessionAuthenticationService::login(const std::string_view login_identifier,
                                    authentication::SensitivePassword &&password,
                                    const std::string_view previous_session) const
{
    auto authenticated = authentication_.authenticate(login_identifier, std::move(password));
    if (!authenticated)
    {
        return unexpected(authenticated.error() ==
                                  authentication::AuthenticationErrorCode::invalid_credentials
                              ? SessionLoginErrorCode::invalid_credentials
                              : SessionLoginErrorCode::unavailable);
    }
    if (!previous_session.empty())
    {
        auto invalidated = sessions_->invalidate(previous_session);
        if (!invalidated)
        {
            return unexpected(SessionLoginErrorCode::unavailable);
        }
    }
    auto credential = sessions_->create_session(std::move(authenticated->principal));
    if (!credential)
    {
        return unexpected(SessionLoginErrorCode::unavailable);
    }
    return std::move(credential).value();
}

Result<std::optional<authentication::Principal>, SessionLogoutErrorCode>
SessionAuthenticationService::logout(const std::string_view credential) const
{
    auto invalidated = sessions_->invalidate(credential);
    if (!invalidated)
    {
        return unexpected(SessionLogoutErrorCode::unavailable);
    }
    return std::move(invalidated).value();
}

const authentication::AuthenticationService &
SessionAuthenticationService::authentication() const noexcept
{
    return authentication_;
}

const std::shared_ptr<session::SessionStore> &
SessionAuthenticationService::sessions() const noexcept
{
    return sessions_;
}

} // namespace sparenode::application
