#pragma once

#include <cstdint>
#include <stop_token>

#include "sparenode/network/detail/native_socket.hpp"
#include "sparenode/network/detail/socket_poller.hpp"
#include "sparenode/network/detail/socket_wait.hpp"

namespace sparenode::network::detail
{

/// @brief Describes why a listener wait completed.
enum class AcceptWaitStatus : std::uint8_t
{
    socket_ready, ///< Native accept should run after readiness, error, or hangup.
    cancelled,    ///< The stop token requested cancellation.
};

/// @brief Implements the wait used by non-cancellable `TcpListener::accept()`.
///
/// Only the listening socket is passed to the poller. No cancellation callback or
/// UDP wake channel is created, so this operation completes only when the listener
/// becomes readable or the native poll reports an error.
///
/// @param[in] context Borrowed listener socket and stable wait collaborators.
/// @return `socket_ready` after readable, error, or hangup poll status so the
/// native accept call can determine the outcome; otherwise a structured poll error.
[[nodiscard]] Result<AcceptWaitStatus, NetworkError>
wait_for_accept(const SocketWaitContext &context);

/// @brief Implements the wait used by `TcpListener::accept(std::stop_token)`.
///
/// The poller watches both the listening socket and a private loopback UDP wake
/// channel. A stop callback signals that channel, allowing a request made after
/// polling begins to wake the blocked operation without closing the listener or
/// periodically polling with a timeout.
///
/// @param[in] context Borrowed listener socket and stable wait collaborators.
/// @param[in] stop_token Token observed before and during the blocking wait.
/// @return `cancelled` after a stop request; `socket_ready` after readable, error,
/// or hangup status; otherwise a structured polling or initialization error.
[[nodiscard]] Result<AcceptWaitStatus, NetworkError>
wait_for_accept(const SocketWaitContext &context, const std::stop_token &stop_token);

} // namespace sparenode::network::detail
