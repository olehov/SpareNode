#pragma once

#include <cstdint>
#include <stop_token>

#include "sparenode/network/detail/native_socket.hpp"
#include "sparenode/network/detail/socket_poller.hpp"
#include "sparenode/network/detail/socket_wait.hpp"

namespace sparenode::network::detail
{

/// Describes why the cancellable listener wait completed.
enum class AcceptWaitStatus : std::uint8_t
{
    socket_ready,
    cancelled,
};

/// Implements the wait used by the non-cancellable `TcpListener::accept()` overload.
///
/// Only the listening socket is passed to the poller. No cancellation callback or
/// UDP wake channel is created, so this operation completes only when the listener
/// becomes readable or the native poll reports an error.
[[nodiscard]] Result<AcceptWaitStatus, NetworkError> wait_for_accept(SocketWaitContext context);

/// Implements the wait used by `TcpListener::accept(std::stop_token)`.
///
/// The poller watches both the listening socket and a private loopback UDP wake
/// channel. A stop callback signals that channel, allowing a request made after
/// polling begins to wake the blocked operation without closing the listener or
/// periodically polling with a timeout.
[[nodiscard]] Result<AcceptWaitStatus, NetworkError>
wait_for_accept(SocketWaitContext context, const std::stop_token &stop_token);

} // namespace sparenode::network::detail
