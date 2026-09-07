#pragma once

#include <optional>

#include "sparenode/http/http_request_timeouts.hpp"
#include "sparenode/network/network_io_options.hpp"

namespace sparenode::http::detail
{

/// @brief Tracks total and inactivity deadlines without owning a clock or timer thread.
class RequestDeadlines final
{
  public:
    /// @brief Validates all budgets and forms the immutable total deadline.
    /// @param[in] timeouts Positive durations representable at the supplied start time.
    /// @param[in] started Monotonic timestamp sampled at session entry.
    /// @return Deadline state, or no value when a duration or addition would overflow.
    [[nodiscard]] static std::optional<RequestDeadlines>
    create(const HttpRequestTimeouts &timeouts, network::NetworkDeadline started) noexcept;

    /// @brief Records a successful nonempty receive; empty reads must not call this method.
    /// @param[in] received_at Monotonic timestamp immediately after the receive completed.
    void record_progress(network::NetworkDeadline received_at) noexcept;

    /// @brief Returns the earlier of the phase inactivity limit and total deadline.
    /// @param[in] reading_body Whether the complete header terminator has arrived.
    /// @return Absolute receive deadline that cannot exceed the total budget.
    [[nodiscard]] network::NetworkDeadline next(bool reading_body) const noexcept;

    /// @brief Returns the non-renewable total receive deadline.
    /// @return Absolute end of the request receive budget.
    [[nodiscard]] network::NetworkDeadline total() const noexcept;

  private:
    /// @brief Initializes validated deadline state at the first receive boundary.
    /// @param[in] timeouts Validated phase and total budgets.
    /// @param[in] started Session start and initial inactivity anchor.
    /// @param[in] total_deadline Representable non-renewable deadline.
    RequestDeadlines(network::NetworkDeadline started, const HttpRequestTimeouts &timeouts,
                     network::NetworkDeadline total_deadline) noexcept;

    HttpRequestTimeouts timeouts_;           ///< Validated receive budgets.
    network::NetworkDeadline last_progress_; ///< Start or most recent nonempty receive.
    network::NetworkDeadline total_;         ///< Immutable total receive deadline.
};

} // namespace sparenode::http::detail
