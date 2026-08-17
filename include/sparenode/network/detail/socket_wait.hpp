#pragma once

#include <cstdint>
#include <stop_token>

#include "sparenode/network/detail/native_socket.hpp"
#include "sparenode/network/detail/socket_poller.hpp"

namespace sparenode::network::detail
{

/// Identifies which readiness condition a socket operation requires.
enum class SocketWaitInterest : std::uint8_t
{
    readable,
    writable,
};

/// Describes why a socket-readiness wait completed.
enum class SocketWaitStatus : std::uint8_t
{
    socket_ready,
    cancelled,
};

/// Waits indefinitely for one socket without allocating cancellation resources.
[[nodiscard]] Result<SocketWaitStatus, NetworkError> wait_for_socket(NativeSocket socket,
                                                                     SocketWaitInterest interest,
                                                                     NetworkOperation operation,
                                                                     SocketPoller &poller);

/// Waits for one socket while allowing a stop request to wake the native poll.
///
/// A private loopback wake channel is created only when the supplied token can
/// actually be stopped. The socket itself is never closed to interrupt the wait.
[[nodiscard]] Result<SocketWaitStatus, NetworkError>
wait_for_socket(NativeSocket socket, SocketWaitInterest interest, NetworkOperation operation,
                const std::stop_token &stop_token, SocketPoller &poller);

} // namespace sparenode::network::detail
