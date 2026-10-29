#include "sparenode/authentication/detail/authentication_rate_limiter.hpp"

namespace sparenode::authentication::detail
{

AuthenticationRateLimiter::AuthenticationRateLimiter(
    const AuthenticationRateLimitPolicy policy) noexcept
    : policy_(policy)
{
}

bool AuthenticationRateLimiter::allows(const std::chrono::steady_clock::time_point now) noexcept
{
    if (consecutive_failures_ < policy_.maximum_failures)
    {
        return true;
    }
    if (now < blocked_until_)
    {
        return false;
    }

    // An elapsed cooldown begins a fresh bounded sequence instead of permanently locking access.
    consecutive_failures_ = 0;
    blocked_until_ = {};
    return true;
}

void AuthenticationRateLimiter::record_failure(
    const std::chrono::steady_clock::time_point now) noexcept
{
    if (consecutive_failures_ >= policy_.maximum_failures)
    {
        return;
    }
    ++consecutive_failures_;
    if (consecutive_failures_ == policy_.maximum_failures)
    {
        const auto remaining = std::chrono::steady_clock::time_point::max() - now;
        blocked_until_ = policy_.cooldown > remaining ? std::chrono::steady_clock::time_point::max()
                                                      : now + policy_.cooldown;
    }
}

void AuthenticationRateLimiter::record_success() noexcept
{
    consecutive_failures_ = 0;
    blocked_until_ = {};
}

} // namespace sparenode::authentication::detail
