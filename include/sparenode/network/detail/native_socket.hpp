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
#include <netinet/in.h>
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

/// Places a listener in nonblocking mode so readiness cannot race into a blocked accept.
[[nodiscard]] bool configure_socket_nonblocking(NativeSocket socket) noexcept;

/// Restores blocking mode on an accepted socket for the synchronous connection API.
[[nodiscard]] bool configure_socket_blocking(NativeSocket socket) noexcept;

/// Reports whether an accept attempt found no connection currently available.
[[nodiscard]] bool socket_error_would_block(int error_code) noexcept;

} // namespace sparenode::network::detail
