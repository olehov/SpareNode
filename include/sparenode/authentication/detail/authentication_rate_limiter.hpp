#pragma once

#include <chrono>
#include <cstddef>

#include "sparenode/authentication/authentication_rate_limit_policy.hpp"

namespace sparenode::authentication::detail
{

/// @brief Constant-memory state machine for a process-wide authentication cooldown.
/// @note Callers must validate policy and synchronize shared access.
class AuthenticationRateLimiter final
{
  public:
    /// @brief Creates a limiter from a policy already validated by its owning service.
    /// @param[in] policy Fixed failure threshold and automatic cooldown.
    explicit AuthenticationRateLimiter(AuthenticationRateLimitPolicy policy) noexcept;

    /// @brief Allows an attempt unless an active cooldown has not yet elapsed.
    /// @param[in] now Current monotonic time.
    /// @return `true` when credential verification may proceed.
    [[nodiscard]] bool allows(std::chrono::steady_clock::time_point now) noexcept;

    /// @brief Counts one invalid credential result and starts cooldown at the threshold.
    /// @param[in] now Current monotonic time used as the cooldown anchor.
    void record_failure(std::chrono::steady_clock::time_point now) noexcept;

    /// @brief Clears all failure state after successful authentication.
    void record_success() noexcept;

  private:
    AuthenticationRateLimitPolicy policy_; ///< Fixed centrally supplied threshold and cooldown.
    std::size_t consecutive_failures_{};   ///< Bounded count below or at the threshold.
    std::chrono::steady_clock::time_point blocked_until_{}; ///< Empty epoch or cooldown boundary.
};

} // namespace sparenode::authentication::detail
