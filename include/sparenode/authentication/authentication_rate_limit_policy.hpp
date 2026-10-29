#pragma once

#include <chrono>
#include <cstddef>

namespace sparenode::authentication
{

/// @brief Central process-wide policy for consecutive failed login attempts.
struct AuthenticationRateLimitPolicy
{
    std::size_t maximum_failures{5}; ///< Failed verifications permitted before cooldown.
    std::chrono::steady_clock::duration cooldown{
        std::chrono::seconds{30}}; ///< Automatic recovery interval after reaching the limit.
};

} // namespace sparenode::authentication
