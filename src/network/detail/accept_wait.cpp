#include "sparenode/network/detail/accept_wait.hpp"

#include "sparenode/network/detail/socket_wait.hpp"

namespace sparenode::network::detail
{
namespace
{

/// @brief Converts a general socket-wait result to listener-specific vocabulary.
/// @param[in] status General readiness completion status.
/// @return `cancelled` for cancellation; otherwise `socket_ready`, intentionally
/// collapsing readable, socket-error, and hangup statuses so accept reports the
/// authoritative native result.
[[nodiscard]] AcceptWaitStatus to_accept_status(const SocketWaitStatus status) noexcept
{
    return status == SocketWaitStatus::cancelled ? AcceptWaitStatus::cancelled
                                                 : AcceptWaitStatus::socket_ready;
}

} // namespace

Result<AcceptWaitStatus, NetworkError> wait_for_accept(const SocketWaitContext &context)
{
    const auto result = wait_for_socket(
        context, {.interest = SocketWaitInterest::readable, .operation = NetworkOperation::accept});
    if (!result)
    {
        return unexpected(result.error());
    }

    return to_accept_status(result.value());
}

Result<AcceptWaitStatus, NetworkError> wait_for_accept(const SocketWaitContext &context,
                                                       const std::stop_token &stop_token)
{
    const auto result = wait_for_socket(
        context, {.interest = SocketWaitInterest::readable, .operation = NetworkOperation::accept},
        stop_token);
    if (!result)
    {
        return unexpected(result.error());
    }

    return to_accept_status(result.value());
}

} // namespace sparenode::network::detail
