#pragma once

#include <utility>

#include "sparenode/network/detail/connection_io.hpp"
#include "sparenode/network/detail/native_socket.hpp"
#include "sparenode/network/detail/socket_poller.hpp"
#include "sparenode/network/detail/socket_wait.hpp"
#include "sparenode/network/tcp_connection.hpp"
#include "sparenode/network/tcp_listener.hpp"

namespace sparenode::network
{

/// @brief Owns the platform-specific state of one TcpConnection.
struct TcpConnection::Impl
{
    /// @brief Takes ownership of an accepted socket and records its remote endpoint.
    /// @param[in] socket Accepted socket whose ownership is transferred.
    /// @param[in] endpoint Remote endpoint associated with the socket.
    Impl(const detail::NativeSocket socket, TcpEndpoint endpoint)
        : socket(socket), peer_endpoint(std::move(endpoint)),
          io({.wait = {.socket = socket, .poller = poller, .wake_channel = wake_channel},
              .operations = operations})
    {
    }

    /// @brief Releases the accepted socket when the public connection is destroyed.
    ~Impl()
    {
        detail::close_socket(socket);
    }

    /// @brief Copying is forbidden because the native socket has one owner.
    Impl(const Impl &) = delete;
    /// @brief Copy assignment is forbidden because the native socket has one owner.
    Impl &operator=(const Impl &) = delete;
    /// @brief Moving is forbidden because collaborators retain internal references.
    Impl(Impl &&) = delete;
    /// @brief Move assignment is forbidden because collaborators retain references.
    Impl &operator=(Impl &&) = delete;

    /// @brief Owned connected native socket.
    detail::NativeSocket socket{detail::invalid_socket};
    /// @brief Remote endpoint captured when the connection was accepted.
    TcpEndpoint peer_endpoint;
    /// @brief Native readiness implementation referenced by io.
    detail::NativeSocketPoller poller;
    /// @brief Lazily initialized cancellation channel referenced by io.
    detail::SocketWakeChannel wake_channel;
    /// @brief Native transfer implementation referenced by io.
    detail::NativeSocketOperations operations;
    /// @brief I/O coordinator; declared last because it references preceding members.
    detail::ConnectionIo io;
};

/// @brief Owns the platform-specific state of one TcpListener.
struct TcpListener::Impl
{
    /// @brief Takes ownership of a socket that is already bound and listening.
    /// @param[in] socket Listening socket whose ownership is transferred.
    explicit Impl(const detail::NativeSocket socket) noexcept
        : socket(socket), wait{.socket = socket, .poller = poller, .wake_channel = wake_channel}
    {
    }

    /// @brief Releases the listening socket when the public listener is destroyed.
    ~Impl()
    {
        detail::close_socket(socket);
    }

    /// @brief Copying is forbidden because the native socket has one owner.
    Impl(const Impl &) = delete;
    /// @brief Copy assignment is forbidden because the native socket has one owner.
    Impl &operator=(const Impl &) = delete;
    /// @brief Moving is forbidden because wait retains internal references.
    Impl(Impl &&) = delete;
    /// @brief Move assignment is forbidden because wait retains internal references.
    Impl &operator=(Impl &&) = delete;

    /// @brief Owned listening native socket.
    detail::NativeSocket socket{detail::invalid_socket};
    /// @brief Native readiness implementation referenced by wait.
    detail::NativeSocketPoller poller;
    /// @brief Lazily initialized cancellation channel referenced by wait.
    detail::SocketWakeChannel wake_channel;
    /// @brief Wait context; declared last because it references preceding members.
    detail::SocketWaitContext wait;
};

} // namespace sparenode::network
