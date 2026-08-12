#pragma once

#include "sparenode/network/network_error.hpp"
#include "sparenode/result.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace sparenode::network::detail
{

#ifdef _WIN32
using NativeSocket = SOCKET;
using SocketLength = int;
inline constexpr NativeSocket invalid_socket = INVALID_SOCKET;
#else
using NativeSocket = int;
using SocketLength = socklen_t;
inline constexpr NativeSocket invalid_socket = -1;
#endif

struct SocketConfiguration
{
    NativeSocket socket;
    int family;
};

/// Returns the most recent platform socket error code.
[[nodiscard]] int last_socket_error() noexcept;

/// Closes a valid native socket and ignores failures during cleanup.
void close_socket(NativeSocket socket) noexcept;

/// Initializes process-wide socket facilities when the platform requires it.
[[nodiscard]] Result<void, NetworkError> ensure_socket_runtime();

/// Applies platform-specific listener security and address-reuse settings.
[[nodiscard]] bool configure_socket_security(SocketConfiguration configuration);

} // namespace sparenode::network::detail
