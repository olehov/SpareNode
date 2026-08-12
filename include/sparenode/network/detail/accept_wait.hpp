#pragma once

#include <cstdint>
#include <stop_token>

#include "sparenode/network/detail/native_socket.hpp"

namespace sparenode::network::detail
{

/// Describes why the cancellable listener wait completed.
enum class AcceptWaitStatus : std::uint8_t
{
    socket_ready,
    cancelled,
};

/// Waits until the listener is readable or the supplied stop token is cancelled.
///
/// A private loopback datagram channel wakes the native polling call, so stop
/// requests do not depend on closing the listening socket or periodic polling.
[[nodiscard]] Result<AcceptWaitStatus, NetworkError>
wait_for_accept(NativeSocket listener_socket, const std::stop_token &stop_token);

} // namespace sparenode::network::detail
