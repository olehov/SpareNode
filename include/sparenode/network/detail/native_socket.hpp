#pragma once

#include <cstddef>
#include <span>

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
/// @brief Platform-native socket handle type.
using NativeSocket = SOCKET;
/// @brief Platform-native socket-address length type.
using SocketLength = int;
/// @brief Sentinel representing an invalid native socket.
inline constexpr NativeSocket invalid_socket = INVALID_SOCKET;
#else
/// @brief Platform-native socket handle type.
using NativeSocket = int;
/// @brief Platform-native socket-address length type.
using SocketLength = socklen_t;
/// @brief Sentinel representing an invalid native socket.
inline constexpr NativeSocket invalid_socket = -1;
#endif

/// @brief Groups a borrowed socket with the address family needed for configuration.
struct SocketConfiguration
{
    /// @brief Socket to configure without taking ownership.
    NativeSocket socket;
    /// @brief Native address family used to select platform options.
    int family;
};

/// @brief Returns the most recent platform socket error code.
/// @return `WSAGetLastError()` on Windows or `errno` on POSIX systems.
[[nodiscard]] int last_socket_error() noexcept;

/// @brief Closes a valid native socket and ignores failures during cleanup.
/// @param[in] socket Native socket whose ownership is being released.
void close_socket(NativeSocket socket) noexcept;

/// @brief Initializes process-wide socket facilities when the platform requires it.
/// @return Success, or a structured initialization error.
[[nodiscard]] Result<void, NetworkError> ensure_socket_runtime();

/// @brief Applies platform-specific listener security and address-reuse settings.
/// @param[in] configuration Borrowed socket and its native address family.
/// @return `true` when all required options were applied.
[[nodiscard]] bool configure_socket_security(SocketConfiguration configuration);

/// @brief Places a socket in nonblocking mode to prevent readiness races.
/// @param[in] socket Borrowed native socket to configure.
/// @return `true` when the socket is nonblocking.
[[nodiscard]] bool configure_socket_nonblocking(NativeSocket socket) noexcept;

/// @brief Reports whether a nonblocking operation has no work currently available.
/// @param[in] error_code Platform socket error code.
/// @return `true` for the platform's would-block condition.
[[nodiscard]] bool socket_error_would_block(int error_code) noexcept;

/// @brief Reports whether a native socket call was interrupted before doing work.
/// @param[in] error_code Platform socket error code.
/// @return `true` when the operation may be retried after interruption.
[[nodiscard]] bool socket_error_interrupted(int error_code) noexcept;

/// @brief Receives at most one bounded span from a nonblocking socket.
/// @param[in] socket Borrowed connected socket.
/// @param[out] buffer Destination storage for received bytes.
/// @return Transferred byte count, or the native failure sentinel.
[[nodiscard]] std::ptrdiff_t receive_socket(NativeSocket socket,
                                            std::span<std::byte> buffer) noexcept;

/// @brief Sends at most one bounded span without allowing SIGPIPE to terminate the process.
/// @param[in] socket Borrowed connected socket.
/// @param[in] buffer Bytes available for transmission.
/// @return Transferred byte count, or the native failure sentinel.
[[nodiscard]] std::ptrdiff_t send_socket(NativeSocket socket,
                                         std::span<const std::byte> buffer) noexcept;

} // namespace sparenode::network::detail
