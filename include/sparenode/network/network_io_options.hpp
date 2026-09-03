#pragma once

#include <chrono>
#include <optional>
#include <stop_token>

namespace sparenode::network
{

/// @brief Absolute monotonic instant after which one network wait must expire.
using NetworkDeadline = std::chrono::steady_clock::time_point;

/// @brief Groups cooperative cancellation and an optional absolute deadline for one I/O call.
struct NetworkIoOptions
{
    std::stop_token stop_token{}; ///< Token observed before and during readiness waits.
    std::optional<NetworkDeadline> deadline{}; ///< Absolute expiry, or no time limit.
};

} // namespace sparenode::network
