#include "sparenode/network/detail/accept_wait.hpp"

#include "sparenode/network/detail/socket_wait.hpp"

namespace sparenode::network::detail
{
namespace
{

/// Converts the general socket-wait result to the listener-specific vocabulary.
[[nodiscard]] AcceptWaitStatus to_accept_status(const SocketWaitStatus status) noexcept
{
    return status == SocketWaitStatus::socket_ready ? AcceptWaitStatus::socket_ready
                                                    : AcceptWaitStatus::cancelled;
}

} // namespace

Result<AcceptWaitStatus, NetworkError> wait_for_accept(const NativeSocket listener_socket,
                                                       SocketPoller &poller)
{
    const auto result = wait_for_socket(listener_socket, SocketWaitInterest::readable,
                                        NetworkOperation::accept, poller);
    if (!result)
    {
        return unexpected(result.error());
    }

    return to_accept_status(result.value());
}

Result<AcceptWaitStatus, NetworkError> wait_for_accept(const NativeSocket listener_socket,
                                                       const std::stop_token &stop_token,
                                                       SocketPoller &poller)
{
    const auto result = wait_for_socket(listener_socket, SocketWaitInterest::readable,
                                        NetworkOperation::accept, stop_token, poller);
    if (!result)
    {
        return unexpected(result.error());
    }

    return to_accept_status(result.value());
}

} // namespace sparenode::network::detail
