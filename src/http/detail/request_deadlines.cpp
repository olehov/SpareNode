#include "sparenode/http/detail/request_deadlines.hpp"

#include <algorithm>

namespace sparenode::http::detail
{
namespace
{

/// @brief Adds a positive millisecond budget without overflowing the native clock.
/// @param[in] started Nonnegative monotonic timestamp.
/// @param[in] timeout Positive budget to convert and add.
/// @return Representable absolute deadline, or no value for an invalid budget.
[[nodiscard]] std::optional<network::NetworkDeadline>
checked_deadline(const network::NetworkDeadline started,
                 const std::chrono::milliseconds timeout) noexcept
{
    using Duration = network::NetworkDeadline::duration;
    if (timeout.count() <= 0 || started < network::NetworkDeadline{})
    {
        return std::nullopt;
    }
    const Duration remaining = network::NetworkDeadline::max() - started;
    if (timeout > std::chrono::duration_cast<std::chrono::milliseconds>(remaining))
    {
        return std::nullopt;
    }
    const Duration converted = std::chrono::duration_cast<Duration>(timeout);
    if (converted <= Duration::zero() || converted > remaining)
    {
        return std::nullopt;
    }
    return started + converted;
}

} // namespace

/// @brief Creates deadline state only when all three budgets can be added safely.
/// @return Initialized state, or no value for an invalid or overflowing budget.
std::optional<RequestDeadlines>
RequestDeadlines::create(const HttpRequestTimeouts &timeouts,
                         const network::NetworkDeadline started) noexcept
{
    const auto total = checked_deadline(started, timeouts.total);
    if (!total.has_value() || !checked_deadline(started, timeouts.headers).has_value() ||
        !checked_deadline(started, timeouts.body).has_value())
    {
        return std::nullopt;
    }
    return RequestDeadlines(started, timeouts, total.value());
}

/// @brief Stores validated budgets and anchors initial inactivity at session entry.
RequestDeadlines::RequestDeadlines(const network::NetworkDeadline started,
                                   const HttpRequestTimeouts &timeouts,
                                   const network::NetworkDeadline total_deadline) noexcept
    : timeouts_(timeouts), last_progress_(started), total_(total_deadline)
{
}

/// @brief Advances the inactivity anchor without moving it backwards or renewing total time.
void RequestDeadlines::record_progress(const network::NetworkDeadline received_at) noexcept
{
    last_progress_ = (std::max)(last_progress_, received_at);
}

/// @brief Clamps phase inactivity to the total deadline, including on addition overflow.
/// @return Earlier of the representable inactivity deadline and the total deadline.
network::NetworkDeadline RequestDeadlines::next(const bool reading_body) const noexcept
{
    const auto inactivity =
        checked_deadline(last_progress_, reading_body ? timeouts_.body : timeouts_.headers);
    return inactivity.has_value() ? (std::min)(inactivity.value(), total_) : total_;
}

/// @brief Exposes the immutable deadline established at session creation.
/// @return Absolute end of the total request receive budget.
network::NetworkDeadline RequestDeadlines::total() const noexcept
{
    return total_;
}

} // namespace sparenode::http::detail
