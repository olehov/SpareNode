#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>

namespace sparenode::concurrency
{

/// @brief Describes dependency-neutral cooperative I/O interruption state.
enum class InterruptionStatus : std::uint8_t
{
    none,             ///< The operation may continue.
    cancelled,        ///< Cancellation was requested and has priority over expiry.
    deadline_exceeded ///< The absolute monotonic deadline has elapsed.
};

/// @brief Inspects cancellation first, then an optional absolute deadline.
/// @param[in] stop_token Cooperative cancellation token.
/// @param[in] deadline Optional absolute steady-clock expiry.
/// @param[in] now Caller-supplied monotonic instant for deterministic policy evaluation.
/// @return Current interruption state with cancellation priority preserved.
[[nodiscard]] inline InterruptionStatus
interruption_status(const std::stop_token &stop_token,
                    const std::optional<std::chrono::steady_clock::time_point> deadline,
                    const std::chrono::steady_clock::time_point now) noexcept
{
    if (stop_token.stop_requested())
    {
        return InterruptionStatus::cancelled;
    }
    if (deadline.has_value() && now >= deadline.value())
    {
        return InterruptionStatus::deadline_exceeded;
    }
    return InterruptionStatus::none;
}

/// @brief Inspects cancellation and expiry against the current monotonic time.
[[nodiscard]] inline InterruptionStatus
interruption_status(const std::stop_token &stop_token,
                    const std::optional<std::chrono::steady_clock::time_point> deadline) noexcept
{
    return interruption_status(stop_token, deadline, std::chrono::steady_clock::now());
}

} // namespace sparenode::concurrency
